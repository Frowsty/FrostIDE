// SPDX-License-Identifier: Apache-2.0
// Evaluated project model preserves conditional/linked item metadata and refuses mismatched authority.
#include "frost/build/managed_evaluation.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>
namespace frost::build {
namespace {
ManagedProjectRequest request() {
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/sdk fixture/" : "/sdk fixture/";
    return {.dotnet = root + "dotnet",
            .project = root + "Library.csproj",
            .configuration = "Debug",
            .targetFramework = "net10.0"};
}
std::string output(const ManagedProjectRequest& query) {
    // JSON uses forward slashes so the same actual property shape is portable between test legs.
    return "{\"Properties\":{\"TargetPath\":\"\",\"TargetFramework\":\"net10.0\",\"TargetFrameworks\":\"net10.0;"
           "netstandard2.1\",\"OutputType\":\"Library\",\"RuntimeIdentifier\":\"\",\"MSBuildProjectFullPath\":\"" +
           query.project +
           "\",\"UsingMicrosoftNETSdk\":\"true\",\"AssemblyName\":\"Library\",\"LangVersion\":\"latest\",\"Nullable\":"
           "\"enable\",\"DefineConstants\":\"TRACE;MODERN_TFM\",\"IsTestProject\":\"false\","
           "\"ManagePackageVersionsCentrally\":\"true\"},\"Items\":{\"Compile\":[{\"Identity\":\"../Shared/"
           "Linked.cs\",\"Link\":\"Shared/"
           "Linked.cs\",\"DefiningProjectFullPath\":\"Library.csproj\"}],\"ProjectReference\":[],\"PackageReference\":["
           "],\"PackageVersion\":[{\"Identity\":\"Example.Central\",\"Version\":\"1.2.3\"}],\"Reference\":[],"
           "\"Analyzer\":[],\"AdditionalFiles\":[],\"EditorConfigFiles\":[]}}";
}
TEST(ManagedEvaluation, CapturesActualInnerTfmLinkedMetadataAndCentralOwnership) {
    const auto query = request();
    auto parsed = parseManagedProjectEvaluation(query, output(query));
    ASSERT_TRUE(parsed) << parsed.error().describe();
    EXPECT_EQ(parsed->targetFrameworks, (std::vector<std::string>{"net10.0", "netstandard2.1"}));
    ASSERT_EQ(parsed->items.at("Compile").size(), 1);
    EXPECT_EQ(parsed->items.at("Compile")[0].metadata.at("Link"), "Shared/Linked.cs");
    EXPECT_EQ(parsed->items.at("PackageVersion")[0].metadata.at("Version"), "1.2.3");
    EXPECT_EQ(parsed->properties.at("ManagePackageVersionsCentrally"), "true");
    auto spec = managedProjectEvaluationSpec(query);
    ASSERT_TRUE(spec);
    EXPECT_TRUE(spec->killOnClose);
    EXPECT_EQ(spec->arguments.front(), "msbuild");
    for (const auto& arg : spec->arguments) {
        EXPECT_NE(arg, "-restore");
        EXPECT_NE(arg, "build");
    }
}
TEST(ManagedEvaluation, RejectsAnotherProjectFrameworkAndNonSdkReports) {
    auto query = request();
    const auto wire = output(query);
    query.targetFramework.clear();
    EXPECT_FALSE(managedProjectEvaluationSpec(query));
    EXPECT_FALSE(parseManagedProjectEvaluation(query, wire));
    query.targetFramework = "netstandard2.1";
    EXPECT_FALSE(parseManagedProjectEvaluation(query, wire));
    query = request();
    query.project += ".other";
    EXPECT_FALSE(parseManagedProjectEvaluation(query, wire));
    query = request();
    auto nonSdk = wire;
    auto at = nonSdk.find("\"UsingMicrosoftNETSdk\":\"true\"");
    nonSdk.replace(at, std::string("\"UsingMicrosoftNETSdk\":\"true\"").size(), "\"UsingMicrosoftNETSdk\":\"false\"");
    EXPECT_FALSE(parseManagedProjectEvaluation(query, nonSdk));
    EXPECT_FALSE(parseManagedProjectEvaluation(query, std::string(32U * 1024U * 1024U + 1, 'x')));
}
} // namespace
} // namespace frost::build
