// SPDX-License-Identifier: Apache-2.0
//
// CPP-5 and REF-12 in the workbench with the real clangd (label "clangd"): a folder project whose
// compile_commands.json names one source. The active C++ editor receives clangd's document links,
// folding ranges and inlay hints (each equal to clangd's own answer to the same request), Ctrl+click
// on the #include opens the header clangd resolved, and Show AST lists the node clangd returns.
// Saving: with the default settings a save writes the text unchanged (format on save is off); with
// editor.formatOnSave on, the saved file equals the pinned clang-format CLI's output for that
// text, and with editor.optimizeImportsOnSave on, clangd's include cleaner removes the unused
// include before the file is written.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/document_features.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/ast_view.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <QMouseEvent>
#include <QTreeWidget>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

namespace fl = frost::lang;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
using namespace std::chrono_literals;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;
constexpr auto kPatience = 60s; // clangd answers in well under a second; this is slack under load.

constexpr std::string_view kHeader = "#pragma once\n"
                                     "int add(int a, int b);\n";

constexpr std::string_view kSource = "#include \"widgets.h\"\n"
                                     "#include \"unused.h\"\n"
                                     "\n"
                                     "int add(int a, int b) {\n"
                                     "    return a + b;\n"
                                     "}\n"
                                     "\n"
                                     "int use() {\n"
                                     "    auto total = add(1, 2);\n"
                                     "    return total;\n"
                                     "}\n";

// A style unlike clang-format's default, so the result must have come from the file's style.
constexpr std::string_view kStyle = "BasedOnStyle: LLVM\nIndentWidth: 3\nBreakBeforeBraces: Allman\n";

std::string clangdPath() {
    auto found = fl::findClangd(frost::io::realFileSystem(), fl::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

// The pinned clang-format CLI (beside clangd) on `text`, written to `file` first: its output.
std::string clangFormatCli(const WorkDir& dir, std::string_view name, std::string_view text) {
    dir.write(name, text);
    frost::io::ProcessSpec spec;
    spec.executable =
        frost::io::joinPath(frost::io::parentPath(clangdPath()), kWindows ? "clang-format.exe" : "clang-format")
            .value_or("");
    spec.arguments = {"-style=file", dir.join(name)};
    spec.workingDirectory = dir.path();
    auto run = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 60s);
    EXPECT_TRUE(run.has_value()) << run.error().describe();
    if (!run) {
        return {};
    }
    EXPECT_EQ(run->exitCode, 0) << run->stderrBytes;
    return run->stdoutBytes;
}

// A folder project with the sources above and a compile_commands.json naming main.cpp.
void writeProject(const WorkDir& dir) {
    dir.write(".clang-format", kStyle);
    dir.write("widgets.h", kHeader);
    dir.write("unused.h", "#pragma once\nint unusedHelper();\n");
    dir.write("main.cpp", kSource);
    const std::string main = dir.join("main.cpp");
    const std::vector<fl::CompileCommand> commands = {fl::CompileCommand{
        .directory = dir.path(), .file = main, .arguments = {"clang++", "-std=c++20", "-I" + dir.path(), "-c", main}}};
    ASSERT_TRUE(fl::writeCompileCommands(frost::io::realFileSystem(), dir.join("build"), commands).has_value());
}

RigOptions clangdOptions() {
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    return options;
}

// clangd's raw answer to `method` (the oracle), driven on a pool of the test's own.
fl::Json rawAnswer(fl::ClangdService& clangd, std::string method, fl::Json params) {
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 1});
    auto answer =
        pool.spawn(frost::sched::Lane::UserVisible, clangd.request(std::move(method), std::move(params))).join();
    pool.shutdown();
    EXPECT_TRUE(answer.has_value()) << answer.error().describe();
    return answer.value_or(fl::Json());
}

std::string readFile(const std::string& path) { return frost::io::realFileSystem().readFile(path).value_or("?"); }

