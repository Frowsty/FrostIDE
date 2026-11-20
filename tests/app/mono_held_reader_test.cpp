// SPDX-License-Identifier: Apache-2.0
// Actual original held-reader helper capture and no-link boundaries; execution is deferred by policy.
#include "../../src/app/src/mono_held_capture.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
#include <chrono>
#ifdef __linux__
#include <unistd.h>
#endif
namespace frost::app {
namespace {
TEST(MonoHeldReader, CapturesOnlyBoundedOriginalBytes) {
    test::ScratchDir root("mono-held-reader");
    const auto path = root.join("source.cs");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(path, "class C {}"));
    auto bytes = captureMonoHeldInput(io::realProcessLauncher(), FROST_HELD_READER, root.path(), path,
        1024, std::chrono::seconds(5), {}, [] { return true; });
    ASSERT_TRUE(bytes) << bytes.error().describe();
    EXPECT_EQ(*bytes, "class C {}");
    EXPECT_FALSE(captureMonoHeldInput(io::realProcessLauncher(), FROST_HELD_READER, root.path(), path,
        1, std::chrono::seconds(5), {}, [] { return true; }));
    EXPECT_FALSE(captureMonoHeldInput(io::realProcessLauncher(), FROST_HELD_READER, root.path(), path,
        1024, std::chrono::seconds(5), {}, [] { return false; }));
}
#ifdef __linux__
TEST(MonoHeldReader, RefusesSymlinkAncestryAndHardlinkedSources) {
    test::ScratchDir root("mono-held-links");
    const auto original = root.join("source.cs"), symbolic = root.join("symbol.cs"), hard = root.join("hard.cs");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(original, "original"));
    ASSERT_EQ(::symlink(original.c_str(), symbolic.c_str()), 0);
    auto capture = [&](const std::string& path) { return captureMonoHeldInput(io::realProcessLauncher(),
        FROST_HELD_READER, root.path(), path, 1024, std::chrono::seconds(5), {}, [] { return true; }); };
    EXPECT_FALSE(capture(symbolic));
    ASSERT_EQ(::link(original.c_str(), hard.c_str()), 0);
    EXPECT_FALSE(capture(hard));
    EXPECT_FALSE(capture(original));
}
#endif
} // namespace
} // namespace frost::app
