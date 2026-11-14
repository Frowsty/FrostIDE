// SPDX-License-Identifier: Apache-2.0
//
// The edit protocol of SLN-6 without a helper (frost/project/project_edit.hpp; label "unit"): the
// request lines FrostIDE sends, the decision line, the answers it accepts, and the project edits a
// rename, move or delete implies for an in-memory solution. The helper itself is exercised against
// the real MSBuild in project_edit_real_test.cpp.

#include "frost/project/project_edit.hpp"
#include "frost/project/solution_model.hpp"

#include "json.hpp"
#include "project_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::MemTree;

constexpr std::string_view kId = "9cdb031c-2eb4-4765-a6f9-62bbeb5f3a15";

ProjectEdit fileEdit(ProjectEditKind kind) {
    ProjectEdit edit;
    edit.kind = kind;
    edit.root = MemTree::root();
    edit.definition = MemTree::path("App/App.vcxproj");
    edit.files = {MemTree::path("App/a.cpp")};
    edit.configuration = "Debug";
    edit.platform = "x64";
    edit.transactionId = std::string(kId);
    return edit;
}

// The request's members as the helper's JavaScriptSerializer reads them (all strings).
std::map<std::string, std::string> members(const std::string& line) {
    auto json = detail::parseJson(line);
    EXPECT_TRUE(json && json->isObject()) << line;
    std::map<std::string, std::string> out;
    if (!json) {
        return out;
    }
    for (std::size_t i = 0; i < json->keys.size(); ++i) {
        EXPECT_TRUE(json->items[i].isString()) << json->keys[i];
        out[json->keys[i]] = json->items[i].text;
    }
    return out;
}

TEST(ProjectEditProtocol, PropertyPagesPreserveExpressionsAsJsonStrings) {
    auto edit = fileEdit(ProjectEditKind::SetProperties);
    edit.files.clear();
    edit.earlyProperties = true;
    edit.propertyUpdates = {
        {"ClCompile.AdditionalIncludeDirectories", "$(ProjectDir)include;%(AdditionalIncludeDirectories)"},
        {"Prop.NMakeBuildCommandLine", "tool \"quoted\"\nnext line"}};
    auto line = projectEditRequest(edit);
    ASSERT_TRUE(line) << line.error().describe();
    const auto got = members(*line);
    EXPECT_EQ(got.at("Set:ClCompile.AdditionalIncludeDirectories"), edit.propertyUpdates[0].second);
    EXPECT_EQ(got.at("Set:Prop.NMakeBuildCommandLine"), edit.propertyUpdates[1].second);
    EXPECT_EQ(got.at("EarlyProperties"), "true");
    EXPECT_FALSE(got.contains("Add"));
    EXPECT_EQ(got.at("Configuration"), "Debug");
    edit.propertyUpdates = {{"Prop.Bad.Name", "value"}};
    EXPECT_FALSE(projectEditRequest(edit));
    edit.propertyUpdates.clear();
    EXPECT_FALSE(projectEditRequest(edit));
}

TEST(ProjectEditProtocol, FileEditsNameTheProjectTheirFilesAndTheConfiguration) {
    auto add = fileEdit(ProjectEditKind::AddFiles);
    add.files.push_back(MemTree::path("App/sub dir/b \"quoted\".h"));
    add.filter = "Source Files";
    auto line = projectEditRequest(add);
    ASSERT_TRUE(line) << line.error().describe();
    auto got = members(*line);
    EXPECT_EQ(got["Project"], add.definition);
    EXPECT_EQ(got["Root"], add.root);
    EXPECT_EQ(got["Trusted"], "true");
    EXPECT_EQ(got["Commit"], "true");
    EXPECT_EQ(got["TransactionId"], kId);
    EXPECT_EQ(got["Configuration"], "Debug");
    EXPECT_EQ(got["Platform"], "x64");
    EXPECT_EQ(got["Add"], add.files[0] + "|" + add.files[1]);
    EXPECT_EQ(got["Filter"], "Source Files");
    EXPECT_FALSE(got.contains("Remove"));

    auto remove = fileEdit(ProjectEditKind::RemoveFiles);
    remove.commit = false;
    got = members(projectEditRequest(remove).value_or(""));
    EXPECT_EQ(got["Remove"], remove.files[0]);
    EXPECT_EQ(got["Commit"], "false");
    EXPECT_FALSE(got.contains("Add"));

    auto rename = fileEdit(ProjectEditKind::RenameFiles);
    rename.renamedTo = {MemTree::path("App/more/a2.cpp")};
    got = members(projectEditRequest(rename).value_or(""));
    EXPECT_EQ(got["RenameFrom"], rename.files[0]);
    EXPECT_EQ(got["RenameTo"], rename.renamedTo[0]);
}

