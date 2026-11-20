// SPDX-License-Identifier: Apache-2.0
// RUN-13 deferred installed-OS-profiler hot-function oracle; permissions/tools must be provisioned.
#include "frost/build/run_target.hpp"
#include "frost/io/path.hpp"
#include "frost/run/native_analysis.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <cstdlib>
namespace frost::run {
TEST(NativeProfileReal, ActualOsSampleReportIdentifiesTheFixtureHotFunction) {
    frost::test::ScratchDir scratch("native-profile-real");
    auto& fs = io::realFileSystem();
    const auto source = scratch.join("hot.cpp");
    const auto binary = scratch.join(
#ifdef _WIN32
        "hot.exe"
#else
        "hot"
#endif
    );
    ASSERT_TRUE(fs.writeFileAtomic(source,
                                   "#include <chrono>\n#include <cstdint>\nvolatile std::uint64_t sink;\n"
                                   "__attribute__((noinline)) std::uint64_t hotFunction(std::uint64_t value){for(int "
                                   "i=0;i<100000;++i)value=value*1664525+1013904223;return value;}\n"
                                   "int main(){auto "
                                   "end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(std::chrono::"
                                   "steady_clock::now()<end)sink=hotFunction(sink);}\n"));
    io::ProcessSpec compile;
    compile.executable = io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    compile.workingDirectory = scratch.path();
    compile.arguments = {"--driver-mode=g++",       "-std=c++20", "-O0", "-g",
                         "-fno-omit-frame-pointer", source,       "-o",  binary};
#ifdef _WIN32
    compile.arguments.insert(compile.arguments.end(), {"-gcodeview", "-Wl,/debug"});
#endif
    auto built = runAnalysisCommand(io::realProcessLauncher(), compile, {});
    ASSERT_TRUE(built);
    ASSERT_EQ(built->exitCode, 0) << built->error;
    NativeProfileRequest request;
    request.plan = {binary, {}, scratch.path(), {}};
    request.outputDirectory = scratch.join("samples");
    request.duration = std::chrono::seconds(5);
#ifdef _WIN32
    request.backend = NativeProfileBackend::WindowsEtw;
    request.systemWideEtwConsent = true;
    request.sessionName = "FrostIDE-" + std::string(FROST_TEST_PROFILE_UUID);
    auto system = build::systemCommandProcessor();
    ASSERT_TRUE(system);
    request.wpr = io::joinPath(io::parentPath(*system), "wpr.exe").value();
    const auto* xperf = std::getenv("FROST_TEST_XPERF");
    ASSERT_NE(xperf, nullptr)
        << "Provision installed Windows Performance Toolkit and recording permission; this is not a skipped pass";
    request.xperf = xperf;
#else
    request.backend = NativeProfileBackend::Perf;
    request.perf = "/usr/bin/perf";
#endif
    auto report = collectNativeProfile(fs, io::realProcessLauncher(), request);
    ASSERT_TRUE(report) << report.error().describe();
    EXPECT_NE(report->toolReport.find("hotFunction"), std::string::npos) << report->toolReport << report->diagnostics;
    EXPECT_TRUE(validateAnalysisIdentity(fs, report->trace));
    EXPECT_TRUE(validateAnalysisIdentity(fs, report->binary));
}
} // namespace frost::run
