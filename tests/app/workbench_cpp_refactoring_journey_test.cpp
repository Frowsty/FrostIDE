// SPDX-License-Identifier: Apache-2.0
// Authored R11 ten-operation real-AST -> Workbench transaction -> Undo/Redo composition.
// This is a C++ component fixture, not evidence that the future engine-corpus journey ran.
#include "frost/document/refactoring_transaction.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/rpc/json.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "clang/pinned_compiler_arguments.hpp"
#include "workbench_rig.hpp"
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include <array>
#include <filesystem>

namespace frost::app::test {
namespace {
using namespace std::chrono_literals;

TEST(WorkbenchCppRefactoringJourney, NamedInlineActionsPreviewApplyCompileAndRefuseObservableIdentity) {
    WorkDir dir("cpp-inline-action"), recovery("cpp-inline-recovery");
    const std::string before =
#ifndef _WIN32
        "#include \"compiler-input.hpp\"\n"
#endif
        "int main(){const int literal=2+3; const int observed=7; "
        "const int* pointer=&observed; return literal+*pointer;}\n";
    dir.write("workspace/main.cpp", before);
#ifndef _WIN32
    constexpr std::string_view header = "// The two targets deliberately have identical contents.\n";
    dir.write("headers/first.hpp", header);
    dir.write("headers/second.hpp", header);
    std::filesystem::create_symlink("first.hpp", dir.join("headers/compiler-input.hpp"));
#endif
    const auto file = dir.join("workspace/main.cpp");
    const auto root = dir.join("workspace");
    const auto compiler = io::normalizePath(FROST_TEST_CXX_COMPILER).value();
#ifdef _WIN32
    std::vector<std::string> arguments{compiler, "/std:c++20", "/EHsc", "/c", file};
#else
    std::vector<std::string> arguments{compiler, "-std=c++20", "-I", dir.join("headers"), "-c", file};
#endif
    frost::test::appendPinnedCompilerArguments(arguments);
    const std::vector<lang::CompileCommand> commands{{root, file, std::move(arguments)}};
    ASSERT_TRUE(lang::writeCompileCommands(io::realFileSystem(), dir.join("workspace/build"), commands));

    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.workbench.refactoringStateDirectory = recovery.join("transactions");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(root));
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(root));
    ASSERT_TRUE(rig.workbench->openPath(file));
    ASSERT_TRUE(waitFor(
        [&] {
            auto* service = rig.workbench->clangd();
            return service && service->state() == lang::ClangdState::Running && service->compileCommands();
        },
        90s));

