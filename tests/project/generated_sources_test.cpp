// SPDX-License-Identifier: Apache-2.0
// Pure pre-create CMake plans preserve original inputs and merge one membership replacement.
#include "frost/io/path.hpp"
#include "frost/project/generated_sources.hpp"

#include "project_test_util.hpp"
#include <gtest/gtest.h>
namespace frost::project {
namespace {
std::string root() { return test::MemTree::root(); }
CMakeProject model() {
    CMakeProject value;
    value.sourceDirectory = root();
    value.buildDirectory = root() + "/build";
    value.generator = "Ninja";
    value.configuration = "Debug";
    value.configurations = {"Debug"};
    value.inputsAvailable = true;
    value.cacheAvailable = true;
    value.inputs.push_back({root() + "/CMakeLists.txt", false, false, false});
    CMakeTarget target;
    target.name = "core";
    target.id = "core-id";
    target.type = CMakeTargetType::StaticLibrary;
    target.sourceDirectory = root();
    target.backtrace = {{root() + "/CMakeLists.txt", 3, "add_library"}};
    target.sources.push_back(CMakeSource{.path = root() + "/old.cpp"});
    value.targets.push_back(std::move(target));
    return value;
}
std::vector<CMakeInputSnapshot> inputs() {
    return {{root() + "/CMakeLists.txt",
             "cmake_minimum_required(VERSION 3.20)\nproject(Test LANGUAGES CXX)\nadd_library(core STATIC old.cpp)\n"}};
}
TEST(GeneratedSources, HeaderAndSourceJoinOneOriginalDefinitionWithoutAnyPrewrittenFiles) {
    const auto files = std::vector<std::string>{root() + "/Widget.hpp", root() + "/Widget.cpp"};
    auto before = inputs();
    auto proposal = planCMakeGeneratedSources(model(), 0, files, before);
    ASSERT_TRUE(proposal) << proposal.error().describe();
    ASSERT_EQ(proposal->size(), 1);
    EXPECT_EQ(proposal->front().original, before.front().contents);
    EXPECT_NE(proposal->front().replacement.find("Widget.hpp"), std::string::npos);
    EXPECT_NE(proposal->front().replacement.find("Widget.cpp"), std::string::npos);
    EXPECT_EQ(before.front().contents, inputs().front().contents);
}
TEST(GeneratedSources, NewTargetAndCurrentCacheIdentityHaveIndependentAdmission) {
    auto source = model();
    const std::vector<std::string> files{root() + "/Tool.cpp"};
    auto proposed = planCMakeTemplateTarget(source, "Tool", false, files, inputs());
    ASSERT_TRUE(proposed) << proposed.error().describe();
    EXPECT_NE(proposed->replacement.find("add_executable(Tool"), std::string::npos);
    EXPECT_FALSE(planCMakeTemplateTarget(source, "core", false, files, inputs()));
    const auto cache = "CMAKE_HOME_DIRECTORY:INTERNAL=" + source.sourceDirectory +
                       "\nCMAKE_CACHEFILE_DIR:INTERNAL=" + source.buildDirectory + "\nCMAKE_GENERATOR:INTERNAL=Ninja\n";
    EXPECT_TRUE(validateTemplateCMakeCache(source, cache));
    EXPECT_FALSE(validateTemplateCMakeCache(source, cache + "CMAKE_PROJECT_INCLUDE:FILEPATH=hidden.cmake\n"));
    source.generator = "Other";
    EXPECT_FALSE(validateTemplateCMakeCache(source, cache));
}
TEST(GeneratedSources, StaleUnmodeledTargetsAndFileDependentConditionsRefuse) {
    const std::vector<std::string> files{root() + "/Tool.cpp"};
    auto changed = inputs();
    changed.front().contents += "add_library(unmodeled STATIC other.cpp)\n";
    EXPECT_FALSE(planCMakeTemplateTarget(model(), "Tool", false, files, changed));
    changed = inputs();
    changed.front().contents += "if(EXISTS Tool.cpp)\nendif()\n";
    EXPECT_FALSE(planCMakeGeneratedSources(model(), 0, files, changed));
}
TEST(GeneratedSources, DirtyOtherTargetCannotAcquireGeneratedDestinationsOutsideThePreview) {
    auto source = model();
    auto snapshots = inputs();
    auto second = source.targets.front();
    second.id = "other-id";
    second.name = "other";
    second.backtrace.front().line = 4;
    second.sources.front().path = root() + "/other.cpp";
    source.targets.push_back(std::move(second));
    snapshots.front().contents += "add_library(other STATIC other.cpp)\ntarget_sources(other PRIVATE Widget.cpp)\n";
    const std::vector<std::string> paths{root() + "/Widget.cpp"};
    EXPECT_FALSE(planCMakeGeneratedSources(source, 0, paths, snapshots));
    EXPECT_FALSE(planCMakeTemplateTarget(source, "NewTarget", false, paths, snapshots));
}
TEST(GeneratedSources, FrostNewProjectLanguageStandardSettingsDoNotBlockClassOrTargetTemplates) {
    auto source = model();
    auto snapshots = inputs();
    snapshots.front().contents =
        "cmake_minimum_required(VERSION 3.20)\nproject(Test LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD "
        "20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nset(CMAKE_CXX_EXTENSIONS OFF)\nadd_library(core STATIC old.cpp)\n";
    source.targets.front().backtrace.front().line = 6;
    const std::vector<std::string> paths{root() + "/Widget.hpp", root() + "/Widget.cpp"};
    EXPECT_TRUE(planCMakeGeneratedSources(source, 0, paths, snapshots));
    EXPECT_TRUE(planCMakeTemplateTarget(source, "Tool", true, paths, snapshots));
    snapshots.front().contents += "set(CMAKE_PROJECT_INCLUDE hidden.cmake)\n";
    EXPECT_FALSE(planCMakeGeneratedSources(source, 0, paths, snapshots));
    EXPECT_FALSE(planCMakeTemplateTarget(source, "Tool", true, paths, snapshots));
}
} // namespace
} // namespace frost::project
