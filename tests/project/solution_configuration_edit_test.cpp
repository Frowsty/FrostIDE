// SPDX-License-Identifier: Apache-2.0
//
// SLN-7's solution writes without MSBuild (frost/project/solution_configuration_edit.hpp): the
// Configuration Manager's mapping edits change exactly the lines (.sln) or rules (.slnx) they
// must, on the synthetic generated solution (fixtures/msvc_generated), and the verified write on
// a scratch copy reads back what was asked for. Label "unit"; that MSBuild builds the edited
// mapping is checked in cl_command_lines_real_test.cpp.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_configuration_edit.hpp"
#include "frost/project/solution_model.hpp"

#include "project_test_util.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace frost::project {
namespace {

using test::fixture;

std::string generated(std::string_view relative) {
    return fixture(std::string("msvc_generated/") + std::string(relative));
}

SolutionModel loadModel(const std::string& path) {
    auto model = SolutionModel::load(io::realFileSystem(), path);
    EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().describe());
    return std::move(*model);
}

ProjectId projectNamed(const SolutionModel& model, std::string_view name) {
    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        if (model.projects()[p].name == name) {
            return ProjectId{static_cast<std::uint32_t>(p)};
        }
    }
    ADD_FAILURE() << "no project " << name;
    return ProjectId{0};
}

std::size_t configurationNamed(const SolutionModel& model, std::string_view name) {
    const auto index = model.findConfiguration(name);
    EXPECT_TRUE(index.has_value()) << name;
    return index.value_or(0);
}

std::vector<std::string> linesOf(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        out.emplace_back(text.substr(start, end - start));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// The lines of `after` that differ from `before` at the same position (both of the same length).
std::vector<std::pair<std::string, std::string>> changedLines(std::string_view before, std::string_view after) {
    const auto a = linesOf(before);
    const auto b = linesOf(after);
    EXPECT_EQ(a.size(), b.size());
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (a[i] != b[i]) {
            out.emplace_back(a[i], b[i]);
        }
    }
    return out;
}

constexpr std::string_view kCore = "{6A1D0C1E-0B0E-4C1B-9E51-0000000C0E01}";

TEST(SolutionConfigurationEdit, ASlnRemapChangesOnlyThatConfigurationsValues) {
    const SolutionModel model = loadModel(generated("Generated.sln"));
    const std::string content = io::realFileSystem().readFile(model.path()).value();
    const ConfigurationMappingEdit edit{
        .project = projectNamed(model, "Core"),
        .solutionConfiguration = configurationNamed(model, "Debug|x64"),
        .mapping =
            ProjectConfigurationMapping{.configuration = "Release", .platform = "x64", .build = true, .deploy = false}};
    auto edited = applySlnConfigurationEdits(content, model, std::span(&edit, 1));
    ASSERT_TRUE(edited.has_value()) << edited.error().describe();
    const auto changed = changedLines(content, *edited);
    ASSERT_EQ(changed.size(), 2U);
    EXPECT_EQ(changed[0].second, "\t\t" + std::string(kCore) + ".Debug|x64.ActiveCfg = Release|x64");
    EXPECT_EQ(changed[1].second, "\t\t" + std::string(kCore) + ".Debug|x64.Build.0 = Release|x64");
    // Nothing to do is no change at all.
    const ConfigurationMappingEdit same{.project = edit.project,
                                        .solutionConfiguration = edit.solutionConfiguration,
                                        .mapping =
                                            model.projectConfiguration(edit.project, edit.solutionConfiguration)};
    EXPECT_EQ(applySlnConfigurationEdits(content, model, std::span(&same, 1)).value(), content);
}

