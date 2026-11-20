// SPDX-License-Identifier: Apache-2.0
// Strict scanner DTO tests. Synthetic protocol cases are not runtime feature evidence.
#include "frost/lang/cpp_modules.hpp"
#include <gtest/gtest.h>
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
namespace frost::lang {
TEST(CppModules, CapturedHeaderChangesAndNewInputsInvalidateWholeReceipt) {
    io::MemFs fs;
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/modules" : "/modules";
    const auto source = io::joinPath(root, "source.cppm").value();
    const auto header = io::joinPath(root, "conditional.h").value();
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(source, "module;\n#include \"conditional.h\"\nexport module m;\n"));
    ASSERT_TRUE(fs.writeFileAtomic(header, "#define IMPORT_ENABLED 1\n"));
    const std::vector<CompileCommand> commands{{root, source, {source + ".compiler", "-std=c++20", "-c", source}}};
    const auto database = compileCommandsJson(commands);
    auto receipt = captureCppModuleInputs(fs, root, database);
    ASSERT_TRUE(receipt);
    ASSERT_TRUE(validateCppModuleInputs(fs, *receipt));
    ASSERT_TRUE(fs.writeFileAtomic(header, "#define IMPORT_ENABLED 0\n"));
    EXPECT_FALSE(validateCppModuleInputs(fs, *receipt));
    receipt = captureCppModuleInputs(fs, root, database);
    ASSERT_TRUE(receipt);
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(root, "new-header.h").value(), "// new include\n"));
    EXPECT_FALSE(validateCppModuleInputs(fs, *receipt));
}
TEST(CppModules, AcceptsActualLlvmVersionBanner) {
    EXPECT_TRUE(validateCppModuleScannerVersion("/sdk/bin/clang-scan-deps\nLLVM (http://llvm.org/):\n  LLVM version 23.1.4\n  Optimized build.\n"));
    EXPECT_FALSE(validateCppModuleScannerVersion("LLVM version 23.1.40\n"));
    EXPECT_FALSE(validateCppModuleScannerVersion("LLVM version 22.1.4\n"));
    EXPECT_FALSE(validateCppModuleScannerVersion("clang-scan-deps version 23.1.4\n"));
}
TEST(CppModules, ResolvesOnlyScannerInterfaceProvider) {
    auto graph = parseCppModuleGraph(R"({"version":1,"revision":0,"rules":[
      {"primary-output":"math.o","provides":[{"logical-name":"math","source-path":"/src/actual.cppm","is-interface":true}]},
      {"primary-output":"main.o","requires":[{"logical-name":"math"},{"logical-name":"external"}]}]})");
    ASSERT_TRUE(graph);
    ASSERT_EQ(graph->rules.size(), 2U);
    EXPECT_EQ(graph->interfaceTargets(graph->rules[1].requiresModules[0]), std::vector<std::string>{"/src/actual.cppm"});
    EXPECT_TRUE(graph->interfaceTargets(graph->rules[1].requiresModules[1]).empty());
}
TEST(CppModules, RejectsMalformedWholeReport) {
    for (const auto report : {
        R"({"version":2,"revision":0,"rules":[]})",
        R"({"version":1,"revision":0,"rules":{}})",
        R"({"version":1,"revision":0,"rules":[{"primary-output":"a"},{"primary-output":"a"}]})",
        R"({"version":1,"revision":0,"rules":[{"primary-output":"a","provides":[{"logical-name":"m","is-interface":true}]}]})",
        R"({"version":1,"revision":0,"rules":[{"primary-output":"a","requires":[{"logical-name":"m","unique-on-source-path":true}]}]})"})
        EXPECT_FALSE(parseCppModuleGraph(report)) << report;
}
TEST(CppModules, AmbiguousProviderNeverInventsNavigation) {
    auto graph = parseCppModuleGraph(R"({"version":1,"revision":0,"rules":[
      {"primary-output":"one.o","provides":[{"logical-name":"m","source-path":"/one.cppm","is-interface":true}]},
      {"primary-output":"two.o","provides":[{"logical-name":"m","source-path":"/two.cppm","is-interface":true}]}]})");
    ASSERT_TRUE(graph);
    EXPECT_TRUE(graph->interfaceTargets({"m", "", "by-name", false}).empty());
    EXPECT_EQ(graph->interfaceTargets({"m", "/two.cppm", "include-quote", true}), std::vector<std::string>{"/two.cppm"});
}
} // namespace frost::lang
