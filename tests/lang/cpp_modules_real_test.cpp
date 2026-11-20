// SPDX-License-Identifier: Apache-2.0
// Deferred real pinned-tool oracle: scanner relationships, actual module compilation and navigation.
#include "frost/lang/cpp_modules.hpp"
#include "frost/io/path.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::lang {
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
    const std::vector<CompileCommand> commands{
        {root, module, {compiler, "-std=c++20", "-c", module, "-o", bmi}},
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