TEST(SolutionConfigurationEdit, SlnBuildDeployAndRemovalAddOrDropOnlyTheirLines) {
    const SolutionModel model = loadModel(generated("Generated.sln"));
    const std::string content = io::realFileSystem().readFile(model.path()).value();
    const ProjectId core = projectNamed(model, "Core");
    const std::size_t debug = configurationNamed(model, "Debug|x64");
    const std::string active = "\t\t" + std::string(kCore) + ".Debug|x64.ActiveCfg = Debug|x64";
    const std::string build = "\t\t" + std::string(kCore) + ".Debug|x64.Build.0 = Debug|x64";
    const std::string deploy = "\t\t" + std::string(kCore) + ".Debug|x64.Deploy.0 = Debug|x64";
    const auto lines = linesOf(content);
    const auto at = [](const std::vector<std::string>& in, const std::string& line) {
        return std::ranges::find(in, line) - in.begin();
    };

    const ConfigurationMappingEdit noBuild{
        .project = core,
        .solutionConfiguration = debug,
        .mapping =
            ProjectConfigurationMapping{.configuration = "Debug", .platform = "x64", .build = false, .deploy = false}};
    const auto withoutBuild = linesOf(applySlnConfigurationEdits(content, model, std::span(&noBuild, 1)).value());
    EXPECT_EQ(withoutBuild.size(), lines.size() - 1);
    EXPECT_EQ(at(withoutBuild, build), static_cast<std::ptrdiff_t>(withoutBuild.size()));
    auto expected = lines;
    std::erase(expected, build);
    EXPECT_EQ(withoutBuild, expected) << "only the Build.0 line went";

    const ConfigurationMappingEdit withDeploy{
        .project = core,
        .solutionConfiguration = debug,
        .mapping =
            ProjectConfigurationMapping{.configuration = "Debug", .platform = "x64", .build = true, .deploy = true}};
    const auto deployed = linesOf(applySlnConfigurationEdits(content, model, std::span(&withDeploy, 1)).value());
    ASSERT_EQ(deployed.size(), lines.size() + 1);
    EXPECT_EQ(at(deployed, deploy), at(deployed, build) + 1) << "Deploy.0 follows Build.0, as Visual Studio writes it";

    const ConfigurationMappingEdit removed{.project = core, .solutionConfiguration = debug, .mapping = std::nullopt};
    const auto without = linesOf(applySlnConfigurationEdits(content, model, std::span(&removed, 1)).value());
    expected = lines;
    std::erase(expected, active);
    std::erase(expected, build);
    EXPECT_EQ(without, expected);

    const ConfigurationMappingEdit unknown{.project = core, .solutionConfiguration = 99, .mapping = std::nullopt};
    EXPECT_EQ(applySlnConfigurationEdits(content, model, std::span(&unknown, 1)).error().code(), ErrorCode::NotFound);
    const ConfigurationMappingEdit empty{
        .project = core,
        .solutionConfiguration = debug,
        .mapping = ProjectConfigurationMapping{.configuration = "", .platform = "x64", .build = true, .deploy = false}};
    EXPECT_EQ(applySlnConfigurationEdits(content, model, std::span(&empty, 1)).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(SolutionConfigurationEdit, SlnxEditsReplaceAnExactRuleOrAppendOneAndKeepEveryOtherByte) {
    const SolutionModel model = loadModel(generated("Generated.slnx"));
    const std::string content = io::realFileSystem().readFile(model.path()).value();
    const ProjectId core = projectNamed(model, "Core");
    const ProjectId renderer = projectNamed(model, "Renderer");
    // Core in Debug|x64 builds Release: no rule names Debug|x64, so one is appended to Core.
    const std::vector<ConfigurationMappingEdit> edits{
        {.project = core,
         .solutionConfiguration = configurationNamed(model, "Debug|x64"),
         .mapping =
             ProjectConfigurationMapping{
                 .configuration = "Release", .platform = "x64", .build = true, .deploy = false}},
        // Renderer does not build in Debug|x64.
        {.project = renderer,
         .solutionConfiguration = configurationNamed(model, "Debug|x64"),
         .mapping = ProjectConfigurationMapping{
             .configuration = "Debug", .platform = "x64", .build = false, .deploy = false}}};
    auto edited = applySlnxConfigurationEdits(content, model, edits);
    ASSERT_TRUE(edited.has_value()) << edited.error().describe();
    std::string expected = content;
    const std::string coreRule =
        "      <Build Solution=\"Profile|x86\" Project=\"false\" />\n    </Project>\n    <Project Path=\"Renderer";
    ASSERT_NE(expected.find(coreRule), std::string::npos);
    expected.replace(expected.find(coreRule), coreRule.size(),
                     "      <Build Solution=\"Profile|x86\" Project=\"false\" />\n"
                     "      <BuildType Solution=\"Debug|x64\" Project=\"Release\" />\n"
                     "    </Project>\n    <Project Path=\"Renderer");
    const std::string rendererRule =
        "      <Build Solution=\"Profile|x86\" Project=\"false\" />\n    </Project>\n  </Folder>\n</Solution>";
    ASSERT_NE(expected.find(rendererRule), std::string::npos);
    expected.replace(expected.find(rendererRule), rendererRule.size(),
                     "      <Build Solution=\"Profile|x86\" Project=\"false\" />\n"
                     "      <Build Solution=\"Debug|x64\" Project=\"false\" />\n"
                     "    </Project>\n  </Folder>\n</Solution>");
    EXPECT_EQ(*edited, expected);

    // Profile|x64's BuildType comes from the wildcard rule "Profile|*", which stays; the exact rule is added.
    const ConfigurationMappingEdit profile{
        .project = core,
        .solutionConfiguration = configurationNamed(model, "Profile|x64"),
        .mapping =
            ProjectConfigurationMapping{.configuration = "Debug", .platform = "x64", .build = true, .deploy = false}};
    auto profiled = applySlnxConfigurationEdits(content, model, std::span(&profile, 1));
    ASSERT_TRUE(profiled.has_value()) << profiled.error().describe();
    EXPECT_NE(profiled->find("<BuildType Solution=\"Profile|*\" Project=\"Release\" />"), std::string::npos);
    EXPECT_NE(profiled->find("<BuildType Solution=\"Profile|x64\" Project=\"Debug\" />"), std::string::npos);
    // Editing that exact rule again replaces its value in place: same length of file, one rule.
    const SolutionModel profiledModel = [&] {
        frost::test::ScratchDir scratch("slnx-config-edit");
        std::error_code error;
        std::filesystem::copy(std::filesystem::path(fixture("msvc_generated")), std::filesystem::path(scratch.path()),
                              std::filesystem::copy_options::recursive, error);
        EXPECT_FALSE(error) << error.message();
        EXPECT_TRUE(io::realFileSystem().writeFileAtomic(scratch.join("Generated.slnx"), *profiled).has_value());
        return loadModel(scratch.join("Generated.slnx"));
    }();
    const ConfigurationMappingEdit back{
        .project = projectNamed(profiledModel, "Core"),
        .solutionConfiguration = configurationNamed(profiledModel, "Profile|x64"),
        .mapping =
            ProjectConfigurationMapping{.configuration = "Profile", .platform = "x64", .build = true, .deploy = false}};
    auto replaced = applySlnxConfigurationEdits(*profiled, profiledModel, std::span(&back, 1));
    ASSERT_TRUE(replaced.has_value()) << replaced.error().describe();
    std::string expectedReplaced = *profiled;
    const std::string debugRule = "<BuildType Solution=\"Profile|x64\" Project=\"Debug\" />";
    expectedReplaced.replace(expectedReplaced.find(debugRule), debugRule.size(),
                             "<BuildType Solution=\"Profile|x64\" Project=\"Profile\" />");
    EXPECT_EQ(*replaced, expectedReplaced);
    // Removal has no .slnx form.
    const ConfigurationMappingEdit removed{.project = core, .solutionConfiguration = 0, .mapping = std::nullopt};
    EXPECT_EQ(applySlnxConfigurationEdits(content, model, std::span(&removed, 1)).error().code(),
              ErrorCode::Unsupported);
}

// The verified write on scratch copies of both files: the reloaded solution reads the new mapping.
TEST(SolutionConfigurationEdit, TheVerifiedWriteReadsBackTheEditedMappings) {
    frost::test::ScratchDir scratch("sln-config-edit");
    std::error_code error;
    std::filesystem::copy(std::filesystem::path(fixture("msvc_generated")), std::filesystem::path(scratch.path()),
                          std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
    for (const char* name : {"Generated.sln", "Generated.slnx"}) {
        SCOPED_TRACE(name);
        const std::string path = scratch.join(name);
        const SolutionModel model = loadModel(path);
        const ProjectId core = projectNamed(model, "Core");
        const std::size_t debug = configurationNamed(model, "Debug|x64");
        const ProjectConfigurationMapping release{
            .configuration = "Release", .platform = "x64", .build = true, .deploy = false};
        const ConfigurationMappingEdit edit{.project = core, .solutionConfiguration = debug, .mapping = release};
        ASSERT_TRUE(writeSolutionConfigurationEdits(io::realFileSystem(), model, std::span(&edit, 1)).has_value());
        const SolutionModel reloaded = loadModel(path);
        EXPECT_EQ(reloaded.projectConfiguration(projectNamed(reloaded, "Core"), debug), release);
        EXPECT_EQ(reloaded.projectConfiguration(projectNamed(reloaded, "Renderer"), debug),
                  model.projectConfiguration(projectNamed(model, "Renderer"), debug))
            << "other projects keep their mapping";
    }
}

} // namespace
} // namespace frost::project
