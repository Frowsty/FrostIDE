// SPDX-License-Identifier: Apache-2.0
//
// Unit and integration tests for frost::sched::ResourceGovernor (FND-18): live hardware probing,
// real child-process RSS sampling, governed clangd/build concurrency scaling and throttling when
// heavy legs are active, in-process + cross-process lock file coordination, and RSS cap
// enforcement.

#include "frost/sched/resource_governor.hpp"

#include "support/engine_lock.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>

#include <csignal>

#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::sched::HeavyLegKind;
using frost::sched::HostResources;
using frost::sched::ResourceGovernor;
using frost::sched::ResourceGovernorOptions;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::int64_t selfPid() noexcept {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

// Spawns a short-lived sleeping child process for real OS RSS sampling tests.
class SleepingChildProcess {
public:
    SleepingChildProcess() {
#ifdef _WIN32
        std::array<wchar_t, MAX_PATH> sysDir{};
        const UINT len = GetSystemDirectoryW(sysDir.data(), MAX_PATH);
        if (len == 0 || len >= MAX_PATH) {
            return;
        }
        std::wstring cmdExe(sysDir.data(), len);
        cmdExe += L"\\cmd.exe";
        std::wstring cmdLine = L"\"";
        cmdLine += cmdExe;
        cmdLine += L"\" /c ping -n 30 127.0.0.1 >nul";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(cmdExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &si, &pi) != 0) {
            CloseHandle(pi.hThread);
            m_process = pi.hProcess;
            m_pid = static_cast<std::int64_t>(pi.dwProcessId);
        }
#else
        const pid_t child = ::fork();
        if (child == 0) {
            char arg0[] = "/bin/sleep";
            char arg1[] = "30";
            char* const argv[] = {arg0, arg1, nullptr};
            ::execv("/bin/sleep", argv);
            ::_exit(127);
        }
        if (child > 0) {
            m_pid = static_cast<std::int64_t>(child);
        }
#endif
    }

    ~SleepingChildProcess() { terminateAndReap(); }
    SleepingChildProcess(const SleepingChildProcess&) = delete ("owns a child process");
    SleepingChildProcess& operator=(const SleepingChildProcess&) = delete ("owns a child process");

    [[nodiscard]] std::int64_t pid() const noexcept { return m_pid; }

    void terminateAndReap() noexcept {
#ifdef _WIN32
        if (m_process != nullptr) {
            TerminateProcess(m_process, 0);
            WaitForSingleObject(m_process, 5000);
            CloseHandle(m_process);
            m_process = nullptr;
        }
#else
        if (m_pid > 0) {
            ::kill(static_cast<pid_t>(m_pid), SIGKILL);
            int status = 0;
            (void)::waitpid(static_cast<pid_t>(m_pid), &status, 0);
            m_pid = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_process = nullptr;
#endif
    std::int64_t m_pid = -1;
};

TEST(ResourceGovernorTest, ProbesLiveHostResourcesAndCurrentProcessRss) {
    const HostResources host = frost::sched::probeHostResources();
    EXPECT_GE(host.logicalCores, 1u);
    EXPECT_GT(host.totalPhysicalBytes, 256ULL * kMiB);
    EXPECT_GT(host.availablePhysicalBytes, 0ULL);
    EXPECT_LE(host.availablePhysicalBytes, host.totalPhysicalBytes);
    EXPECT_GE(host.totalCommitBytes, host.totalPhysicalBytes);
    EXPECT_GT(host.availableCommitBytes, 0ULL);

    const std::uint64_t selfRss = frost::sched::currentProcessRssBytes();
    EXPECT_GT(selfRss, 64ULL * 1024ULL);

    const auto queriedSelf = frost::sched::queryProcessRssBytes(selfPid());
    ASSERT_TRUE(queriedSelf.has_value()) << queriedSelf.error().describe();
    EXPECT_GT(*queriedSelf, 64ULL * 1024ULL);

    const auto invalidZero = frost::sched::queryProcessRssBytes(0);
    ASSERT_FALSE(invalidZero.has_value());
    EXPECT_EQ(invalidZero.error().code(), ErrorCode::InvalidArgument);

    const auto invalidNegative = frost::sched::queryProcessRssBytes(-42);
    ASSERT_FALSE(invalidNegative.has_value());
    EXPECT_EQ(invalidNegative.error().code(), ErrorCode::InvalidArgument);

    const auto missing = frost::sched::queryProcessRssBytes(99999999);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(ResourceGovernorTest, SamplesRssOfRealChildProcessAndPreservesPeakAfterExit) {
    SleepingChildProcess child;
    ASSERT_GT(child.pid(), 0);

    ResourceGovernor governor;
    std::uint64_t sampledRss = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto sampled = governor.sampleProcessRss(child.pid(), HeavyLegKind::BackgroundIndex);
        if (sampled && *sampled > 0) {
            sampledRss = *sampled;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_GT(sampledRss, 0ULL);
    EXPECT_EQ(governor.currentProcessRssBytes(child.pid()), sampledRss);
    EXPECT_EQ(governor.peakProcessRssBytes(child.pid()), sampledRss);
    EXPECT_EQ(governor.peakLegRssBytes(HeavyLegKind::BackgroundIndex), sampledRss);
    EXPECT_EQ(governor.currentCombinedRssBytes(), sampledRss);
    EXPECT_EQ(governor.peakCombinedRssBytes(), sampledRss);
    EXPECT_FALSE(governor.isProcessOverRssCap(child.pid()));

    const std::int64_t exitedPid = child.pid();
    child.terminateAndReap();
    governor.unregisterProcess(exitedPid);
    EXPECT_EQ(governor.currentProcessRssBytes(exitedPid), 0ULL);
    EXPECT_EQ(governor.currentCombinedRssBytes(), 0ULL);
    EXPECT_EQ(governor.peakProcessRssBytes(exitedPid), sampledRss);
    EXPECT_EQ(governor.peakCombinedRssBytes(), sampledRss);

    const auto afterExit = governor.sampleProcessRss(exitedPid, HeavyLegKind::BackgroundIndex);
    EXPECT_FALSE(afterExit.has_value());
}

TEST(ResourceGovernorTest, ScalesClangdAndBuildJobsWithCoresAndAvailableMemory) {
    ResourceGovernorOptions options;
    options.maxClangdJobs = 10;
    options.throttledClangdJobs = 2;
    options.maxBuildJobs = 8;
    options.hostOverride = HostResources{
        .logicalCores = 24,
        .totalPhysicalBytes = 32ULL * kGiB,
        .availablePhysicalBytes = 20ULL * kGiB,
        .totalCommitBytes = 36ULL * kGiB,
        .availableCommitBytes = 20ULL * kGiB,
    };
    ResourceGovernor governor(options);

    EXPECT_EQ(governor.clangdJobs(), 10u);
    EXPECT_EQ(governor.buildJobs(), 8u);
    EXPECT_EQ(governor.backgroundIndexPriority(), "background");
    EXPECT_TRUE(governor.pchStorageDisk());

    const std::vector<std::string> flags = governor.clangdFlags();
    EXPECT_EQ(flags, (std::vector<std::string>{
                         "-j=10",
                         "--background-index-priority=background",
                         "--pch-storage=disk",
                     }));

    // 8 logical cores with ample RAM -> cores / 2 = 4.
    governor.setHostResourcesForTesting(HostResources{
        .logicalCores = 8,
        .totalPhysicalBytes = 16ULL * kGiB,
        .availablePhysicalBytes = 12ULL * kGiB,
        .totalCommitBytes = 16ULL * kGiB,
        .availableCommitBytes = 12ULL * kGiB,
    });
    EXPECT_EQ(governor.clangdJobs(), 4u);
    EXPECT_EQ(governor.buildJobs(), 4u);

    // 24 logical cores, but available physical RAM dropped to 2 GiB -> throttled to 2.
    governor.setHostResourcesForTesting(HostResources{
        .logicalCores = 24,
        .totalPhysicalBytes = 32ULL * kGiB,
        .availablePhysicalBytes = 2ULL * kGiB,
        .totalCommitBytes = 32ULL * kGiB,
        .availableCommitBytes = 16ULL * kGiB,
    });
    EXPECT_EQ(governor.clangdJobs(), 2u);
    EXPECT_EQ(governor.buildJobs(), 2u);

    // 24 logical cores, plenty of physical RAM, but available commit is only 1 GiB -> 1 job.
    governor.setHostResourcesForTesting(HostResources{
        .logicalCores = 24,
        .totalPhysicalBytes = 32ULL * kGiB,
        .availablePhysicalBytes = 16ULL * kGiB,
        .totalCommitBytes = 32ULL * kGiB,
        .availableCommitBytes = 1ULL * kGiB,
    });
    EXPECT_EQ(governor.clangdJobs(), 1u);
    EXPECT_EQ(governor.buildJobs(), 1u);
}

TEST(ResourceGovernorTest, ThrottlesConcurrencyWhenHeavyLegIsActive) {
    ResourceGovernorOptions options;
    options.maxClangdJobs = 8;
    options.throttledClangdJobs = 2;
    options.maxBuildJobs = 8;
    options.hostOverride = HostResources{
        .logicalCores = 24,
        .totalPhysicalBytes = 32ULL * kGiB,
        .availablePhysicalBytes = 16ULL * kGiB,
        .totalCommitBytes = 32ULL * kGiB,
        .availableCommitBytes = 16ULL * kGiB,
    };
    ResourceGovernor governor(options);

    EXPECT_EQ(governor.clangdJobs(), 8u);
    EXPECT_EQ(governor.buildJobs(), 8u);
    EXPECT_FALSE(governor.activeHeavyLeg().has_value());
    EXPECT_EQ(governor.activeLegCount(), 0u);

    {
        auto buildLease = governor.tryAcquireHeavyLeg(HeavyLegKind::Build);
        ASSERT_TRUE(buildLease.has_value()) << buildLease.error().describe();
        EXPECT_TRUE(buildLease->held());
        EXPECT_EQ(buildLease->kind(), HeavyLegKind::Build);
        EXPECT_TRUE(governor.isLegActive(HeavyLegKind::Build));
        EXPECT_EQ(governor.activeHeavyLeg(), HeavyLegKind::Build);
        EXPECT_EQ(governor.activeLegCount(), 1u);
        // While a build is active, clangd is throttled to throttledClangdJobs (2).
        EXPECT_EQ(governor.clangdJobs(), 2u);
    }

    EXPECT_FALSE(governor.isLegActive(HeavyLegKind::Build));
    EXPECT_EQ(governor.clangdJobs(), 8u);

    {
        auto indexScope = governor.beginLegActivity(HeavyLegKind::BackgroundIndex);
        EXPECT_TRUE(indexScope.active());
        EXPECT_TRUE(governor.isLegActive(HeavyLegKind::BackgroundIndex));
        // While background indexing is active, build jobs throttle down.
        EXPECT_EQ(governor.buildJobs(), 4u);
    }

    EXPECT_FALSE(governor.isLegActive(HeavyLegKind::BackgroundIndex));
    EXPECT_EQ(governor.buildJobs(), 8u);
}

TEST(ResourceGovernorTest, SerializesExclusiveHeavyLegsAndSharesCrossProcessBuildLock) {
    std::error_code ec;
    const std::filesystem::path lockFile =
        std::filesystem::temp_directory_path(ec) /
        ("frost-sched-governor-lock-" + std::to_string(selfPid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".lock");
    const std::string lockPath = lockFile.string();
    struct RemoveLockFile {
        std::filesystem::path path;
        ~RemoveLockFile() {
            std::error_code removeEc;
            std::filesystem::remove(path, removeEc);
        }
    } cleanup{lockFile};

    ResourceGovernorOptions options;
    options.lockFilePath = lockPath;
    ResourceGovernor governorA(options);
    ResourceGovernor governorB(options);

    auto first = governorA.tryAcquireHeavyLeg(HeavyLegKind::Build);
    ASSERT_TRUE(first.has_value()) << first.error().describe();

    // Same governor rejects a second exclusive heavy leg immediately.
    auto sameGovSecond = governorA.tryAcquireHeavyLeg(HeavyLegKind::Inspection);
    ASSERT_FALSE(sameGovSecond.has_value());
    EXPECT_EQ(sameGovSecond.error().code(), ErrorCode::Busy);

    auto timedOut = governorA.acquireHeavyLeg(HeavyLegKind::Inspection, 40ms);
    ASSERT_FALSE(timedOut.has_value());
    EXPECT_EQ(timedOut.error().code(), ErrorCode::Timeout);

    // Second governor sharing the lock file also sees Busy because byte 0 is locked.
    auto otherGov = governorB.tryAcquireHeavyLeg(HeavyLegKind::Build);
    ASSERT_FALSE(otherGov.has_value());
    EXPECT_EQ(otherGov.error().code(), ErrorCode::Busy);

    // Waiting thread acquires the leg as soon as `first` releases.
    auto waiter =
        std::async(std::launch::async, [&] { return governorA.acquireHeavyLeg(HeavyLegKind::Inspection, 5s); });
    std::this_thread::sleep_for(30ms);
    first->release();

    auto acquiredByWaiter = waiter.get();
    ASSERT_TRUE(acquiredByWaiter.has_value()) << acquiredByWaiter.error().describe();
    EXPECT_EQ(acquiredByWaiter->kind(), HeavyLegKind::Inspection);
    acquiredByWaiter->release();

    // Compatibility with frost::test::EngineBuildLock on the same lock file.
    {
        frost::test::EngineBuildLock externalLock(lockPath, 1s);
        ASSERT_TRUE(externalLock.held());
        auto blockedByExternal = governorA.tryAcquireHeavyLeg(HeavyLegKind::Build);
        ASSERT_FALSE(blockedByExternal.has_value());
        EXPECT_EQ(blockedByExternal.error().code(), ErrorCode::Busy);
    }

    auto afterExternal = governorA.tryAcquireHeavyLeg(HeavyLegKind::Build);
    ASSERT_TRUE(afterExternal.has_value()) << afterExternal.error().describe();
}

TEST(ResourceGovernorTest, EnforcesPerProcessAndCombinedRssCaps) {
    ResourceGovernorOptions options;
    options.clangdRssCapBytes = 512ULL * kMiB;
    options.combinedRssCapBytes = 1024ULL * kMiB;
    options.maxClangdJobs = 8;
    options.maxBuildJobs = 8;
    options.hostOverride = HostResources{
        .logicalCores = 24,
        .totalPhysicalBytes = 32ULL * kGiB,
        .availablePhysicalBytes = 16ULL * kGiB,
        .totalCommitBytes = 32ULL * kGiB,
        .availableCommitBytes = 16ULL * kGiB,
    };
    ResourceGovernor governor(options);

    EXPECT_EQ(governor.clangdRssCapBytes(), 512ULL * kMiB);
    EXPECT_EQ(governor.combinedRssCapBytes(), 1024ULL * kMiB);

    // Under cap.
    governor.recordProcessRss(1001, 300ULL * kMiB, HeavyLegKind::BackgroundIndex);
    EXPECT_FALSE(governor.isProcessOverRssCap(1001));
    EXPECT_FALSE(governor.isCombinedOverRssCap());
    EXPECT_EQ(governor.clangdJobs(), 8u);

    // Exceeds clangd RSS cap -> flagged and throttled to 1 job.
    governor.recordProcessRss(1001, 600ULL * kMiB, HeavyLegKind::BackgroundIndex);
    EXPECT_TRUE(governor.isProcessOverRssCap(1001));
    EXPECT_FALSE(governor.isCombinedOverRssCap());
    EXPECT_EQ(governor.peakProcessRssBytes(1001), 600ULL * kMiB);
    EXPECT_EQ(governor.clangdJobs(), 1u);

    // Reset and test combined index + build RSS cap.
    governor.resetRssTracking();
    EXPECT_EQ(governor.peakCombinedRssBytes(), 0ULL);
    EXPECT_FALSE(governor.isProcessOverRssCap(1001));

    governor.recordProcessRss(2001, 400ULL * kMiB, HeavyLegKind::BackgroundIndex);
    governor.recordProcessRss(2002, 500ULL * kMiB, HeavyLegKind::Build);
    EXPECT_EQ(governor.currentCombinedRssBytes(), 900ULL * kMiB);
    EXPECT_FALSE(governor.isProcessOverRssCap(2001));
    EXPECT_FALSE(governor.isProcessOverRssCap(2002));
    EXPECT_FALSE(governor.isCombinedOverRssCap());

    // Additional build process pushes combined RSS above 1024 MiB.
    governor.recordProcessRss(2003, 250ULL * kMiB, HeavyLegKind::Build);
    EXPECT_EQ(governor.currentCombinedRssBytes(), 1150ULL * kMiB);
    EXPECT_EQ(governor.peakCombinedRssBytes(), 1150ULL * kMiB);
    EXPECT_TRUE(governor.isCombinedOverRssCap());
    EXPECT_EQ(governor.clangdJobs(), 1u);
    EXPECT_EQ(governor.buildJobs(), 1u);
}

} // namespace
