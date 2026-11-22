// SPDX-License-Identifier: Apache-2.0
// Strict scanner DTO tests. Synthetic protocol cases are not runtime feature evidence.
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/cpp_modules.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include <gtest/gtest.h>
namespace frost::lang {
namespace {
std::string rootPath() { return io::kNativePathStyle == io::PathStyle::Windows ? "C:/module build" : "/module build"; }
std::string databasePath() { return io::joinPath(rootPath(), "compile_commands.json").value(); }
std::string compilerPath() { return io::joinPath(rootPath(), "clang++").value(); }

Json command(Json argv) {
    const auto root = rootPath();
    return Json::object()
        .set("directory", root)
        .set("file", io::joinPath(root, "source.cpp").value())
        .set("arguments", std::move(argv));
}

CppModuleScanRequest requestFor(const Json& entries) {
    const auto root = rootPath();
    CppModuleScanRequest request;
    request.scanner = io::joinPath(root, "clang-scan-deps").value();
    request.privateDatabasePath = databasePath();
    request.databaseBytes = entries.dump();
    request.workingDirectory = root;
    request.trusted = true;
    request.validateCurrent = [] -> Result<void> { return {}; };
    return request;
}
} // namespace
TEST(CppModules, AcceptsActualLlvmVersionBanner) {
    EXPECT_TRUE(validateCppModuleScannerVersion(
        "/sdk/bin/clang-scan-deps\nLLVM (http://llvm.org/):\n  LLVM version 23.1.4\n  Optimized build.\n"));
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
    EXPECT_EQ(graph->interfaceTargets(graph->rules[1].requiresModules[0]),
              std::vector<std::string>{"/src/actual.cppm"});
    EXPECT_TRUE(graph->interfaceTargets(graph->rules[1].requiresModules[1]).empty());
}
TEST(CppModules, RejectsMalformedWholeReport) {
    for (
        const auto report :
        {R"({"version":2,"revision":0,"rules":[]})", R"({"version":1,"revision":0,"rules":{}})",
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
    EXPECT_EQ(graph->interfaceTargets({"m", "/two.cppm", "include-quote", true}),
              std::vector<std::string>{"/two.cppm"});
}

TEST(CppModules, AcceptsExplicitOutputAndClangClObjectFlagWithoutChangingTheCapturedDatabase) {
    const auto root = rootPath();
    const auto database = databasePath();
    const auto compiler = compilerPath();
    const auto clangCl = io::joinPath(root, "clang-cl").value();
    const auto clOutput = io::joinPath(root, "file.obj").value();
    const auto explicitOutput = io::joinPath(root, "explicit.pcm").value();
    const auto request =
        requestFor(Json::array({command(Json::array({compiler, "-c", "source.cpp"})).set("output", "explicit.pcm"),
                                command(Json::array({clangCl, "/c", "source.cpp", "/Fofile.obj"}))}));
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(database, request.databaseBytes));
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = "LLVM version 23.1.4\n"});
    const auto report = Json::object()
                            .set("version", 1)
                            .set("revision", 0)
                            .set("rules", Json::array({Json::object().set("primary-output", explicitOutput),
                                                       Json::object().set("primary-output", clOutput)}));
    launcher.expect({.stdoutBytes = report.dump()});
    const auto graph = scanCppModules(launcher, fs, request);
    ASSERT_TRUE(graph) << graph.error().describe();
    EXPECT_EQ(graph->rules.size(), 2U);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    const auto derived = io::joinPath(root, "compile_commands.p1689.json").value();
    EXPECT_EQ(launches.back().spec.arguments,
              (std::vector<std::string>{"-format=p1689", "-compilation-database=" + derived}));
    EXPECT_FALSE(fs.exists(derived).value_or(true));
    EXPECT_EQ(fs.readFile(database).value_or(""), request.databaseBytes);
}

