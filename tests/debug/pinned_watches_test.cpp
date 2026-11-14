// SPDX-License-Identifier: Apache-2.0
#include "frost/core/clock.hpp"
#include "frost/debug/pinned_watches.hpp"
#include "frost/io/mem_fs.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>
namespace frost::debug {
TEST(PinnedWatches, RoundTripPreservesUnrelatedPersonalSettingsAndNeverPersistsReferences) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("WatchWorkspace");
    const auto path = test::workPath("WatchWorkspace/.frost/workspace.json");
    ASSERT_TRUE(fs.createDirectories(test::workPath("WatchWorkspace/.frost")));
    ASSERT_TRUE(fs.writeFileAtomic(
        path, "{\n // personal comment\n \"schemaVersion\": 1, \"breakpoints\": [], \"theme\": \"dark\"\n}\n"));
    ASSERT_TRUE(savePinnedWatches(fs, root, {"obj.count", "values[7]"}));
    auto reopened = loadPinnedWatches(fs, root);
    ASSERT_TRUE(reopened);
    EXPECT_EQ(*reopened, (std::vector<std::string>{"obj.count", "values[7]"}));
    auto bytes = fs.readFile(path);
    ASSERT_TRUE(bytes);
    EXPECT_NE(bytes->find("// personal comment"), std::string::npos);
    EXPECT_NE(bytes->find("\"theme\": \"dark\""), std::string::npos);
    EXPECT_EQ(bytes->find("variablesReference"), std::string::npos);
    EXPECT_EQ(bytes->find("dataId"), std::string::npos);
    ASSERT_TRUE(savePinnedWatches(fs, root, {}));
    EXPECT_TRUE(loadPinnedWatches(fs, root)->empty());
    EXPECT_NE(fs.readFile(path)->find("// personal comment"), std::string::npos);
}
TEST(PinnedWatches, MissingWorkspaceIsReadOnlyAndCorruptDocumentIsNeverOverwritten) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("WatchWorkspace");
    const auto path = test::workPath("WatchWorkspace/.frost/workspace.json");
    ASSERT_TRUE(loadPinnedWatches(fs, root));
    EXPECT_FALSE(fs.exists(path).value_or(true));
    ASSERT_TRUE(fs.createDirectories(test::workPath("WatchWorkspace/.frost")));
    ASSERT_TRUE(fs.writeFileAtomic(path, "{ broken"));
    EXPECT_FALSE(savePinnedWatches(fs, root, {"counter"}));
    EXPECT_EQ(*fs.readFile(path), "{ broken");
}
} // namespace frost::debug