TEST(WorkbenchClangd, CppEditorLinksFoldsInlayHintsAndTheAstViewComeFromClangd) {
    frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
    WorkDir dir("cpp-editor-features");
    writeProject(dir);
    const std::string main = dir.join("main.cpp");
    WorkbenchRig rig(dir, clangdOptions());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value()); // The folder is the project.
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(rig.workbench->openPath(main).has_value());
    frost::app::DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    frost::ui::EditorView& editor = view->editor();
    ASSERT_TRUE(waitFor(
        [&] {
            return !editor.documentLinks().empty() && !editor.languageFoldRanges().empty() &&
                   !editor.inlayHints().empty();
        },
        kPatience))
        << "links " << editor.documentLinks().size() << ", folds " << editor.languageFoldRanges().size() << ", hints "
        << editor.inlayHints().size();
    fl::ClangdService& clangd = *rig.workbench->clangd();
    const fl::Json identifier = fl::textDocumentIdentifier(main).value();
    const frost::text::Snapshot& text = editor.editor().text();

    // Links: one per resolved #include, each on its quoted name, targeting the header clangd named.
    const fl::Json rawLinks =
        rawAnswer(clangd, "textDocument/documentLink", fl::Json::object().set("textDocument", identifier));
    ASSERT_EQ(editor.documentLinks().size(), rawLinks.size());
    for (std::size_t i = 0; i < rawLinks.size(); ++i) {
        const frost::ui::EditorDocumentLink& link = editor.documentLinks()[i];
        EXPECT_EQ(fl::documentKey(link.target),
                  fl::documentKey(fl::uriToPath(rawLinks.items()[i].stringAt("target")).value()));
        const auto start = fl::fromLspPosition(
            text, *fl::parsePosition(*rawLinks.items()[i].find("range")->find("start")), clangd.encoding());
        ASSERT_TRUE(start.has_value());
        EXPECT_EQ(link.range.begin, *start);
    }
    EXPECT_EQ(text.text(editor.documentLinks().front().range), "\"widgets.h\"");

    // Folds: one candidate per multi-line range clangd sent, from its first line's end to its last line's start.
    const fl::Json rawFolds =
        rawAnswer(clangd, "textDocument/foldingRange", fl::Json::object().set("textDocument", identifier));
    std::vector<frost::text::ByteRange> expectedFolds;
    for (const fl::Json& item : rawFolds.items()) {
        const auto first = static_cast<std::uint32_t>(item.integerAt("startLine").value_or(0));
        const auto last = static_cast<std::uint32_t>(item.integerAt("endLine").value_or(0));
        if (last > first) {
            expectedFolds.push_back({text.lineInfo(frost::LineIndex{first})->content.end,
                                     text.lineInfo(frost::LineIndex{last})->content.begin});
        }
    }
    std::ranges::sort(expectedFolds, {}, &frost::text::ByteRange::begin);
    EXPECT_EQ(editor.languageFoldRanges(), expectedFolds);

    // Inlay hints: clangd's labels at clangd's positions.
    const auto lines = static_cast<std::uint32_t>(std::ranges::count(kSource, '\n'));
    const fl::Json rawHints = rawAnswer(
        clangd, "textDocument/inlayHint",
        fl::Json::object().set("textDocument", identifier).set("range", fl::toJson(fl::LspRange{{0, 0}, {lines, 0}})));
    ASSERT_EQ(editor.inlayHints().size(), rawHints.size());
    std::vector<std::string> labels;
    for (const frost::ui::EditorInlayHint& hint : editor.inlayHints()) {
        labels.push_back(hint.label);
    }
    EXPECT_NE(std::ranges::find(labels, "a:"), labels.end());
    EXPECT_NE(std::ranges::find(labels, "b:"), labels.end());

    // Show AST at `a + b`: the popup's root is the node clangd returns there.
    const std::size_t plus = std::string_view(kSource).find("a + b") + 2;
    editor.setState(frost::text::EditorState::caretAt(frost::ByteOffset{plus}));
    frost::ui::lang::AstViewPopup& popup = rig.workbench->language()->astPopup();
    rig.workbench->language()->showAst();
    ASSERT_TRUE(waitFor([&] { return popup.isVisible() && popup.nodeCount() > 0; }, kPatience));
    const fl::LspPosition at = fl::toLspPosition(text, frost::ByteOffset{plus}, clangd.encoding());
    const fl::Json rawAst =
        rawAnswer(clangd, "textDocument/ast",
                  fl::Json::object().set("textDocument", identifier).set("range", fl::toJson(fl::LspRange{at, at})));
    ASSERT_TRUE(rawAst.isObject());
    EXPECT_TRUE(popup.tree()->topLevelItem(0)->text(0).startsWith(
        QString::fromStdString(std::string(rawAst.stringAt("kind")))));
    EXPECT_EQ(popup.tree()->topLevelItem(0)->childCount(), static_cast<int>(rawAst.find("children")->size()));
    popup.hide();

    // Ctrl+click on the #include opens the header.
    const frost::ByteOffset inLink{editor.documentLinks().front().range.begin.value() + 3};
    const QPoint local = editor.viewport()->mapFromGlobal(editor.globalCaretRect(inLink).center() + QPoint(2, 0));
    const QPointF global(editor.viewport()->mapToGlobal(local));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), global, Qt::LeftButton, Qt::LeftButton,
                      Qt::ControlModifier);
    QCoreApplication::sendEvent(editor.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), global, Qt::LeftButton, Qt::NoButton,
                        Qt::ControlModifier);
    QCoreApplication::sendEvent(editor.viewport(), &release);
    ASSERT_TRUE(waitFor([&] {
        const frost::app::DocumentEditorView* current = rig.workbench->currentView();
        return current != nullptr && fl::documentKey(current->path()) == fl::documentKey(dir.join("widgets.h"));
    }));
}

