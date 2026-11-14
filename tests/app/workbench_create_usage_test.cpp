// SPDX-License-Identifier: Apache-2.0
// Future real native recovered-AST -> preview plan -> atomic header Apply/Undo integration.
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/cpp_refactoring_plan.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include "support/crash_dialogs.hpp"
#include "workbench_rig.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
namespace frost::app::test {
namespace {
using namespace std::chrono_literals;
TEST(WorkbenchCreateUsage, OriginalUnresolvedCallProducesActualHeaderPlanWithStaleAdmissionAndAtomicUndo) {
    frost::test::disableCrashDialogs();
    WorkDir dir("native-usage"), recovery("native-usage-recovery");
    const std::string sourceBefore =
        "#include \"box.hpp\"\nint main(){Box box; int value=3; return box.read(value);}\n";
    const std::string headerBefore = "struct Box {};\n";
    dir.write("src/main.cpp", sourceBefore);
    dir.write("src/box.hpp", headerBefore);
    const auto source = dir.join("src/main.cpp"), header = dir.join("src/box.hpp");
    ASSERT_TRUE(lang::writeCompileCommands(
        io::realFileSystem(), dir.join("build"),
        std::vector<lang::CompileCommand>{{dir.path(), source, {"clang++", "-std=c++20", "-c", source}}}));
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.workbench.refactoringStateDirectory = recovery.join("transactions");
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(source));
    ASSERT_TRUE(waitFor(
        [&] { return rig.workbench->clangd() && rig.workbench->clangd()->compileCommands().has_value(); }, 60s));
    const auto action = rig.workbench->registry().find("code.createFromUsage");
    ASSERT_TRUE(action);
    EXPECT_TRUE(rig.workbench->registry().info(*action).hasHandler);
    using Plan = document::RefactoringPlan;
    auto prepare = [&] {
        auto state = std::make_shared<std::optional<Result<Plan>>>();
        lang::CppRefactoringRequest request;
        request.operation = "create-from-usage";
        request.context.path = source;
        request.context.offset = sourceBefore.find("read");
        request.endOffset = request.context.offset + 4;
        request.context.newName = "unused";
        request.usageReturnType = "int";
        request.usageBody = "return value;";
        rig.workbench->prepareCppRefactoring(request, {},
                                             [state](Result<Plan> value) { state->emplace(std::move(value)); });
        return state;
    };
    auto first = prepare();
    ASSERT_TRUE(waitFor([&] { return first->has_value(); }, 120s));
    ASSERT_TRUE(first->value()) << first->value().error().describe();
    ASSERT_EQ(first->value()->disk.size(), 1U);
    EXPECT_EQ(first->value()->disk.front().path, header);
    EXPECT_TRUE(first->value()->buffers.empty());
    EXPECT_EQ(dir.read("src/box.hpp"), headerBefore);
    EXPECT_EQ(rig.view()->editor().document().text().text(), sourceBefore);
    dir.write("src/box.hpp", "// external change\n" + headerBefore);
    EXPECT_FALSE(rig.workbench->applyRefactoring(*first->value()));
    EXPECT_FALSE(rig.workbench->canUndoRefactoring());
    auto second = prepare();
    ASSERT_TRUE(waitFor([&] { return second->has_value(); }, 120s));
    ASSERT_TRUE(second->value()) << second->value().error().describe();
    auto applied = rig.workbench->applyRefactoring(*second->value());
    ASSERT_TRUE(applied) << applied.error().describe();
    EXPECT_EQ(applied->status, document::RefactoringStatus::Applied);
    EXPECT_NE(dir.read("src/box.hpp").find("int read(int value)"), std::string::npos);
    EXPECT_EQ(rig.view()->editor().document().text().text(), sourceBefore);
    auto undone = rig.workbench->undoRefactoring();
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(dir.read("src/box.hpp"), "// external change\n" + headerBefore);
}
} // namespace
} // namespace frost::app::test