TEST(ProjectEditProtocol, SolutionEditsNameTheSolutionAndTheProject) {
    ProjectEdit edit;
    edit.kind = ProjectEditKind::RemoveProject;
    edit.root = MemTree::root();
    edit.definition = MemTree::path("Demo.slnx");
    edit.project = MemTree::path("Core/Core.vcxproj");
    edit.transactionId = std::string(kId);
    auto got = members(projectEditRequest(edit).value_or(""));
    EXPECT_EQ(got["Solution"], edit.definition);
    EXPECT_EQ(got["RemoveProject"], edit.project);
    EXPECT_FALSE(got.contains("Project"));
    edit.kind = ProjectEditKind::AddProject;
    edit.definition = MemTree::path("Demo.sln");
    got = members(projectEditRequest(edit).value_or(""));
    EXPECT_EQ(got["AddProject"], edit.project);
}

TEST(ProjectEditProtocol, RefusesRequestsTheHelperCouldMisread) {
    const auto refused = [](const ProjectEdit& edit, std::string_view what) {
        auto line = projectEditRequest(edit);
        ASSERT_FALSE(line) << what << ": " << *line;
        EXPECT_EQ(line.error().code(), ErrorCode::InvalidArgument) << what;
    };
    auto edit = fileEdit(ProjectEditKind::AddFiles);
    edit.files = {"App/a.cpp"};
    refused(edit, "relative file");
    edit = fileEdit(ProjectEditKind::AddFiles);
    edit.files = {MemTree::path("App/a|b.cpp")};
    refused(edit, "list separator in a name");
    edit = fileEdit(ProjectEditKind::AddFiles);
    edit.files.clear();
    refused(edit, "no files");
    edit = fileEdit(ProjectEditKind::RenameFiles);
    refused(edit, "rename without new names");
    edit = fileEdit(ProjectEditKind::RemoveFiles);
    edit.filter = "Source Files";
    refused(edit, "filter on a removal");
    edit = fileEdit(ProjectEditKind::RemoveFiles);
    edit.transactionId = "not-a-guid";
    refused(edit, "transaction id");
    edit = fileEdit(ProjectEditKind::RemoveFiles);
    edit.definition = MemTree::path("Demo.sln");
    refused(edit, "file edit of a solution");
    edit = fileEdit(ProjectEditKind::RemoveFiles);
    edit.configuration.clear();
    refused(edit, "no configuration");
    edit = fileEdit(ProjectEditKind::AddProject);
    edit.project = MemTree::path("Core/Core.vcxproj");
    refused(edit, "solution edit of a project");
}

