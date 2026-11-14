// SPDX-License-Identifier: Apache-2.0
// Authored R8 membership/plan protocol coverage; execution is deferred by user instruction.
#include "frost/project/vcxproj.hpp"
#include "frost/project/vcxproj_rename.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::project {
namespace {

using test::MemTree;

struct RenameRig {
    std::string project = MemTree::path("App/App.vcxproj");
    std::string from = MemTree::path("App/src/old.cpp");
    std::string to = MemTree::path("App/src/new.cpp");
    std::vector<VcxprojRenameInput> inputs{
        {project, "<Project><ItemGroup><ClCompile Include=\"src\\old.cpp\"/></ItemGroup></Project>"},
        {project + ".filters", std::nullopt}};
    Result<VcxprojRenamePlan> plan() { return planVcxprojRename({project, from, to, inputs}); }
};

TEST(VcxprojRename, IncludesFiltersAndDisplayLinkJoinOneBeforeAfterPlan) {
    RenameRig rig;
    rig.inputs[0].contents =
        "<?xml version=\"1.0\"?><Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">"
        "<!--keep--><ItemGroup><ClCompile Include=\"src\\old.cpp\">"
        "<Link>Virtual\\old.cpp</Link><ExcludedFromBuild Condition=\"'$(Configuration)'=='Release'\">true"
        "</ExcludedFromBuild></ClCompile></ItemGroup></Project>";
    rig.inputs[1].contents = "<Project><ItemGroup><ClCompile Include=\"src\\old.cpp\"><Filter>Source Files</Filter>"
                             "</ClCompile></ItemGroup></Project>";
    const auto original = rig.inputs;
    auto plan = rig.plan();
    ASSERT_TRUE(plan) << plan.error().describe();
    ASSERT_EQ(plan->replacements.size(), 2);
    EXPECT_EQ(plan->replacements[0].before, *original[0].contents);
    EXPECT_NE(plan->replacements[0].after.find("src\\new.cpp"), std::string::npos);
    EXPECT_NE(plan->replacements[0].after.find("Virtual\\new.cpp"), std::string::npos);
    EXPECT_NE(plan->replacements[0].after.find("<!--keep-->"), std::string::npos);
    EXPECT_NE(plan->replacements[0].after.find("ExcludedFromBuild"), std::string::npos);
    EXPECT_NE(plan->replacements[1].after.find("<Filter>Source Files</Filter>"), std::string::npos);
    EXPECT_EQ(rig.inputs[0].contents, original[0].contents);
    EXPECT_EQ(rig.inputs[1].contents, original[1].contents);
    EXPECT_EQ(plan->dependencies.size(), 2);
    // The established reader is an independent semantic oracle for the serialized replacement.
    MemTree tree;
    auto parsed = parseVcxproj(tree.fs, plan->replacements[0].after, rig.project);
    ASSERT_TRUE(parsed) << parsed.error().describe();
    ASSERT_EQ(parsed->items.size(), 1);
    EXPECT_EQ(parsed->items[0].path, rig.to);
}

TEST(VcxprojRename, AbsentFiltersRemainAnObservedAbsenceAndCustomLinkAliasesStay) {
    RenameRig rig;
    rig.inputs[0].contents = "<Project><ItemGroup><ClCompile Include='src/old.cpp'><Link>Alias.cpp</Link>"
                             "</ClCompile></ItemGroup></Project>";
    auto plan = rig.plan();
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_EQ(plan->replacements.size(), 1);
    EXPECT_FALSE(plan->dependencies[1].contents);
    EXPECT_NE(plan->replacements[0].after.find("Alias.cpp"), std::string::npos);
}

TEST(VcxprojRename, XmlEscapesDestinationInsteadOfInjectingMarkup) {
    RenameRig rig;
    rig.to = MemTree::path("App/src/new&name.cpp");
    auto plan = rig.plan();
    ASSERT_TRUE(plan) << plan.error().describe();
    EXPECT_NE(plan->replacements[0].after.find("new&amp;name.cpp"), std::string::npos);
    MemTree tree;
    auto parsed = parseVcxproj(tree.fs, plan->replacements[0].after, rig.project);
    ASSERT_TRUE(parsed) << parsed.error().describe();
    ASSERT_EQ(parsed->items.size(), 1);
    EXPECT_EQ(parsed->items[0].path, rig.to);
}

TEST(VcxprojRename, AmbiguousIncludesAndBuildTimeOrExcludedMembershipAreRefused) {
    for (const std::string xml :
         {"<Project><ItemGroup><ClCompile Include='src/*.cpp'/></ItemGroup></Project>",
          "<Project><ItemGroup><ClCompile Include='$(Sources)'/></ItemGroup></Project>",
          "<Project><ItemGroup><ClCompile Include='src/old.cpp;other.cpp'/></ItemGroup></Project>",
          "<Project><Target Name='Build'><ItemGroup><ClCompile Include='src/old.cpp'/></ItemGroup></Target></Project>",
          "<Project><ItemGroup><ClCompile Include='src/old.cpp' Exclude='old.cpp'/></ItemGroup></Project>",
          "<Project><ItemGroup><ClCompile Update='src/old.cpp'/></ItemGroup></Project>"}) {
        RenameRig rig;
        rig.inputs[0].contents = xml;
        auto plan = rig.plan();
        ASSERT_FALSE(plan) << xml;
        EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported) << plan.error().describe();
    }
}