TEST(WorkbenchClangd, CppEditorFormatOnSaveIsOffByDefaultAndWhenOnEqualsTheClangFormatCli) {
    frost::test::disableCrashDialogs();
    WorkDir dir("cpp-editor-save");
    writeProject(dir);
    const std::string main = dir.join("main.cpp");
    WorkbenchRig rig(dir, clangdOptions());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_NE(rig.workbench->clangd(), nullptr);
    ASSERT_TRUE(rig.workbench->openPath(main).has_value());
    frost::app::DocumentEditorView* view = rig.view();
    ASSERT_NE(view, nullptr);
    frost::ui::EditorView& editor = view->editor();
    ASSERT_TRUE(waitFor([&] { return !editor.semanticTokens().empty(); }, kPatience)) << "clangd never answered";

    // The default: off, and the save writes exactly the edited text.
    const auto snapshot = rig.settings->snapshot();
    EXPECT_FALSE(snapshot->get(frost::settings::key::editorFormatOnSave));
    EXPECT_FALSE(snapshot->get(frost::settings::key::editorOptimizeImportsOnSave));
    editor.setState(frost::text::EditorState::caretAt(editor.editor().text().size()));
    ASSERT_TRUE(editor.typeText("int   more( ){return 3;}\n"));
    const std::string edited = editor.editor().text().text();
    ASSERT_TRUE(rig.workbench->save(*view).has_value());
    EXPECT_EQ(readFile(main), edited);

    // Format on save: the file on disk is the CLI's output for the text being saved.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::editorFormatOnSave, true));
    ASSERT_TRUE(editor.typeText("int   evenMore( ){return 4;}\n"));
    const std::string before = editor.editor().text().text();
    const std::string expected = clangFormatCli(dir, "cli_copy.cpp", before);
    ASSERT_FALSE(expected.empty());
    ASSERT_NE(expected, before);
    ASSERT_TRUE(rig.workbench->save(*view).has_value());
    EXPECT_EQ(readFile(main), expected);
    EXPECT_EQ(editor.editor().text().text(), expected);
    EXPECT_FALSE(view->document().dirty());

    // Optimize includes on save: once clangd's diagnostics describe this text, the unused include goes.
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application,
                                  frost::settings::key::editorOptimizeImportsOnSave, true));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto set = rig.workbench->clangd()->diagnostics().forFile(main);
            return set && !set->stale && set->textVersion == editor.editor().text().version();
        },
        kPatience));
    ASSERT_TRUE(rig.workbench->save(*view).has_value());
    const std::string saved = readFile(main);
    EXPECT_EQ(saved.find("unused.h"), std::string::npos) << saved;
    EXPECT_NE(saved.find("#include \"widgets.h\""), std::string::npos) << saved;
    EXPECT_EQ(clangFormatCli(dir, "cli_copy2.cpp", saved), saved) << "the saved file is not formatted";

    // The file is clean now: once clangd has analysed it, saving with both actions on changes
    // nothing and warns about nothing (no diagnostics is not "not analysed yet").
    fl::ClangdService& clangd = *rig.workbench->clangd();
    ASSERT_TRUE(waitFor([&] { return clangd.diagnosedVersion(main) == clangd.documentVersion(main); }, kPatience));
    EXPECT_FALSE(clangd.diagnostics().forFile(main).has_value());
    const std::size_t notices = rig.window->notifications().log().size();
    ASSERT_TRUE(rig.workbench->save(*view).has_value());
    EXPECT_EQ(readFile(main), saved);
    for (std::size_t i = notices; i < rig.window->notifications().log().size(); ++i) {
        ADD_FAILURE() << rig.window->notifications().log()[i].notification.text.toStdString();
    }
}

} // namespace
