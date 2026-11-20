// SPDX-License-Identifier: Apache-2.0
// Checks that the native Unreal Editor run configuration preserves the project argument.
#include "frost/run/unreal_run.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
namespace frost::run {
TEST(UnrealRun, UsesExistingNativeRunTypeWithLiteralProjectArgument) {
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic";
    UnrealEditorRunRequest input{"unreal.editor", "Synthetic Editor", io::joinPath(root, "Tools/UnrealEditor").value(), io::joinPath(root, "Game With Spaces/Oracle.uproject").value(), {"-log"}};
    auto result = unrealEditorRunConfiguration(input);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->type, RunType::Native);
    EXPECT_EQ(result->program, input.editor);
    EXPECT_EQ(result->arguments, (std::vector<std::string>{input.projectFile, "-log"}));
    EXPECT_EQ(result->workingDirectory, io::parentPath(input.projectFile));
    EXPECT_TRUE(result->beforeLaunchTasks.empty());
    input.editor = "UnrealEditor";
    EXPECT_FALSE(unrealEditorRunConfiguration(input));
}
} // namespace frost::run
