// SPDX-License-Identifier: Apache-2.0
// Deferred real pinned-tool oracle: scanner relationships, actual module compilation and navigation.
#include "frost/io/path.hpp"
#include "frost/lang/cpp_modules.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::lang {
TEST(CppModulesReal, ScannerAcceptsCMakeGeneratedCommandWithAndWithoutExplicitOutput) {
    frost::test::ScratchDir scratch("cpp modules cmake");
    auto& fs = io::realFileSystem();
    const auto root = scratch.path();
    const auto compilerDir = std::string(io::parentPath(FROST_TEST_CXX_COMPILER));
#ifdef _WIN32
    const auto compiler = io::joinPath(compilerDir, "clang-cl.exe").value();
    const auto scanner = io::joinPath(compilerDir, "clang-scan-deps.exe").value();
#else
    const auto compiler = io::joinPath(compilerDir, "clang++").value();
    const auto scanner = io::joinPath(compilerDir, "clang-scan-deps").value();
#endif
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("source file.cpp"), "int value() { return 42; }\n"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("CMakeLists.txt"), R"cmake(
cmake_minimum_required(VERSION 3.25)
project(ScannerCommands LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_library(scanner_commands OBJECT "source file.cpp")
target_compile_definitions(scanner_commands PRIVATE LABEL="quoted value")
)cmake"));
    const auto build = scratch.join("cmake build");
    io::ProcessSpec configure;
    configure.executable = FROST_TEST_CMAKE;
    configure.workingDirectory = root;
    configure.arguments = {"-S",
                           root,
                           "-B",
                           build,
                           "-G",
                           "Ninja",
                           "-DCMAKE_MAKE_PROGRAM=" FROST_TEST_NINJA,
                           "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                           "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
                           "-DCMAKE_CXX_COMPILER=" + compiler};
    const auto configured = io::runToCompletion(io::realProcessLauncher(), configure, "", std::chrono::seconds(60));
    ASSERT_TRUE(configured) << configured.error().describe();
    ASSERT_EQ(configured->exitCode, 0) << configured->stdoutBytes << configured->stderrBytes;
    const auto captured = fs.readFile(io::joinPath(build, "compile_commands.json").value());
    ASSERT_TRUE(captured);
    const auto original = parseJson(*captured);
    ASSERT_TRUE(original);
    ASSERT_TRUE(original->isArray());
    ASSERT_EQ(original->size(), 1U);
    const auto& command = original->items().front();
    ASSERT_FALSE(command.stringAt("command").empty());
    ASSERT_FALSE(command.stringAt("output").empty());

    const auto privateDir = scratch.join("private");
    ASSERT_TRUE(fs.createDirectories(privateDir));
    CppModuleScanRequest request;
    request.scanner = scanner;
    request.privateDatabasePath = io::joinPath(privateDir, "compile_commands.json").value();
    request.workingDirectory = root;
    request.trusted = true;
    request.validateCurrent = [] -> Result<void> { return {}; };
    request.databaseBytes = *captured;
    ASSERT_TRUE(fs.writeFileAtomic(request.privateDatabasePath, request.databaseBytes));
    const auto explicitGraph = scanCppModules(io::realProcessLauncher(), fs, request);
    ASSERT_TRUE(explicitGraph) << explicitGraph.error().describe();
    ASSERT_EQ(explicitGraph->rules.size(), 1U);

    auto omitted = Json::array({Json::object()
                                    .set("directory", command.stringAt("directory"))
                                    .set("file", command.stringAt("file"))
                                    .set("command", command.stringAt("command"))});
    request.databaseBytes = omitted.dump();
    ASSERT_TRUE(fs.writeFileAtomic(request.privateDatabasePath, request.databaseBytes));
    const auto inferredGraph = scanCppModules(io::realProcessLauncher(), fs, request);
    ASSERT_TRUE(inferredGraph) << inferredGraph.error().describe();
    ASSERT_EQ(inferredGraph->rules.size(), 1U);
    EXPECT_EQ(inferredGraph->rules.front().primaryOutput, explicitGraph->rules.front().primaryOutput);
    EXPECT_EQ(fs.readFile(request.privateDatabasePath).value_or(""), request.databaseBytes);
}