TEST(ProjectEditProtocol, ReadsReadyAndFinalAnswersAndRefusesOthers) {
    const std::string digest(64, 'A');
    auto ready = parseProjectEditAnswer(R"({"Ready":true,"TransactionId":")" + std::string(kId) +
                                        R"(","JournalDigest":")" + digest + "\"}");
    ASSERT_TRUE(ready) << ready.error().describe();
    EXPECT_TRUE(ready->ready);
    EXPECT_EQ(ready->transactionId, kId);
    EXPECT_EQ(ready->journalDigest, digest);
    auto done = parseProjectEditAnswer(R"({"Items":null,"Error":null})");
    ASSERT_TRUE(done);
    EXPECT_FALSE(done->ready);
    EXPECT_FALSE(done->error.has_value());
    auto refused = parseProjectEditAnswer(R"({"Items":null,"Error":"a.cpp comes from a wildcard"})");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->error.value_or(""), "a.cpp comes from a wildcard");
    EXPECT_FALSE(parseProjectEditAnswer("not json"));
    EXPECT_FALSE(parseProjectEditAnswer(R"({"Items":null})"));
    EXPECT_FALSE(parseProjectEditAnswer(R"({"Ready":true,"TransactionId":"x","JournalDigest":"y"})"));
    auto decision = members(projectEditDecision(kId, true));
    EXPECT_EQ(decision["Accept"], "true");
    EXPECT_EQ(decision["TransactionId"], kId);
    EXPECT_EQ(members(projectEditDecision(kId, false))["Accept"], "false");
}

TEST(ProjectEditPathChange, RenamesAndRemovesOnlyLiteralItemsUnderThePath) {
    MemTree tree;
    tree.write("Demo.sln",
               "Microsoft Visual Studio Solution File, Format Version 12.00\n"
               "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"App\", \"App\\App.vcxproj\", "
               "\"{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E03}\"\nEndProject\n"
               "Global\n\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n\t\tDebug|x64 = Debug|x64\n"
               "\tEndGlobalSection\nEndGlobal\n");
    tree.write("App/App.vcxproj", "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"><ItemGroup>"
                                  "<ClCompile Include=\"src\\a.cpp\" /><ClCompile Include=\"src\\deep\\b.cpp\" />"
                                  "<ClCompile Include=\"glob\\*.cpp\" /><ClInclude Include=\"..\\shared\\s.h\" />"
                                  "<ClCompile Include=\"srcx.cpp\" /></ItemGroup></Project>");
    tree.write("App/glob/g.cpp", "");
    auto model = SolutionModel::load(tree.fs, MemTree::path("Demo.sln"));
    ASSERT_TRUE(model) << model.error().describe();

    auto folder = projectEditsForPathChange(*model, MemTree::path("App/src"), MemTree::path("App/code"), true);
    ASSERT_EQ(folder.size(), 1U);
    EXPECT_EQ(folder[0].project, MemTree::path("App/App.vcxproj"));
    EXPECT_EQ(folder[0].files,
              (std::vector<std::string>{MemTree::path("App/src/a.cpp"), MemTree::path("App/src/deep/b.cpp")}));
    EXPECT_EQ(folder[0].renamedTo,
              (std::vector<std::string>{MemTree::path("App/code/a.cpp"), MemTree::path("App/code/deep/b.cpp")}));

    // A sibling whose name starts like the folder ("srcx.cpp") is not under it.
    const bool windows = io::kNativePathStyle == io::PathStyle::Windows; // NTFS compares names case-insensitively.
    auto file =
        projectEditsForPathChange(*model, MemTree::path(windows ? "App/SRC/A.CPP" : "App/src/a.cpp"), {}, false);
    ASSERT_EQ(file.size(), 1U);
    EXPECT_EQ(file[0].files, (std::vector<std::string>{MemTree::path("App/src/a.cpp")}));
    EXPECT_TRUE(file[0].renamedTo.empty());

    auto outside = projectEditsForPathChange(*model, MemTree::path("shared/s.h"), MemTree::path("shared/t.h"), false);
    ASSERT_EQ(outside.size(), 1U);
    EXPECT_EQ(outside[0].renamedTo, (std::vector<std::string>{MemTree::path("shared/t.h")}));

    // Wildcard items follow the disk by themselves.
    EXPECT_TRUE(projectEditsForPathChange(*model, MemTree::path("App/glob/g.cpp"), {}, false).empty());
    EXPECT_TRUE(projectEditsForPathChange(*model, MemTree::path("App/other.cpp"), {}, false).empty());
}

} // namespace
} // namespace frost::project