    QString refusal;
    QTimer dismissWarning;
    QObject::connect(&dismissWarning, &QTimer::timeout, [&] {
        if (auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            refusal = warning->text();
            warning->reject();
        }
    });
    dismissWarning.start(1);
    const auto moveCaret = [&](std::string_view name) {
        rig.view()->widget()->setFocus();
        rig.view()->editor().setState(text::EditorState::caretAt(ByteOffset{before.find(name)}));
    };
    const auto awaitPreview = [&] {
        QDialogButtonBox* preview = nullptr;
        const bool appeared = waitFor(
            [&] {
                for (auto* box : rig.window->findChildren<QDialogButtonBox*>())
                    if (box->isVisible() && box->button(QDialogButtonBox::Apply))
                        preview = box;
                return preview || !refusal.isEmpty();
            },
            120s);
        EXPECT_TRUE(appeared);
        return preview;
    };
    const input::ActionContext context{.keys = input::ContextKeys{input::ContextKey::EditorFocus}, .trusted = true};
    for (const auto* id : {"refactor.inline", "refactor.cpp.inlineVariable"}) {
        SCOPED_TRACE(id);
        const auto action = rig.workbench->registry().find(id);
        ASSERT_TRUE(action);
        moveCaret("literal");
        ASSERT_EQ(rig.workbench->registry().tryInvoke(*action, context), input::InvokeResult::Ran);
        auto* preview = awaitPreview();
        ASSERT_NE(preview, nullptr) << refusal.toStdString();
        auto* dialog = preview->parentWidget();
        ASSERT_NE(dialog, nullptr);
        auto* image = dialog->findChild<QPlainTextEdit*>();
        ASSERT_NE(image, nullptr);
        const auto previewText = image->toPlainText();
        EXPECT_TRUE(previewText.contains(QStringLiteral("BEFORE\n") + QString::fromStdString(before)));
        EXPECT_TRUE(previewText.contains(QStringLiteral("AFTER\n")));
        const auto afterPreview = previewText.section(QStringLiteral("AFTER\n"), 1);
        EXPECT_TRUE(afterPreview.contains(QStringLiteral("2+3")));
        EXPECT_FALSE(afterPreview.contains(QStringLiteral("literal")));
        EXPECT_EQ(rig.view()->editor().document().text().text(), before);
        EXPECT_EQ(dir.read("workspace/main.cpp"), before);
        preview->button(QDialogButtonBox::Apply)->click();
        ASSERT_TRUE(refusal.isEmpty()) << refusal.toStdString();
        ASSERT_TRUE(rig.workbench->canUndoRefactoring());

        const auto after = rig.view()->editor().document().text().text();
        EXPECT_EQ(after.find("literal"), std::string::npos);
        EXPECT_NE(after.find("2+3"), std::string::npos);
        EXPECT_NE(after.find("&observed"), std::string::npos);
        dir.write("workspace/compiled.cpp", after);
        io::ProcessSpec compile;
        compile.executable = compiler;
        compile.workingDirectory = root;
#ifdef _WIN32
        compile.arguments = {"/std:c++20", "/EHsc", "/Zs", dir.join("workspace/compiled.cpp")};
#else
        compile.arguments = {"-std=c++20", "-I", dir.join("headers"), "-fsyntax-only",
                             dir.join("workspace/compiled.cpp")};
#endif
        frost::test::appendPinnedCompilerArguments(compile.arguments);
        const auto result = io::runToCompletion(io::realProcessLauncher(), compile, {}, 30s);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->exitCode, 0) << result->stderrBytes;
        ASSERT_TRUE(rig.workbench->undoRefactoring());
        EXPECT_EQ(rig.view()->editor().document().text().text(), before);
    }

    const auto action = rig.workbench->registry().find("refactor.inline");
    ASSERT_TRUE(action);
#ifndef _WIN32
    moveCaret("literal");
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*action, context), input::InvokeResult::Ran);
    auto* preview = awaitPreview();
    ASSERT_NE(preview, nullptr) << refusal.toStdString();
    std::filesystem::create_symlink("second.hpp", dir.join("headers/retargeted.hpp"));
    std::filesystem::rename(dir.join("headers/retargeted.hpp"), dir.join("headers/compiler-input.hpp"));
    preview->button(QDialogButtonBox::Apply)->click();
    ASSERT_TRUE(waitFor([&] { return !refusal.isEmpty(); }, 10s));
    EXPECT_TRUE(refusal.contains(QStringLiteral("Compiler input target changed"))) << refusal.toStdString();
    EXPECT_EQ(rig.view()->editor().document().text().text(), before);
    EXPECT_EQ(dir.read("workspace/main.cpp"), before);
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    refusal.clear();
#endif
    moveCaret("observed");
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*action, context), input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return !refusal.isEmpty(); }, 120s));
    EXPECT_TRUE(refusal.contains(QStringLiteral("Variable identity/address is observable"))) << refusal.toStdString();
    EXPECT_EQ(rig.view()->editor().document().text().text(), before);
    EXPECT_EQ(dir.read("workspace/main.cpp"), before);
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
}

