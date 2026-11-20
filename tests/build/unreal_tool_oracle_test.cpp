// SPDX-License-Identifier: Apache-2.0
// Deferred actual installed-tool oracle. Target exists only under explicit CMake opt-in.
#include "frost/build/unreal_build.hpp"
#include "frost/io/file_system.hpp"
#include "frost/project/standalone_project.hpp"
#include <gtest/gtest.h>
#include <chrono>
namespace frost::build {
TEST(UnrealInstalledOracle, ActualUbtBuildThenGeneratedHeaderAndCompilationDatabase) {
    UnrealBuildRequest request{.tool=FROST_UNREAL_TOOL, .buildToolAssembly=FROST_UNREAL_UBT_ASSEMBLY,
        .projectFile=FROST_UNREAL_PROJECT, .target=FROST_UNREAL_TARGET};
#ifndef _WIN32
    request.platform = UnrealPlatform::Linux;
#endif
    auto spec = unrealBuildSpec(request);
    ASSERT_TRUE(spec) << spec.error().describe();
    auto build = io::runToCompletion(io::realProcessLauncher(), *spec, {}, std::chrono::minutes(30));
    ASSERT_TRUE(build) << build.error().describe();
    ASSERT_EQ(build->exitCode, 0) << build->stdoutBytes << build->stderrBytes;
    ASSERT_EQ(build->signal, 0);
    auto& fs = io::realFileSystem();
    auto generated = fs.status(FROST_UNREAL_GENERATED_HEADER);
    ASSERT_TRUE(generated) << "UHT did not produce the explicitly selected generated header";
    EXPECT_EQ(generated->type, io::FileType::Regular);
    EXPECT_GT(generated->size, 0U);
    request.operation = UnrealOperation::GenerateCompileDatabase;
    spec = unrealBuildSpec(request);
    ASSERT_TRUE(spec);
    auto database = io::runToCompletion(io::realProcessLauncher(), *spec, {}, std::chrono::minutes(30));
    ASSERT_TRUE(database) << database.error().describe();
    ASSERT_EQ(database->exitCode, 0) << database->stdoutBytes << database->stderrBytes;
    ASSERT_EQ(database->signal, 0);
    auto bytes = fs.readFile(FROST_UNREAL_COMPILE_DATABASE);
    ASSERT_TRUE(bytes) << bytes.error().describe();
    auto sources = project::compilationDatabaseSources(*bytes, io::parentPath(FROST_UNREAL_COMPILE_DATABASE));
    ASSERT_TRUE(sources) << sources.error().describe();
    ASSERT_FALSE(sources->empty());
}
} // namespace frost::build
