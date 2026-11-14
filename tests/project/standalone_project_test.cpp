// SPDX-License-Identifier: Apache-2.0
// CMK-17 read-only folder recognition and literal Make planning; no external process needed.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/project/standalone_project.hpp"

#include <gtest/gtest.h>

namespace frost::project {
namespace {
std::string at(std::string_view name = {}) {
    return io::joinPath(io::kNativePathStyle == io::PathStyle::Windows ? "C:/project" : "/project", name).value();
}
TEST(StandaloneProject, RelativeDatabaseDirectoriesAndDuplicateTranslationUnits) {
    auto result = compilationDatabaseSources(R"([
      {"directory":"../src","file":"one.cpp","arguments":["c++","-c","one.cpp"]},
      {"directory":"../src","file":"one.cpp","command":"c++ -DOTHER -c one.cpp"}
    ])",
                                             at("build"));
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front(), at("src/one.cpp"));
}
TEST(StandaloneProject, DoesNotInferCompilerCommandsFromMalformedEntries) {
    EXPECT_FALSE(compilationDatabaseSources(R"({"file":"one.cpp"})", at()));
    EXPECT_FALSE(compilationDatabaseSources(R"([{"directory":".","file":"one.cpp"}])", at()));
    EXPECT_FALSE(compilationDatabaseSources(R"([{"directory":".","file":"one.cpp","arguments":[7]}])", at()));
    EXPECT_FALSE(
        compilationDatabaseSources(R"([{"directory":".","file":"one.cpp","file":"two.cpp","command":"c++"}])", at()));
}
TEST(StandaloneProject, DatabaseOnlyCannotInventBuildOrRunProducts) {
    StandaloneProject model{.root = at(), .database = at("compile_commands.json")};
    auto steps = makeProjectArguments(model, MakeAction::Build);
    ASSERT_FALSE(steps);
    EXPECT_EQ(steps.error().code(), ErrorCode::Unsupported);
}
TEST(StandaloneProject, RebuildIsOrderedCleanThenLiteralTarget) {
    StandaloneProject model{.root = at(), .makefile = at("GNUmakefile")};
    auto steps = makeProjectArguments(model, MakeAction::Rebuild, "target with spaces");
    ASSERT_TRUE(steps);
    ASSERT_EQ(steps->size(), 2u);
    EXPECT_EQ((*steps)[0], (std::vector<std::string>{"-f", *model.makefile, "clean"}));
    EXPECT_EQ((*steps)[1], (std::vector<std::string>{"-f", *model.makefile, "target with spaces"}));
    auto install = makeProjectArguments(model, MakeAction::Install);
    ASSERT_TRUE(install);
    EXPECT_EQ(install->front().back(), "install");
    EXPECT_FALSE(makeProjectArguments(model, MakeAction::Build, "--eval=evil"));
    EXPECT_FALSE(makeProjectArguments(model, MakeAction::Build, "SHELL=evil"));
    EXPECT_FALSE(makeProjectArguments(model, MakeAction::Build, std::string("bad\0target", 10)));
}
TEST(StandaloneProject, DiscoveryIsReadOnlyAndUsesGnuMakePrecedence) {
    io::MemFs fs(systemClock());
    ASSERT_TRUE(fs.createDirectories(at("build")));
    ASSERT_TRUE(fs.writeFileAtomic(at("Makefile"), "all:\n\t@echo second\n"));
    ASSERT_TRUE(fs.writeFileAtomic(at("GNUmakefile"), "all:\n\t@echo first\n"));
    const std::string database = R"([{"directory":".","file":"main.cpp","command":"c++ -c main.cpp"}])";
    ASSERT_TRUE(fs.writeFileAtomic(at("build/compile_commands.json"), database));
    auto model = readStandaloneProject(fs, at());
    ASSERT_TRUE(model);
    ASSERT_TRUE(*model);
    EXPECT_EQ((**model).makefile, at("GNUmakefile"));
    EXPECT_EQ((**model).database, at("build/compile_commands.json"));
    EXPECT_EQ(fs.readFile(at("build/compile_commands.json")).value(), database);
}
TEST(StandaloneProject, NinjaObjectTargetComesFromActualOutput) {
    auto target = compilationDatabaseObjectTarget(R"([
      {"directory":".","file":"../main.cpp","arguments":["c++","-o","CMakeFiles/app.dir/main.cpp.o","-c","../main.cpp"]}
    ])",
                                                  at("build"), at("main.cpp"));
    ASSERT_TRUE(target) << target.error().describe();
    EXPECT_EQ(*target, "CMakeFiles/app.dir/main.cpp.o");
    EXPECT_FALSE(compilationDatabaseObjectTarget(R"([
      {"directory":".","file":"../main.cpp","output":"../outside.o","command":"c++"}
    ])",
                                                 at("build"), at("main.cpp")));
    EXPECT_FALSE(compilationDatabaseObjectTarget(R"([
      {"directory":".","file":"../main.cpp","output":"one.o","command":"c++"},
      {"directory":".","file":"../main.cpp","output":"two.o","command":"c++"}
    ])",
                                                 at("build"), at("main.cpp")));
}
} // namespace
} // namespace frost::project