TEST(CppModules, RejectsMissingAmbiguousAndRepeatedOutputsBeforeStartingTheScanner) {
    const auto root = rootPath();
    const auto database = databasePath();
    const auto compiler = compilerPath();
    const auto duplicate = command(Json::array({compiler, "-c", "source.cpp", "-o", "result.o"}));
    for (const auto& entries : {Json::array({command(Json::array({compiler, "-c", "source.cpp"}))}),
                                Json::array({command(Json::array({compiler, "-o", "one.o", "-o", "two.o"}))}),
                                Json::array({command(Json::array({compiler, "-o", "one.o"})).set("output", "two.o")}),
                                Json::array({duplicate, command(Json::array({compiler, "-o", "./result.o"}))})}) {
        const auto request = requestFor(entries);
        ManualClock clock;
        io::MemFs fs(clock);
        ASSERT_TRUE(fs.createDirectories(root));
        ASSERT_TRUE(fs.writeFileAtomic(database, request.databaseBytes));
        io::ScriptedProcessLauncher launcher;
        EXPECT_FALSE(scanCppModules(launcher, fs, request)) << entries.dump();
        EXPECT_TRUE(launcher.launches().empty());
    }
}

TEST(CppModules, RefusesScannerReportThatOmitsACapturedOutput) {
    const auto root = rootPath();
    const auto database = databasePath();
    const auto compiler = compilerPath();
    const auto request = requestFor(Json::array(
        {command(Json::array({compiler, "-o", "one.o"})), command(Json::array({compiler, "-o", "two.o"}))}));
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.writeFileAtomic(database, request.databaseBytes));
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.stdoutBytes = "LLVM version 23.1.4\n"});
    const auto output = io::joinPath(root, "one.o").value();
    const auto report = Json::object()
                            .set("version", 1)
                            .set("revision", 0)
                            .set("rules", Json::array({Json::object().set("primary-output", output)}));
    launcher.expect({.stdoutBytes = report.dump()});
    const auto graph = scanCppModules(launcher, fs, request);
    ASSERT_FALSE(graph);
    EXPECT_EQ(graph.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(launcher.launches().size(), 2U);
}

TEST(CppModules, LiteralCommandParserPreservesQuotingAndRefusesShellInterpretation) {
    using project::CMakeShell;
    const auto windows = project::parseCompileCommandInvocation(
        R"("C:\Program Files\clang-cl.exe" /Fo"object directory\unit.obj" /c "source file.cpp")", CMakeShell::Windows);
    ASSERT_TRUE(windows);
    EXPECT_EQ(*windows, (std::vector<std::string>{R"(C:\Program Files\clang-cl.exe)", R"(/Foobject directory\unit.obj)",
                                                  "/c", "source file.cpp"}));
    const auto posix = project::parseCompileCommandInvocation(
        R"('/usr/bin/clang++' -DVALUE='literal $1' -o 'object directory/unit.o')", CMakeShell::Posix);
    ASSERT_TRUE(posix);
    EXPECT_EQ(*posix,
              (std::vector<std::string>{"/usr/bin/clang++", "-DVALUE=literal $1", "-o", "object directory/unit.o"}));
    for (const auto text : {R"(/usr/bin/clang++ -o "$OUT")", R"(/usr/bin/clang++ -o 'unclosed)",
                            R"(/usr/bin/clang++ -o object.o && true)", "/usr/bin/clang++ -c source.cpp\\"})
        EXPECT_FALSE(project::parseCompileCommandInvocation(text, CMakeShell::Posix)) << text;
    for (const auto text : {R"(C:\clang-cl.exe /Fo"%OUT%")", R"(C:\clang-cl.exe /Fo"unclosed)",
                            R"(C:\clang-cl.exe /Foout.obj & echo unexpected)"})
        EXPECT_FALSE(project::parseCompileCommandInvocation(text, CMakeShell::Windows)) << text;
}
} // namespace frost::lang
