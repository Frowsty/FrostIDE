// SPDX-License-Identifier: Apache-2.0
// Authored CPP-8 actual frontend/configuration/relations and no-output integration cases; not run.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/cpp_index.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <filesystem>

namespace {
using frost::lang::Json;
TEST(SemanticIndex, RealConfigurationsKeepDifferentSymbolsAndCanonicalOverrideRelations) {
    frost::test::ScratchDir scratch("semantic-index");
    const auto source = scratch.join("unit.cpp");
    auto& fs = frost::io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(scratch.join("build")));
    ASSERT_TRUE(fs.createDirectories(scratch.join("include")));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("include/settings.hpp"), "struct Included{int count;};\n"));
    ASSERT_TRUE(fs.writeFileAtomic(source,
                                   "#include <settings.hpp>\n"
                                   "struct Base{virtual int value()const{return 0;}};\n"
                                   "struct Derived:Base{int value()const override{return 1;}};\n"
                                   "#if MODE\nint enabled(){return 1;}\n#else\nint disabled(){return 2;}\n#endif\n"
                                   "int use(){Derived d;return d.value();}\n"));
    const auto compiler = frost::io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    std::vector<Json> rows;
    for (const auto mode : {"1", "0"}) {
        std::vector<Json> arguments{Json(compiler)};
#ifdef _WIN32
        for (const auto& argument :
             std::vector<std::string>{"/std:c++20", "/I../include", "/DMODE=" + std::string(mode), "/c", source,
                                      "/Fo" + scratch.join("forbidden.obj")})
            arguments.emplace_back(argument);
#else
        for (const auto& argument :
             std::vector<std::string>{"-std=c++20", "-I../include", "-DMODE=" + std::string(mode), "-c", source, "-o",
                                      scratch.join("forbidden.o"), "-MMD", "-MF", scratch.join("forbidden.d")})
            arguments.emplace_back(argument);
#endif
        rows.push_back(Json::object()
                           .set("directory", scratch.join("build"))
                           .set("file", source)
                           .set("arguments", Json::array(std::move(arguments))));
    }
    const auto database = Json::array(std::move(rows)).dump();
    std::vector<frost::lang::CppIndexUnit> units;
    for (std::uint64_t index = 0; index < 2; ++index) {
        auto request = Json::object()
                           .set("projectRoot", scratch.path())
                           .set("configuration", "same-label")
                           .set("compilationDatabase", database)
                           .set("commandIndex", index);
        frost::io::ProcessSpec spec;
        spec.executable = frost::io::normalizePath(FROST_CLANG_BINARY).value();
        spec.arguments = {"--index-unit"};
        spec.workingDirectory = scratch.path();
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.environment.emplace_back("FROST_LLVM_ROOT",
                                      std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto output =
            frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, request.dump(), std::chrono::minutes(5));
        ASSERT_TRUE(output);
        ASSERT_EQ(output->exitCode, 0) << output->stdoutBytes << output->stderrBytes;
        auto unit = frost::lang::parseCppIndexUnit(output->stdoutBytes);
        ASSERT_TRUE(unit) << output->stdoutBytes;
        const auto present = [&](std::string_view name) {
            for (const auto& symbol : unit->data.find("symbols")->items())
                if (symbol.stringAt("name") == name)
                    return true;
            return false;
        };
        EXPECT_EQ(present("enabled"), index == 0);
        EXPECT_EQ(present("disabled"), index == 1);
        EXPECT_TRUE(frost::lang::validateCppIndexInputs(fs, *unit, {}));
        bool headerLocation = false;
        for (const auto& occurrence : unit->data.find("occurrences")->items())
            headerLocation |= occurrence.stringAt("path") == scratch.join("include/settings.hpp");
        EXPECT_TRUE(headerLocation);
        bool overrides = false;
        for (const auto& relation : unit->data.find("relations")->items())
            overrides |= (*relation.find("roles")->asUnsigned() & (1U << 12)) != 0;
        EXPECT_TRUE(overrides);
        units.push_back(std::move(*unit));
    }
    EXPECT_NE(units[0].commandIdentity, units[1].commandIdentity);
    EXPECT_FALSE(fs.exists(scratch.join("forbidden.obj")).value_or(true));
    EXPECT_FALSE(fs.exists(scratch.join("forbidden.o")).value_or(true));
    EXPECT_FALSE(fs.exists(scratch.join("forbidden.d")).value_or(true));
}
} // namespace