TEST(VcxprojRename, PureRouteRefusesFileDependentGroupItemAndMetadataConditions) {
    for (const std::string xml : {"<Project><ItemGroup Condition=\"Exists('src/old.cpp')\"><ClCompile "
                                  "Include='src/old.cpp'/></ItemGroup></Project>",
                                  "<Project><ItemGroup><ClCompile Include='src/old.cpp' "
                                  "Condition=\"Exists('src/new.cpp')\"/></ItemGroup></Project>",
                                  "<Project><ItemGroup><ClCompile Include='src/old.cpp'><ExcludedFromBuild "
                                  "Condition=\"Exists('src/old.cpp')\">true"
                                  "</ExcludedFromBuild></ClCompile></ItemGroup></Project>"}) {
        RenameRig rig;
        rig.inputs[0].contents = xml;
        auto plan = rig.plan();
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
    }
}

TEST(VcxprojRename, ImportsAndBothSdkFormsRequireActualPlanOnlyMsbuild) {
    for (const std::string prefix : {"<Project><Import Project='sdk.props'/>", "<Project Sdk='Vendor.Sdk'>",
                                     "<Project><Sdk Name='Vendor.Sdk'/>"}) {
        RenameRig rig;
        rig.inputs[0].contents = prefix + "<ItemGroup><ClCompile Include='src/old.cpp'/></ItemGroup></Project>";
        auto plan = rig.plan();
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error().code(), ErrorCode::Unsupported);
        EXPECT_NE(plan.error().message().find("MSBuild"), std::string::npos);
    }
}

TEST(VcxprojRename, DestinationCollisionsMalformedXmlAndDuplicateSnapshotsRefuseTheWholePlan) {
    RenameRig rig;
    rig.inputs[0].contents = "<Project><ItemGroup><ClCompile Include='src/old.cpp'/><ClCompile Include='src/new.cpp'/>"
                             "</ItemGroup></Project>";
    auto collision = rig.plan();
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code(), ErrorCode::AlreadyExists);
    rig.inputs[0].contents = "<Project>";
    auto malformed = rig.plan();
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code(), ErrorCode::Corrupt);
    rig.inputs.push_back(rig.inputs[0]);
    auto duplicate = rig.plan();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);
}

TEST(VcxprojRename, FiltersPresenceMustBeCapturedAndUnsafeDestinationNamesAreRefused) {
    RenameRig rig;
    rig.inputs.pop_back();
    EXPECT_FALSE(rig.plan());
    rig.inputs.push_back({rig.project + ".filters", std::nullopt});
    rig.to = MemTree::path("App/src/$(NewName).cpp");
    EXPECT_FALSE(rig.plan());
}

} // namespace
} // namespace frost::project