TEST(WorkbenchCppRefactoringJourney, TenRealAstOperationsHaveWholeOperationUndoAndRedo) {
    WorkDir dir("cpp-refactoring-journey");
    std::string source = "struct Health { int value; };\n"
                         "static int difference(int left,int right){return left-right;}\n"
                         "static int twice(int value){return value+value;}\n"
                         "static int obsolete(int value){return value;}\n"
                         "static int baseHealth(){return 2+3;}\n"
                         "static int bonusHealth(){return 4+5;}\n"
                         "static int maximumHealth(){return 6+7;}\n"
                         "int main(){int n=4;const int literal=3;return "
                         "difference(1,2)+twice(n)+literal+baseHealth()+bonusHealth()+maximumHealth();}\n";
    const auto initial = source;
    dir.write("workspace/component.cpp", source);
    const auto file = dir.join("workspace/component.cpp");
    const auto root = dir.join("workspace");
    const auto compiler = io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    using rpc::Json;
    std::vector<Json> arguments{Json(compiler)};
#ifdef _WIN32
    for (const auto& argument : std::vector<std::string>{"/std:c++20", "/c", file})
        arguments.emplace_back(argument);
#else
    for (const auto& argument : std::vector<std::string>{"-std=c++20", "-c", file})
        arguments.emplace_back(argument);
#endif
    frost::test::appendPinnedCompilerArguments(arguments);
    const auto database = Json::array({Json::object()
                                           .set("directory", root)
                                           .set("file", file)
                                           .set("arguments", Json::array(std::move(arguments)))})
                              .dump();
    RigOptions options;
    options.workbench.refactoringStateDirectory = dir.join("private-refactoring");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(root));
    struct Step {
        const char* operation;
        const char* selection;
    };
    const std::array steps{Step{"change-signature", "difference(int"},
                           Step{"inline-function", "twice(n)"},
                           Step{"inline-variable", "literal=3"},
                           Step{"extract-variable", "2+3"},
                           Step{"extract-constant", "4+5"},
                           Step{"extract-method", "6+7"},
                           Step{"safe-delete", "obsolete(int"},
                           Step{"generate-accessors", "Health"},
                           Step{"generate-equality", "Health"},
                           Step{"generate-formatting", "Health"}};
    std::vector<std::string> images{source};
    for (const auto& step : steps) {
        SCOPED_TRACE(step.operation);
        const auto offset = source.find(step.selection);
        ASSERT_NE(offset, std::string::npos);
        const auto request = Json::object()
                                 .set("projectRoot", root)
                                 .set("seedTranslationUnit", file)
                                 .set("path", file)
                                 .set("offset", offset)
                                 .set("endOffset", offset + std::string_view(step.selection).size())
                                 .set("operation", step.operation)
                                 .set("newName", "extracted" + std::to_string(images.size()))
                                 .set("parameterOrder", Json::array({Json(1), Json(0)}))
                                 .set("compilationDatabase", database)
                                 .set("overlays", Json::array());
        io::ProcessSpec spec;
        spec.executable = io::normalizePath(FROST_CLANG_BINARY).value();
        spec.arguments = {"--plan-ast-refactoring"};
        spec.workingDirectory = root;
        spec.stdinMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        spec.environment.emplace_back("FROST_LLVM_ROOT",
                                      std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto process = io::runToCompletion(io::realProcessLauncher(), spec, request.dump(), std::chrono::minutes(5));
        ASSERT_TRUE(process);
        ASSERT_EQ(process->exitCode, 0) << process->stdoutBytes << process->stderrBytes;
        auto proposal = rpc::parseJson(process->stdoutBytes);
        ASSERT_TRUE(proposal);
        ASSERT_EQ(proposal->stringAt("status"), "ready");
        ASSERT_EQ(proposal->find("files")->items().size(), 1u);
        const auto& edit = proposal->find("files")->items().front();
        ASSERT_EQ(edit.stringAt("path"), file);
        ASSERT_EQ(edit.stringAt("before"), source);
        document::RefactoringPlan plan;
        plan.label = step.operation;
        plan.disk = {{file, source, std::string(edit.stringAt("after"))}};
        ASSERT_TRUE(rig.workbench->applyRefactoring(plan));
        source = std::string(edit.stringAt("after"));
        EXPECT_EQ(dir.read("workspace/component.cpp"), source);
        images.push_back(source);
    }
    for (std::size_t index = steps.size(); index != 0; --index) {
        ASSERT_TRUE(rig.workbench->undoRefactoring());
        EXPECT_EQ(dir.read("workspace/component.cpp"), images[index - 1]);
    }
    EXPECT_EQ(dir.read("workspace/component.cpp"), initial);
    for (std::size_t index = 1; index < images.size(); ++index) {
        ASSERT_TRUE(rig.workbench->redoRefactoring());
        EXPECT_EQ(dir.read("workspace/component.cpp"), images[index]);
    }
    EXPECT_EQ(dir.read("workspace/component.cpp"), source);
}
} // namespace
} // namespace frost::app::test
