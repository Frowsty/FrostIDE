// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/unreal_debug.hpp"
#include "frost/io/path.hpp"
#include <gtest/gtest.h>
namespace frost::debug {
TEST(UnrealDebug, ProducesNativeTargetWithExplicitProjectTrustRoot) {
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic";
    const auto editor = io::joinPath(root, "Tools/UnrealEditor").value();
    const auto project = io::joinPath(root, "Game With Spaces/Oracle.uproject").value();
    auto target = unrealEditorDebugTarget(editor, project, {"-log"});
    ASSERT_TRUE(target);
    EXPECT_EQ(target->program, editor);
    EXPECT_EQ(target->arguments, (std::vector<std::string>{project, "-log"}));
    EXPECT_EQ(target->trustRoots, (std::vector<std::string>{std::string(io::parentPath(project))}));
    EXPECT_FALSE(unrealEditorDebugTarget("relative", project));
}
} // namespace frost::debug
