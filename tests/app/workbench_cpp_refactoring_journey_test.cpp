// SPDX-License-Identifier: Apache-2.0
// Authored R11 ten-operation real-AST -> Workbench transaction -> Undo/Redo composition.
// This is a C++ component fixture, not evidence that the future engine-corpus journey ran.
#include "frost/document/refactoring_transaction.hpp"
#include "frost/rpc/json.hpp"

#include "workbench_rig.hpp"

#include <array>
#include <filesystem>

namespace frost::app::test {
namespace {
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
