// SPDX-License-Identifier: Apache-2.0
// Explicit later local EventPipe oracle: never executed during implementation/source review.
#include "frost/debug/coreclr_dap.hpp"
#include "frost/run/managed_diagnostics.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>
namespace frost::run {
TEST(ManagedDiagnosticsReal, AttachedToolsProduceIndependentlyReadableArtifactsAndLeaveTargetAlive) {
    test::ScratchDir scratch("managed-diagnostics");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("Fixture.csproj"),
                                   "<Project Sdk=\"Microsoft.NET.Sdk\"><PropertyGroup><OutputType>Exe</OutputType>"
                                   "<TargetFramework>net10.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>"
                                   "</PropertyGroup></Project>"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("NuGet.Config"),
                                   "<configuration><packageSources><clear/></packageSources></configuration>"));
    ASSERT_TRUE(fs.writeFileAtomic(
        scratch.join("Program.cs"),
        "var held = new System.Collections.Generic.List<byte[]>(); while(true) { "
        "held.Add(new byte[4096]); if(held.Count>2048) held.Clear(); System.Threading.Thread.Sleep(2); }"));
    io::ProcessSpec build;
    build.executable = FROST_DIAGNOSTIC_DOTNET;
    build.workingDirectory = scratch.path();
    build.arguments = {"build", scratch.join("Fixture.csproj"), "--configuration", "Debug", "--nologo"};
    auto built = runAnalysisCommand(io::realProcessLauncher(), build, {});
    ASSERT_TRUE(built);
    ASSERT_EQ(built->exitCode, 0) << built->output << built->error;
    io::ProcessSpec launch;
    launch.executable = FROST_DIAGNOSTIC_DOTNET;
    launch.workingDirectory = scratch.path();
    launch.arguments = {scratch.join("bin/Debug/net10.0/Fixture.dll")};
    launch.killOnClose = true;
    auto target = build::RunSession::start(io::realProcessLauncher(), launch, {});
    ASSERT_TRUE(target);
    auto identity =
        debug::coreClrProcessAdmission(static_cast<std::uint32_t>((*target)->processId()), launch.executable);
    ASSERT_TRUE(identity);
    const std::pair<ManagedDiagnosticKind, const char*> tools[] = {
        {ManagedDiagnosticKind::Trace, FROST_DIAGNOSTIC_TRACE},
        {ManagedDiagnosticKind::Counters, FROST_DIAGNOSTIC_COUNTERS},
        {ManagedDiagnosticKind::GcDump, FROST_DIAGNOSTIC_GCDUMP}};
    unsigned sequence = 0;
    for (auto [kind, tool] : tools) {
        io::ProcessSpec version;
        version.executable = tool;
        version.workingDirectory = scratch.path();
        version.arguments = {"--version"};
        auto actual = runAnalysisCommand(io::realProcessLauncher(), version, {});
        ASSERT_TRUE(actual);
        ASSERT_EQ(actual->exitCode, 0);
        auto token = actual->output;
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r'))
            token.pop_back();
        ManagedDiagnosticRequest request;
        request.kind = kind;
        request.tool = tool;
        request.expectedVersion = token;
        request.workingDirectory = scratch.path();
        request.outputDirectory = scratch.join("capture-" + std::to_string(sequence++));
        request.processId = static_cast<std::uint32_t>((*target)->processId());
        request.durationSeconds = kind == ManagedDiagnosticKind::GcDump ? 60 : 3;
        request.allowFullGc = true;
        request.admit = *identity;
        auto report = collectManagedDiagnostics(fs, io::realProcessLauncher(), request);
        ASSERT_TRUE(report) << report.error().describe();
        ASSERT_FALSE(report->report.empty());
        ASSERT_TRUE((*identity)());
        EXPECT_FALSE((*target)->finished());
        if (kind == ManagedDiagnosticKind::Counters) {
            auto csv = fs.readFileSnapshot(report->artifact.path, 32 * 1024 * 1024);
            ASSERT_TRUE(csv);
            EXPECT_EQ(report->report, *csv);
        } else {
            io::ProcessSpec oracle = version;
            oracle.arguments = {"report", report->artifact.path};
            if (kind == ManagedDiagnosticKind::Trace)
                oracle.arguments.insert(oracle.arguments.end(), {"topN", "--number", "50", "--inclusive", "--verbose"});
            auto independent = runAnalysisCommand(io::realProcessLauncher(), oracle, {});
            ASSERT_TRUE(independent);
            ASSERT_EQ(independent->exitCode, 0);
            EXPECT_EQ(report->report, independent->output);
        }
        request.outputDirectory = scratch.join("revoked-" + std::to_string(sequence));
        request.admit = []() -> Result<void> { return fail(ErrorCode::Cancelled, "Target lease revoked"); };
        EXPECT_FALSE(collectManagedDiagnostics(fs, io::realProcessLauncher(), request));
        EXPECT_FALSE((*target)->finished());
    }
    (*target)->stop();
    EXPECT_FALSE((*identity)());
}
} // namespace frost::run
