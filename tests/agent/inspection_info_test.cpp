// SPDX-License-Identifier: Apache-2.0
// Source metadata tests never launch the analyzer or assert analysis/tool availability.
#include "frost/agent/ide_queries.hpp"
#include <gtest/gtest.h>
namespace {
TEST(AgentInspectionInfo, ReturnsExactSourceOwnedMetadataWithoutExecutionClaims) {
    auto result = frost::agent::inspectionInfo("frost-self-assignment");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->stringAt("id"), "frost-self-assignment");
    EXPECT_EQ(result->stringAt("source"), "source-owned-registry");
    EXPECT_EQ(result->stringAt("engine"), "frost-analyzer");
    EXPECT_EQ(result->stringAt("tier"), "T2-AST");
    EXPECT_FALSE(result->stringAt("title").empty()); EXPECT_FALSE(result->stringAt("applicability").empty());
    EXPECT_FALSE(result->stringAt("settingKey").empty());
    EXPECT_EQ(result->find("toolAvailable"), nullptr); EXPECT_EQ(result->find("findings"), nullptr);
}
TEST(AgentInspectionInfo, UnknownExternalPatternAndMissingIdsRemainUnavailable) {
    EXPECT_FALSE(frost::agent::inspectionInfo(""));
    EXPECT_FALSE(frost::agent::inspectionInfo("frost-self-assignment*"));
    EXPECT_FALSE(frost::agent::inspectionInfo("clang-diagnostic-invented"));
    EXPECT_FALSE(frost::agent::inspectionInfo("bugprone-external-rule"));
}
}