TEST(CppModulesReal, ScannerGraphMatchesBuiltModuleFixture) {
    frost::test::ScratchDir scratch("cpp-modules");
    auto& fs = io::realFileSystem();
    const auto root = scratch.path();
    const auto module = io::joinPath(root, "different-filename.cppm").value();
    const auto source = io::joinPath(root, "consumer.cpp").value();
    const auto bmi = io::joinPath(root, "fixture.pcm").value();
    const auto output = io::joinPath(root, "consumer.o").value();
    const auto databasePath = io::joinPath(root, "compile_commands.json").value();
    const auto compilerDir = std::string(io::parentPath(FROST_TEST_CXX_COMPILER));
#ifdef _WIN32
    const auto compiler = io::joinPath(compilerDir, "clang++.exe").value();
    const auto scanner = io::joinPath(compilerDir, "clang-scan-deps.exe").value();
#else
    const auto compiler = io::joinPath(compilerDir, "clang++").value();
    const auto scanner = io::joinPath(compilerDir, "clang-scan-deps").value();
#endif
    // The declaration name intentionally differs from the file. Only scanner evidence links it.
    const std::string moduleBytes = "export module fixture_math;\nexport int answer() { return 42; }\n";
    const std::string sourceBytes = "import fixture_math;\nint consume() { return answer(); }\n";
    ASSERT_TRUE(fs.writeFileAtomic(module, moduleBytes));
    ASSERT_TRUE(fs.writeFileAtomic(source, sourceBytes));
    const std::vector<CompileCommand> commands{{root, module, {compiler, "-std=c++20", "-c", module, "-o", bmi}},
                                               {root, source, {compiler, "-std=c++20", "-c", source, "-o", output}}};
    const auto bytes = compileCommandsJson(commands);
    ASSERT_TRUE(fs.writeFileAtomic(databasePath, bytes));
    CppModuleScanRequest request;
    request.scanner = scanner;
    request.privateDatabasePath = databasePath;
    request.databaseBytes = bytes;
    request.workingDirectory = root;
    request.trusted = true;
    request.validateCurrent = [&]() -> Result<void> {
        auto m = fs.readFileSnapshot(module, 4096);
        auto s = fs.readFileSnapshot(source, 4096);
        if (!m || !s || *m != moduleBytes || *s != sourceBytes)
            return fail(ErrorCode::Cancelled, "Fixture source changed");
        return {};
    };
    auto graph = scanCppModules(io::realProcessLauncher(), fs, request);
    ASSERT_TRUE(graph) << graph.error().describe();
    // Independent raw scanner DTO is the relationship oracle; no hand-written graph count.
    auto raw = parseJson(graph->rawP1689);
    ASSERT_TRUE(raw);
    const auto* rawRules = raw->find("rules");
    ASSERT_NE(rawRules, nullptr);
    std::vector<std::string> expectedProviders, expectedImports;
    for (const auto& rawRule : rawRules->items()) {
        if (const auto* provides = rawRule.find("provides"))
            for (const auto& provider : provides->items())
                expectedProviders.push_back(std::string(provider.stringAt("logical-name")) + "=" +
                                            std::string(provider.stringAt("source-path")));
        if (const auto* imports = rawRule.find("requires"))
            for (const auto& imported : imports->items())
                expectedImports.push_back(std::string(imported.stringAt("logical-name")));
    }
    std::vector<std::string> actualProviders, actualImports;
    for (const auto& rule : graph->rules) {
        for (const auto& provider : rule.provides)
            actualProviders.push_back(provider.name + "=" + provider.sourcePath);
        for (const auto& dependency : rule.requiresModules)
            actualImports.push_back(dependency.name);
    }
    EXPECT_EQ(actualProviders, expectedProviders);
    EXPECT_EQ(actualImports, expectedImports);
    bool foundImport = false;
    for (const auto& rule : graph->rules)
        for (const auto& dependency : rule.requiresModules)
            if (dependency.name == "fixture_math") {
                foundImport = true;
                EXPECT_EQ(graph->interfaceTargets(dependency), std::vector<std::string>{module});
            }
    EXPECT_TRUE(foundImport);
    io::ProcessSpec spec;
    spec.executable = compiler;
    spec.workingDirectory = root;
    spec.arguments = {"-std=c++20", "--precompile", module, "-o", bmi};
    auto precompile = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(60));
    ASSERT_TRUE(precompile);
    ASSERT_EQ(precompile->exitCode, 0) << precompile->stderrBytes;
    spec.arguments = {"-std=c++20", "-fmodule-file=fixture_math=" + bmi, "-c", source, "-o", output};
    auto compile = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(60));
    ASSERT_TRUE(compile);
    EXPECT_EQ(compile->exitCode, 0) << compile->stderrBytes;
}
} // namespace frost::lang
