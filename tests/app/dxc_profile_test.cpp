// SPDX-License-Identifier: Apache-2.0
// Checks shader compiler arguments, diagnostic parsing, and trust gating.
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "../../src/app/src/dxc_profile.hpp"
#include "../../src/app/src/trusted_project_launcher.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::app {
#ifndef FROST_TEST_DXC
namespace {
DxcProfile profile() {
#ifdef _WIN32
    return {"C:/installed DXC/dxc.exe", "C:/shader project", "C:/shader project/test.hlsl", "main", "ps_6_0"};
#else
    return {"/installed/dxc", "/shader project", "/shader project/test.hlsl", "main", "ps_6_0"};
#endif
}
} // namespace
TEST(DxcProfile, RefusesImplicitCompilerInventedEntryAndShaderLabVariants) {
    auto p = profile();
    p.executable = "dxc";
    EXPECT_FALSE(dxcSpec(p));
    p = profile();
    p.source += ".shader";
    EXPECT_FALSE(dxcSpec(p));
    p = profile();
    p.entry = "main -Fo injected";
    EXPECT_FALSE(dxcSpec(p));
    p = profile();
    p.target = "ps_5_0";
    EXPECT_FALSE(dxcSpec(p));
}
TEST(DxcProfile, ActualCompilerFormatLocationsRemainMessagesWithoutInferredSuccess) {
    auto p = profile();
    auto rows = parseDxcDiagnostics("test.hlsl(4,9) :  error: use of undeclared identifier 'actual'\r\n"
                                    "    return actual;\r\n"
                                    "           ^\r\n",
                                    p.root);
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ(rows->front().diagnostic.line, 4);
    EXPECT_EQ(rows->front().diagnostic.column, 9);
    EXPECT_EQ(rows->front().diagnostic.severity, build::Severity::Error);
    EXPECT_TRUE(rows->front().jumpable);
    EXPECT_EQ(rows->front().path, p.source);
    EXPECT_EQ(rows->front().diagnostic.message, "use of undeclared identifier 'actual'");
}
TEST(DxcProfile, RealCommandRecipeUsesExactArgvAndBoundedDiagnosticCollection) {
    io::ScriptedProcessLauncher launcher;
    io::ScriptedProcess script;
    script.exitCode = 1;
    script.stderrBytes = "test.hlsl(2,3) :  error: actual adapter output\r\n";
    launcher.expect(std::move(script));
    auto p = profile();
    auto report = runDxc(launcher, p);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exitCode, 1);
    ASSERT_EQ(report->diagnostics.size(), 1u);
    const auto records = launcher.launches();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].spec.executable, p.executable);
    EXPECT_FALSE(records[0].spec.rawArguments);
    const std::vector<std::string> expectedArguments{
        "-E",    p.entry, "-T", p.target, "-HV", "2021", "-fdiagnostics-format=msvc", "-Fo",
#ifdef _WIN32
        "NUL",
#else
        "/dev/null",
#endif
        p.source};
    EXPECT_EQ(records[0].spec.arguments, expectedArguments);
    EXPECT_EQ(records[0].spec.stderrMode, io::Stdio::MergeIntoStdout);
    script = {};
    script.stdoutBytes = std::string(1024 * 1024 + 1, 'x');
    launcher.expect(std::move(script));
    EXPECT_FALSE(runDxc(launcher, p));
    EXPECT_TRUE(launcher.launches().back().terminated);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(runDxc(launcher, p, stop.get_token()));
    EXPECT_EQ(launcher.launches().size(), 2u);
}
TEST(DxcProfile, UntrustedOrRevokedProjectNeverLaunchesTheCompiler) {
    io::MemFs fs(systemClock());
    auto p = profile();
    ASSERT_TRUE(fs.createDirectories(p.root));
    auto trust = project::TrustStore::open(fs, p.root + "/trust.txt");
    ASSERT_TRUE(trust);
    io::ScriptedProcessLauncher launcher;
    auto denied = (*trust)->snapshot(p.root);
    TrustedProjectLauncher before(launcher, **trust, p.root, denied.revision);
    EXPECT_FALSE(runDxc(before, p));
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE((*trust)->trust(p.root));
    auto granted = (*trust)->snapshot(p.root);
    TrustedProjectLauncher captured(launcher, **trust, p.root, granted.revision);
    ASSERT_TRUE((*trust)->untrust(p.root));
    ASSERT_TRUE((*trust)->trust(p.root));
    EXPECT_FALSE(runDxc(captured, p));
    EXPECT_TRUE(launcher.launches().empty());
}
#else
TEST(DxcInstalledCompiler, DiagnosesActualSavedHlslThroughRealCommand) {
    frost::test::ScratchDir scratch("installed-dxc");
    auto& fs = io::realFileSystem();
    DxcProfile p{FROST_TEST_DXC, scratch.path(), scratch.path() + "/broken.hlsl", "main", "ps_6_0"};
    ASSERT_TRUE(fs.writeFileAtomic(p.source, "float4 main() : SV_Target { return actual_missing_identifier; }\n"));
    auto report = runDxc(io::realProcessLauncher(), p);
    ASSERT_TRUE(report) << report.error().message();
    EXPECT_NE(report->exitCode, 0);
    ASSERT_FALSE(report->diagnostics.empty()) << report->output;
    EXPECT_EQ(report->diagnostics.front().diagnostic.line, 1);
    EXPECT_NE(report->diagnostics.front().diagnostic.message.find("actual_missing_identifier"), std::string::npos);
}
#endif
} // namespace frost::app
