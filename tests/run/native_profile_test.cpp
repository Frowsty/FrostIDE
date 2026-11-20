// SPDX-License-Identifier: Apache-2.0
// RUN-13 ownership/identity seams. Synthetic process bytes are not profiling acceptance evidence.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/native_analysis.hpp"

#include <gtest/gtest.h>
namespace frost::run {
class ProfileLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher processes;
    std::function<void(const io::ProcessSpec&)> observe;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (observe)
            observe(spec);
        return processes.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return processes.launchAsync(spec, context);
    }
};
#ifdef _WIN32
TEST(NativeProfile, CancellationDuringRecorderStartupRetiresOnlyItsOwnedInstance) {
    ManualClock clock;
    io::MemFs fs(clock);
    ProfileLauncher launcher;
    std::stop_source cancel;
    ASSERT_TRUE(fs.createDirectories("C:/tools"));
    ASSERT_TRUE(fs.writeFileAtomic("C:/tools/wpr.exe", "installed recorder"));
    ASSERT_TRUE(fs.writeFileAtomic("C:/tools/xperf.exe", "installed analyzer"));
    ASSERT_TRUE(fs.writeFileAtomic("C:/tools/app.exe", "program"));
    NativeProfileRequest request;
    request.backend = NativeProfileBackend::WindowsEtw;
    request.wpr = "C:/tools/wpr.exe";
    request.xperf = "C:/tools/xperf.exe";
    request.plan = {"C:/tools/app.exe", {}, "C:/tools", {}};
    request.outputDirectory = "C:/new-session";
    request.systemWideEtwConsent = true;
    request.sessionName = "FrostIDE-01234567-89ab-cdef-0123-456789abcdef";
    launcher.processes.expect({.expectExecutable = request.wpr});
    launcher.processes.expect({.expectExecutable = request.wpr});
    launcher.observe = [&](const auto& spec) {
        if (spec.arguments.front() == "-start")
            cancel.request_stop();
    };
    auto result = collectNativeProfile(fs, launcher, request, cancel.get_token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    const auto launches = launcher.processes.launches();
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_EQ(launches[0].spec.arguments,
              (std::vector<std::string>{"-start", "CPU", "-filemode", "-instancename", request.sessionName}));
    EXPECT_EQ(launches[1].spec.arguments, (std::vector<std::string>{"-cancel", "-instancename", request.sessionName}));
}
#else
TEST(NativeProfile, ChangedTraceDuringPerfReportCannotPublish) {
    ManualClock clock;
    io::MemFs fs(clock);
    ProfileLauncher launcher;
    ASSERT_TRUE(fs.createDirectories("/tools"));
    ASSERT_TRUE(fs.writeFileAtomic("/tools/perf", "installed perf"));
    ASSERT_TRUE(fs.writeFileAtomic("/tools/app", "program"));
    NativeProfileRequest request;
    request.perf = "/tools/perf";
    request.plan = {"/tools/app", {}, "/tools", {}};
    request.outputDirectory = "/new-session";
    launcher.processes.expect({.expectExecutable = request.perf});
    launcher.processes.expect({.expectExecutable = request.perf, .stdoutBytes = "actual report fixture\n"});
    launcher.observe = [&](const auto& spec) {
        ASSERT_TRUE(fs.writeFileAtomic("/new-session/perf.data",
                                       spec.arguments.front() == "record" ? "first trace" : "changed trace"));
    };
    auto result = collectNativeProfile(fs, launcher, request);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
}
#endif
} // namespace frost::run
