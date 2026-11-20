// SPDX-License-Identifier: Apache-2.0
// Native profile preparation refuses missing/outside inputs and safely preserves debugger path operands.
#include "../../src/app/src/unity_native_profile.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
TEST(UnityNativeProfile, RequiresOwnedOriginalInputsAndExplicitTrust) {
    test::ScratchDir root("unity-native-plan"), outside("unity-native-outside");
    auto& fs = io::realFileSystem();
    auto trust = project::TrustStore::open(fs, root.join("trust.json"));
    ASSERT_TRUE(trust);
    UnityIl2cppNativeSelection selection{root.join("Player"), root.join("GameAssembly"), root.join("GameAssembly.pdb"),
                                         root.join("generated"), root.join("build-generated")};
    ASSERT_TRUE(fs.writeFileAtomic(selection.player, "player fixture"));
    ASSERT_TRUE(fs.writeFileAtomic(selection.module, "module fixture"));
    ASSERT_TRUE(fs.writeFileAtomic(selection.symbols, "symbol fixture"));
    ASSERT_TRUE(fs.createDirectories(selection.generatedRoot));
    EXPECT_FALSE(prepareUnityNativeProfile(fs, **trust, root.path(), selection));
    ASSERT_TRUE((*trust)->trust(root.path()));
    auto profile = prepareUnityNativeProfile(fs, **trust, root.path(), selection);
    ASSERT_TRUE(profile) << profile.error().describe();
    EXPECT_EQ(profile->target.program, selection.player);
    EXPECT_TRUE(profile->target.stopOnEntry);
    EXPECT_EQ(profile->arguments.stringAt("type"), "lldb-dap");
    const auto* commands = profile->arguments.find("preRunCommands");
    ASSERT_TRUE(commands);
    EXPECT_EQ(commands->size(), 1u);
    const auto* command = commands->items().front().asString();
    ASSERT_TRUE(command);
    EXPECT_TRUE(command->starts_with("!target modules add --symfile "));
    const auto cpp = root.join("generated/Player.cpp"), cs = root.join("generated/Player.cs");
    ASSERT_TRUE(fs.writeFileAtomic(cpp, "int main() {}"));
    ASSERT_TRUE(fs.writeFileAtomic(cs, "class Player {}"));
    EXPECT_TRUE(isUnityGeneratedSource(fs, selection.generatedRoot, cpp));
    EXPECT_FALSE(isUnityGeneratedSource(fs, selection.generatedRoot, cs));
    selection.buildSourceRoot.clear();
    auto original = prepareUnityNativeProfile(fs, **trust, root.path(), selection);
    ASSERT_TRUE(original) << original.error().describe();
    EXPECT_FALSE(original->target.sourceMap);
    selection.symbols = outside.join("GameAssembly.pdb");
    ASSERT_TRUE(fs.writeFileAtomic(selection.symbols, "outside"));
    EXPECT_FALSE(prepareUnityNativeProfile(fs, **trust, root.path(), selection));
    selection.symbols = root.join("missing.pdb");
    EXPECT_FALSE(prepareUnityNativeProfile(fs, **trust, root.path(), selection));
}
} // namespace
} // namespace frost::app
