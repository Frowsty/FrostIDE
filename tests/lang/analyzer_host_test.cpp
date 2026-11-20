// SPDX-License-Identifier: Apache-2.0
// Host acceptance guards for real cross-configuration fix agreement and strict profile settings.
#include "frost/lang/analyzer.hpp"
#include <gtest/gtest.h>
namespace frost::lang::analyzer_detail {
void reconcileFixes(AnalyzerReport&,const std::vector<Json>&);
}
namespace {
using namespace frost::lang;
TEST(AnalyzerHost, MissingFixInOneConfigurationCannotAgreeWithDeletion) {
    const std::string path="/project/header.hpp";
    auto finding=Json::object().set("ruleId","frost-empty-if").set("path",path).set("offset",4).set("length",1);
    auto unit=[&](Json value) {return Json::object().set("dependencies",Json::array({Json::object().set("path",path)})).set("findings",Json::array({std::move(value)}));};
    auto deletion=finding;deletion.set("replacement","");
    AnalyzerReport report;report.fixes.push_back({"frost-empty-if",path,"",4,1});
    analyzer_detail::reconcileFixes(report,{unit(deletion),unit(finding)});
    EXPECT_TRUE(report.fixes.empty());
    report.fixes.push_back({"frost-empty-if",path,"",4,1});
    analyzer_detail::reconcileFixes(report,{unit(deletion),unit(deletion)});
    ASSERT_EQ(report.fixes.size(),1U);EXPECT_TRUE(report.fixes.front().replacement.empty());
}
TEST(AnalyzerHost, ProfileRejectsMalformedSeverityAndRetainsExplicitDisable) {
    const std::vector<std::string> malformed={"frost-*=loud"};
    EXPECT_FALSE(analyzerProfile(malformed));
    const std::vector<std::string> disabled={"frost-null-*=off"};
    auto profile=analyzerProfile(disabled);ASSERT_TRUE(profile);
    ASSERT_EQ(profile->rules.size(),1U);EXPECT_FALSE(profile->rules.front().enabled);
}
}
