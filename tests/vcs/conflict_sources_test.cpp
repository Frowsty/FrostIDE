// SPDX-License-Identifier: Apache-2.0
// Actual Git stage record framing, absence and immutable blob identity protections.
#include "frost/vcs/conflict_sources.hpp"

#include <gtest/gtest.h>
namespace frost::vcs {
namespace {
std::string record(char hash, int stage, std::string_view path, std::string mode = "100644") {
    return mode + " " + std::string(40, hash) + " " + std::to_string(stage) + "\t" + std::string(path) +
           std::string(1, '\0');
}
TEST(ConflictSources, ExactRecordsKeepAbsentBaseAndOpaquePathBytes) {
    const std::string path = "Assets/Scene 1\tname\n.fscene";
    auto parsed = parseConflictStages(record('a', 2, path) + record('b', 3, path), path);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE((*parsed)[0]);
    ASSERT_TRUE((*parsed)[1]);
    EXPECT_EQ((*parsed)[1]->object, std::string(40, 'a'));
}
TEST(ConflictSources, RefusesWrongPathsDuplicateStagesSymlinkAndMissingTerminator) {
    const std::string path = "Assets/Scene.fscene";
    EXPECT_FALSE(parseConflictStages(record('a', 1, "other"), path));
    EXPECT_FALSE(parseConflictStages(record('a', 1, path) + record('b', 1, path), path));
    EXPECT_FALSE(parseConflictStages(record('a', 2, path, "120000"), path));
    auto missing = record('a', 1, path);
    missing.pop_back();
    EXPECT_FALSE(parseConflictStages(missing, path));
}
} // namespace
} // namespace frost::vcs
