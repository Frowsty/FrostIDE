// SPDX-License-Identifier: Apache-2.0
// Pinned CMake FileAPI/configure/build oracle for pure source-rename proposals; fixture writes only.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cmake_membership.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace frost::project {
namespace {
using namespace std::chrono_literals;

TEST(CMakeMembershipOracle, RenamedLiteralReconfiguresBuildsAndFixtureInverseRestoresBytes) {
    test::ScratchDir scratch("cmake-membership-rename");
    auto& fs = io::realFileSystem();
    const auto root = scratch.path();
    ASSERT_FALSE(root.empty());
    auto child = [&](std::string_view name) { return *io::joinPath(root, name); };
    const auto build = child("build");
    const auto definition = child("CMakeLists.txt");
    const auto oldSource = child("old source.cpp");
    const auto newSource = child("new source.cpp");
    const std::string original = "cmake_minimum_required(VERSION 3.20)\nproject(Membership LANGUAGES CXX)\n"
                                 "add_library(core STATIC base.cpp \"old source.cpp\") # keep\n";
    const std::string sourceBytes = "int renamed_member() { return 17; }\n";
    ASSERT_TRUE(fs.writeFileAtomic(definition, original));
    ASSERT_TRUE(fs.writeFileAtomic(oldSource, sourceBytes));
    ASSERT_TRUE(fs.writeFileAtomic(child("base.cpp"), "int base_member() { return 3; }\n"));
    ASSERT_TRUE(writeCMakeFileApiQuery(fs, build));
    auto run = [&](std::vector<std::string> arguments) {
        io::ProcessSpec spec;
        spec.executable = FROST_TEST_CMAKE;
        spec.arguments = std::move(arguments);
        spec.workingDirectory = root;
        return io::runToCompletion(io::realProcessLauncher(), spec, {}, 5min);
    };
    auto configure = [&] {
        return run({"-S", root, "-B", build, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" FROST_TEST_NINJA,
                    "-DCMAKE_CXX_COMPILER=" FROST_TEST_CXX_COMPILER, "-DCMAKE_BUILD_TYPE=Debug"});
    };
    auto configured = configure();
    ASSERT_TRUE(configured) << configured.error().describe();
    ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
    auto loaded = readCMakeFileApiReply(fs, build);
    ASSERT_TRUE(loaded && loaded->has_value());
    const auto model = **loaded;
    const auto* target = model.findTarget("core");
    ASSERT_NE(target, nullptr);
    const auto targetIndex = static_cast<std::size_t>(target - model.targets.data());
    std::vector<CMakeInputSnapshot> inputs;
    for (const auto& input : model.inputs) {
        if (input.cmake)
            continue;
        auto bytes = fs.readFile(input.path);
        ASSERT_TRUE(bytes) << input.path;
        inputs.push_back({input.path, *bytes});
    }
    auto proposal = planCMakeSourceRename(model, targetIndex, oldSource, newSource, definition, inputs);
    ASSERT_TRUE(proposal) << proposal.error().describe();
    EXPECT_EQ(*fs.readFile(definition), original); // Product planner has not written anything.
    EXPECT_EQ(proposal->original, original);
    // Test harness applies the two proposed effects only inside its owned disposable fixture.
    // This is a CMake semantic oracle, not transaction/Apply/global Undo acceptance.
    ASSERT_TRUE(fs.rename(oldSource, newSource));
    ASSERT_TRUE(fs.writeFileAtomic(definition, proposal->replacement));
    configured = configure();
    ASSERT_TRUE(configured);
    ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
    auto changed = readCMakeFileApiReply(fs, build);
    ASSERT_TRUE(changed && changed->has_value());
    const auto* changedTarget = (**changed).findTarget("core");
    ASSERT_NE(changedTarget, nullptr);
    auto count = [](const CMakeTarget& t, const std::string& path) {
        return std::ranges::count_if(t.sources, [&](const auto& source) { return source.path == path; });
    };
    ASSERT_EQ(count(*target, oldSource), 1);
    EXPECT_EQ(count(*changedTarget, oldSource), 0);
    EXPECT_EQ(count(*changedTarget, newSource), count(*target, oldSource));
    EXPECT_EQ(changedTarget->sources.size(), target->sources.size());
    auto built = run({"--build", build, "--target", "core", "--parallel", "1"});
    ASSERT_TRUE(built);
    ASSERT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
    ASSERT_TRUE(fs.rename(newSource, oldSource));
    ASSERT_TRUE(fs.writeFileAtomic(definition, proposal->original));
    EXPECT_EQ(*fs.readFile(definition), original);
    EXPECT_EQ(*fs.readFile(oldSource), sourceBytes);
    configured = configure();
    ASSERT_TRUE(configured);
    ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
    auto restored = readCMakeFileApiReply(fs, build);
    ASSERT_TRUE(restored && restored->has_value());
    const auto* restoredTarget = (**restored).findTarget("core");
    ASSERT_NE(restoredTarget, nullptr);
    EXPECT_EQ(count(*restoredTarget, oldSource), count(*target, oldSource));
    EXPECT_EQ(count(*restoredTarget, newSource), 0);
    built = run({"--build", build, "--target", "core", "--parallel", "1"});
    ASSERT_TRUE(built);
    EXPECT_EQ(built->exitCode, 0) << built->stdoutBytes << built->stderrBytes;
}
} // namespace
} // namespace frost::project
