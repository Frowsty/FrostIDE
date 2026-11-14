// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for CMakeService (cmake_service.hpp) over a MemFs folder and the scripted launcher:
// nothing runs before trust (an existing reply is still shown), configure after trust (the query,
// the command line, the reply read, CMake warnings and errors as diagnostics), Build, Clean and
// Rebuild of a target with the compiler diagnostics resolved against the build directory, the
// debounced reconfigure on a CMake file change, and profiles (settings records, persistence).
// The synthetic File API reply comes from tests/project/fixtures/cmake/reply.

#include "frost/build/build_service.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/value.hpp"

#include "../../src/build/src/cmake_service_internal.hpp"
#include "build_test_util.hpp"
#include "project/cmake_reply_fixture.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::build {
namespace {

using test::kWindows;
using test::MemTree;
using namespace std::chrono_literals;

constexpr auto kWait = 20s;

std::string exe(std::string_view name) { return kWindows ? std::string(name) + ".exe" : std::string(name); }

// What the callbacks saw, under a lock (they run on the service thread).
struct Events {
    mutable Mutex mutex;
    std::vector<BuildState> states FROST_GUARDED_BY(mutex);
    std::vector<std::vector<std::string>> trustRequests FROST_GUARDED_BY(mutex);
    std::vector<std::string> configureLines FROST_GUARDED_BY(mutex);
    std::vector<std::string> buildLines FROST_GUARDED_BY(mutex);
    std::vector<CMakeConfigureOutcome> configures FROST_GUARDED_BY(mutex);
    std::vector<CMakeBuildOutcome> builds FROST_GUARDED_BY(mutex);
    int models FROST_GUARDED_BY(mutex) = 0;
    int profileChanges FROST_GUARDED_BY(mutex) = 0;

    CMakeServiceCallbacks callbacks() {
        CMakeServiceCallbacks out;
        out.onStateChanged = [this](BuildState state) {
            LockGuard lock(mutex);
            states.push_back(state);
        };
        out.onNeedsTrust = [this](const std::vector<std::string>& roots) {
            LockGuard lock(mutex);
            trustRequests.push_back(roots);
        };
        out.onConfigureOutput = [this](BuildOutputKind, std::string_view line) {
            LockGuard lock(mutex);
            configureLines.emplace_back(line);
        };
        out.onOutput = [this](BuildOutputKind, std::string_view line) {
            LockGuard lock(mutex);
            buildLines.emplace_back(line);
        };
        out.onConfigureFinished = [this](const CMakeConfigureOutcome& outcome) {
            LockGuard lock(mutex);
            configures.push_back(outcome);
        };
        out.onFinished = [this](const CMakeBuildOutcome& outcome) {
            LockGuard lock(mutex);
            builds.push_back(outcome);
        };
        out.onModelChanged = [this](const std::shared_ptr<const project::CMakeProject>&) {
            LockGuard lock(mutex);
            ++models;
        };
        out.onProfilesChanged = [this](const std::vector<project::CMakeProfile>&, std::optional<std::size_t>) {
            LockGuard lock(mutex);
            ++profileChanges;
        };
        return out;
    }
};

/*
 * The scripted launcher, seen through a cmake that rewrites its File API reply: each configure
 * launch (cmake -S ...) whose build directory already holds a reply adds a newer index file (a
 * copy of the newest), as real cmake writes a fresh index-*.json at the end of every configure
 * (CMK-1: the service refuses a reply that is not newer than the configure). Launch records and
 * scripts stay with the inner launcher.
 */
class ReplyRewritingLauncher final : public io::IProcessLauncher {
public:
    ReplyRewritingLauncher(io::IProcessLauncher& inner, io::IFileSystem& fs) : m_inner(inner), m_fs(fs) {}

    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        rewrite(spec);
        return m_inner.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        rewrite(spec);
        return m_inner.launchAsync(spec, context);
    }

private:
    void rewrite(const io::ProcessSpec& spec) {
        const auto build = std::ranges::find(spec.arguments, "-B");
        if (spec.arguments.empty() || spec.arguments.front() != "-S" || build == spec.arguments.end() ||
            build + 1 == spec.arguments.end()) {
            return;
        }
        auto replyDir = project::cmakeFileApiReplyDirectory(*(build + 1));
        auto entries =
            replyDir ? m_fs.listDirectory(*replyDir) : Result<std::vector<io::DirEntry>>(std::vector<io::DirEntry>{});
        if (!entries) {
            return;
        }
        std::string newest;
        for (const io::DirEntry& entry : *entries) {
            if (entry.name.starts_with("index-") && entry.name > newest) {
                newest = entry.name;
            }
        }
        if (newest.empty()) {
            return;
        }
        auto text = m_fs.readFile(io::joinPath(*replyDir, newest).value());
        const int serial = ++m_serial;
        const std::string fresh = std::format("index-2099-01-01T00-00-00-{:04}.json", serial);
        if (text) {
            static_cast<void>(m_fs.writeFileAtomic(io::joinPath(*replyDir, fresh).value(), *text));
        }
    }

    io::IProcessLauncher& m_inner;
    io::IFileSystem& m_fs;
    std::atomic<int> m_serial{0};
};

class CMakeServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tree.write("tiny/CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Tiny C)\n");
        tree.write("tiny/src/core.c", "int core(void) { return 0; }\n");
        tree.write("sdk/" + exe("cmake"), "x");
        tree.write("sdk/" + exe("ninja"), "x");
        auto opened = project::TrustStore::open(tree.fs, MemTree::path("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        trustStore = std::move(*opened);
    }

    void TearDown() override { service.reset(); }

    CMakeServiceOptions options() const {
        CMakeServiceOptions out;
        out.cmakePath = cmake();
        out.ninjaPath = ninja();
        out.pathVariable = "unused";
        out.useVisualStudio = false;
        out.captureMsvcEnvironment = false;
        out.stateFilePath = MemTree::path("data/cmake-profiles.txt");
        out.reloadDebounce = 30ms;
        return out;
    }

    void start(CMakeServiceOptions serviceOptions) {
        service =
            CMakeService::create(tree.fs, cmakeLauncher, *trustStore, std::move(serviceOptions), events.callbacks());
    }
    void start() { start(options()); }

    static std::string folder() { return MemTree::path("tiny"); }
    static std::string buildDir(std::string_view relative = {}) {
        return relative.empty() ? MemTree::path("tiny/cmake-build-debug")
                                : MemTree::path(std::string("tiny/cmake-build-debug/") + std::string(relative));
    }
    static std::string cmake() { return MemTree::path("sdk/" + exe("cmake")); }
    static std::string ninja() { return MemTree::path("sdk/" + exe("ninja")); }

    void installReply() {
        frost::test::installCMakeReply(tree.fs, FROST_CMAKE_REPLY_FIXTURE_DIR, folder(), buildDir(),
                                       MemTree::path("other"));
    }

    // The scripts of a first configure: the version probe, then cmake itself.
    void expectFirstConfigure(io::ScriptedProcess configure = {}) {
        launcher.expect({.expectExecutable = cmake(), .stdoutBytes = "cmake version 4.4.5\n"});
        configure.expectExecutable = cmake();
        launcher.expect(std::move(configure));
    }

    void waitIdle() { ASSERT_TRUE(service->waitForIdle(kWait)); }

    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    ReplyRewritingLauncher cmakeLauncher{launcher, tree.fs}; // What the services launch through.
    std::unique_ptr<project::TrustStore> trustStore;
    Events events;
    std::unique_ptr<CMakeService> service;
};

TEST_F(CMakeServiceTest, DebugReloadUsesConfigurePipelineAndSavesBeforeLaunch) {
    auto opts = options();
    opts.configureOnOpen = false;
    ASSERT_TRUE(trustStore->trust(folder()));
    expectFirstConfigure({.neverExits = true});
    start(std::move(opts));
    ASSERT_TRUE(service->openFolder(folder()));
    std::atomic<bool> saved{false};
    std::promise<void> connected;
    service->setSaveAllHook([&](auto done) { saved = true; done(Result<void>{}); });
    ASSERT_TRUE(service->configureDebug("private-debug-endpoint", [&] { connected.set_value(); }));
    ASSERT_EQ(connected.get_future().wait_for(kWait), std::future_status::ready);
    ASSERT_TRUE(saved);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    const auto& args = launches.back().spec.arguments;
    EXPECT_NE(std::ranges::find(args, "--debugger"), args.end());
    EXPECT_NE(std::ranges::find(args, "--debugger-pipe"), args.end());
    EXPECT_NE(std::ranges::find(args, "private-debug-endpoint"), args.end());
    service->cancel();
    waitIdle();
}

TEST_F(CMakeServiceTest, DebugReloadWaitsForTrustAndCannotRunCallbackEarly) {
    auto opts = options();
    opts.configureOnOpen = false;
    start(std::move(opts));
    ASSERT_TRUE(service->openFolder(folder()));
    std::atomic<bool> ready{false};
    ASSERT_TRUE(service->configureDebug("private-debug-endpoint", [&] { ready = true; }));
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_FALSE(ready);
    service->cancel();
    waitIdle();
}

TEST_F(CMakeServiceTest, DeferredWatcherReloadWaitsForExternalAdmission) {
    std::atomic<bool> blocked{false};
    std::atomic<int> held{0};
    auto opts = options();
    opts.reserveProjectCode = [&]() -> Result<void> {
        if (blocked.load())
            return fail(ErrorCode::Busy, "external launch pending");
        ++held;
        return {};
    };
    opts.releaseProjectCode = [&] { --held; };
    ASSERT_TRUE(trustStore->trust(folder()));
    installReply();
    expectFirstConfigure();
    start(std::move(opts));
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    ASSERT_EQ(launcher.launches().size(), 2U);
    EXPECT_EQ(held.load(), 0);
    blocked = true;
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/CMakeLists.txt")}},
                           .sequence = 1});
    waitIdle();
    EXPECT_EQ(launcher.launches().size(), 2U);
    EXPECT_FALSE(service->configure());
    EXPECT_FALSE(service->setActiveProfile(std::size_t{0}));
    launcher.expect({.expectExecutable = cmake()});
    blocked = false;
    service->admissionAvailable();
    waitIdle();
    EXPECT_EQ(launcher.launches().size(), 3U);
    EXPECT_EQ(held.load(), 0);
}

TEST_F(CMakeServiceTest, OwnedConfigureRetainsExactChildUntilCancellation) {
    std::atomic<int> held{0};
    auto opts = options();
    opts.reserveOwnedProjectCode = [&](std::uint64_t owner) -> Result<std::uint64_t> {
        if (owner != 42)
            return fail(ErrorCode::Busy, "wrong owner");
        ++held;
        return 17;
    };
    opts.releaseOwnedProjectCode = [&](std::uint64_t owner, std::uint64_t child) {
        EXPECT_EQ(owner, 42U);
        EXPECT_EQ(child, 17U);
        --held;
    };
    start(std::move(opts));
    ASSERT_TRUE(service->openFolder(folder()));
    EXPECT_FALSE(service->configureOwned(41));
    ASSERT_TRUE(service->configureOwned(42));
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_EQ(held.load(), 1);
    service->cancel();
    waitIdle();
    EXPECT_EQ(held.load(), 0);
}

TEST_F(CMakeServiceTest, OwnedConfigureKeepsAdmissionUntilRunningProcessIsCancelled) {
    auto shared = BuildService::create(tree.fs, launcher, *trustStore);
    auto owner = shared->reserveExternalLaunch();
    ASSERT_TRUE(owner);
    auto opts = options();
    opts.configureOnOpen = false;
    opts.reserveOwnedProjectCode = [&](std::uint64_t token) { return shared->reserveExternalChild(token); };
    opts.releaseOwnedProjectCode = [&](std::uint64_t token, std::uint64_t child) {
        shared->releaseExternalChild(token, child);
    };
    ASSERT_TRUE(trustStore->trust(folder()));
    expectFirstConfigure({.neverExits = true});
    auto local = CMakeService::create(tree.fs, cmakeLauncher, *trustStore, std::move(opts), events.callbacks());
    ASSERT_TRUE(local->openFolder(folder()));
    ASSERT_TRUE(local->configureOwned(*owner));
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (launcher.launches().size() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(launcher.launches().size(), 2U);
    shared->releaseExternalLaunch(*owner);
    EXPECT_FALSE(shared->reserveExternalLaunch());
    local->cancel();
    ASSERT_TRUE(local->waitForIdle(kWait));
    EXPECT_TRUE(launcher.launches().back().terminated);
    auto successor = shared->reserveExternalLaunch();
    ASSERT_TRUE(successor);
    shared->releaseExternalLaunch(*successor);
    local.reset();
}

TEST_F(CMakeServiceTest, CancelDuringProbeRetainsAdmissionUntilProbeReturns) {
    class HeldLauncher final : public io::IProcessLauncher {
    public:
        io::ScriptedProcessLauncher delegate;
        std::atomic<bool> entered{false}, release{false};
        Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
            entered = true;
            while (!release.load())
                std::this_thread::sleep_for(5ms);
            return delegate.launch(spec);
        }
        Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                               const io::AsyncContext& context) override {
            return delegate.launchAsync(spec, context);
        }
    } held;
    auto shared = BuildService::create(tree.fs, held, *trustStore);
    auto owner = shared->reserveExternalLaunch();
    ASSERT_TRUE(owner);
    auto opts = options();
    std::atomic<int> refused{0};
    opts.reserveProjectCode = [&]() -> Result<void> {
        auto result = shared->reserveRun();
        if (!result)
            ++refused;
        return result;
    };
    opts.releaseProjectCode = [&] { shared->releaseRun(); };
    opts.reserveOwnedProjectCode = [&](std::uint64_t token) { return shared->reserveExternalChild(token); };
    opts.releaseOwnedProjectCode = [&](std::uint64_t token, std::uint64_t child) {
        shared->releaseExternalChild(token, child);
    };
    held.delegate.expect({.expectExecutable = cmake(), .stdoutBytes = "cmake version 4.4.5\n"});
    held.delegate.expect({.expectExecutable = cmake()});
    auto local = CMakeService::create(tree.fs, held, *trustStore, std::move(opts), events.callbacks());
    ASSERT_TRUE(local->openFolder(folder()));
    ASSERT_TRUE(trustStore->trust(folder()));
    ASSERT_TRUE(local->configureOwned(*owner));
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!held.entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    if (!held.entered.load()) {
        held.release = true;
        local.reset();
        FAIL() << "probe did not start";
    }
    local->cancel();
    shared->releaseExternalLaunch(*owner);
    EXPECT_FALSE(shared->reserveExternalLaunch());
    EXPECT_TRUE(local->openFolder(folder())); // Automatic configure is deferred under the closing owner.
    EXPECT_GT(refused.load(), 0);
    held.release = true;
    ASSERT_TRUE(local->waitForIdle(kWait));
    EXPECT_EQ(held.delegate.launches().size(), 2U);
    auto successor = shared->reserveExternalLaunch();
    ASSERT_TRUE(successor);
    shared->releaseExternalLaunch(*successor);
    local.reset();
}

/*
 * A file system over another that holds every read of a File API reply (a path below
 * .cmake/api/v1/reply) until release(), and records the threads those reads ran on. The R0 exit
 * criterion's stall came from openFolder reading the reply on its caller's (the GUI) thread.
 */
class HeldReplyFs final : public io::IFileSystem {
public:
    explicit HeldReplyFs(io::IFileSystem& inner) : m_inner(inner) {}

    Result<std::string> readFile(std::string_view path) override {
        hold(path);
        return m_inner.readFile(path);
    }
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_inner.writeFileAtomic(path, data);
    }
    Result<void> writeFileAtomicWithMtime(std::string_view path, std::string_view data,
                                          std::filesystem::file_time_type mtime) override {
        return m_inner.writeFileAtomicWithMtime(path, data, mtime);
    }
    Result<io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                          const io::SafeWriteOptions& options) override {
        return m_inner.safeWrite(path, data, options);
    }
    Result<io::FileStatus> status(std::string_view path) override { return m_inner.status(path); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view path) override {
        hold(path);
        return m_inner.listDirectory(path);
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        hold(path);
        return m_inner.listDirectoryWithDiagnostics(path);
    }
    Result<void> createDirectories(std::string_view path) override { return m_inner.createDirectories(path); }
    Result<void> removeFile(std::string_view path) override { return m_inner.removeFile(path); }
    Result<void> removeDirectory(std::string_view path) override { return m_inner.removeDirectory(path); }
    Result<bool> exists(std::string_view path) override { return m_inner.exists(path); }
    Result<std::string> canonicalPath(std::string_view path) override { return m_inner.canonicalPath(path); }
    Result<std::string> physicalPath(std::string_view path) override { return m_inner.physicalPath(path); }

    // Lets the held reads, and every later one, go on.
    void release() {
        {
            LockGuard lock(m_mutex);
            m_released = true;
        }
        m_changed.notify_all();
    }
    // Holds the reply reads that start from now on again, until the next release().
    void holdAgain() {
        LockGuard lock(m_mutex);
        m_released = false;
    }
    // Waits until a reply read is held, or `timeout` passes.
    bool waitForHeldRead(std::chrono::milliseconds timeout) {
        UniqueLock lock(m_mutex);
        return m_changed.wait_for(lock.native(), timeout,
                                  [this]() FROST_NO_THREAD_SAFETY_ANALYSIS { return m_held > 0; });
    }
    // Whether any reply read ran on `thread`, and how many ran.
    bool readOn(std::thread::id thread) const {
        LockGuard lock(m_mutex);
        return std::ranges::find(m_threads, thread) != m_threads.end();
    }
    std::size_t reads() const {
        LockGuard lock(m_mutex);
        return m_threads.size();
    }

private:
    static bool isReply(std::string_view path) {
        return path.find(kWindows ? "\\v1\\reply" : "/v1/reply") != std::string_view::npos;
    }
    void hold(std::string_view path) {
        if (!isReply(path))
            return;
        UniqueLock lock(m_mutex);
        m_threads.push_back(std::this_thread::get_id());
        ++m_held;
        m_changed.notify_all();
        m_changed.wait(lock.native(), [this]() FROST_NO_THREAD_SAFETY_ANALYSIS { return m_released; });
        --m_held;
    }

    io::IFileSystem& m_inner;
    mutable Mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<std::thread::id> m_threads FROST_GUARDED_BY(m_mutex);
    int m_held FROST_GUARDED_BY(m_mutex) = 0;
    bool m_released FROST_GUARDED_BY(m_mutex) = false;
};

/*
 * R0 exit criterion (no GUI stall above 250 ms opening FrostEngine; FND-9a): the File API reply of
 * a large project is several MB of JSON, so openFolder and setActiveProfile return without reading
 * it, and the service's thread reads it and publishes the model (onModelChanged), in queue order.
 * The reads are held here: both calls return while a read is held, the model is published only
 * once the read goes on, and no reply read ever runs on the calling thread.
 */
TEST_F(CMakeServiceTest, OpenAndProfileSwitchReadTheReplyOnTheServiceThread) {
    installReply();
    HeldReplyFs fs(tree.fs);
    CMakeServiceOptions opts = options();
    opts.profileRecords = {project::CMakeProfileRecord{.name = "Debug"},
                           project::CMakeProfileRecord{.name = "Release", .buildType = "Release"}};
    service = CMakeService::create(fs, cmakeLauncher, *trustStore, std::move(opts), events.callbacks());
    ASSERT_TRUE(service->openFolder(folder()).has_value()); // Untrusted: no configure.
    ASSERT_TRUE(fs.waitForHeldRead(kWait)) << "the existing reply was never read";
    EXPECT_EQ(service->model(), nullptr) << "a model was published before its reply was read";
    {
        LockGuard lock(events.mutex);
        EXPECT_EQ(events.models, 0);
    }
    // A profile switch also returns while the reads are held: Release (no reply), then Debug again.
    ASSERT_TRUE(service->setActiveProfile("Release").has_value());
    ASSERT_TRUE(service->setActiveProfile("Debug").has_value());
    EXPECT_EQ(service->model(), nullptr);
    fs.release();
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->model()->targets.size(), 4U);
    EXPECT_EQ(service->activeProfile()->name, "Debug");
    EXPECT_GE(fs.reads(), 1U);
    EXPECT_FALSE(fs.readOn(std::this_thread::get_id())) << "a reply was read on the calling thread";
    {
        // The open's read and the Release read find the profile switched since they were queued and
        // publish nothing; the last switch's read publishes Debug's model, once.
        LockGuard lock(events.mutex);
        EXPECT_EQ(events.models, 1);
    }
    EXPECT_TRUE(launcher.launches().empty()); // Untrusted: neither the open nor the switches ran anything.
}

/*
 * R0 follow-up 10 (clangd started about three times per profile switch): a switch to a profile
 * whose build directory has no reply starts its configure before setActiveProfile returns, so the
 * service is never idle on the new build directory while its reply read waits on the service
 * thread (the trust watch restarted clangd without a database in that window). A Build asked for
 * then is refused (Busy), as it was before the read moved to the service thread. A profile whose
 * reply exists but does not parse is configured by the read itself, and modelLoading() reports the
 * read until it has run. The reads are held to make that window as wide as a slow read is.
 */
TEST_F(CMakeServiceTest, AProfileSwitchDecidesItsConfigureBeforeTheReplyIsRead) {
    installReply();
    tree.write("tiny/cmake-build-release/.cmake/api/v1/reply/index-2026-01-01T00-00-00-0000.json", "not json");
    HeldReplyFs fs(tree.fs);
    // A failed assertion returns with a read held: release it and stop the service before `fs` goes.
    struct StopFirst {
        HeldReplyFs& fs;
        std::unique_ptr<CMakeService>& service;
        ~StopFirst() {
            fs.release();
            service.reset();
        }
    } stopFirst{fs, service};
    CMakeServiceOptions opts = options();
    opts.profileRecords = {project::CMakeProfileRecord{.name = "Debug"},
                           project::CMakeProfileRecord{.name = "Release", .buildType = "Release"},
                           project::CMakeProfileRecord{.name = "Fresh", .buildType = "RelWithDebInfo"}};
    service = CMakeService::create(fs, cmakeLauncher, *trustStore, std::move(opts), events.callbacks());
    ASSERT_TRUE(service->openFolder(folder()).has_value()); // Untrusted: the open configures nothing.
    EXPECT_TRUE(service->modelLoading());
    fs.release();
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_FALSE(service->modelLoading());
    ASSERT_TRUE(trustStore->trust(folder()).has_value()); // The store only: nothing configures yet.
    ASSERT_TRUE(launcher.launches().empty());

    // No reply directory: Configuring when the call returns, with the read still held.
    fs.holdAgain();
    expectFirstConfigure();
    ASSERT_TRUE(service->setActiveProfile("Fresh").has_value());
    EXPECT_NE(service->state(), BuildState::Idle) << "the switch left its configure to the held reply read";
    const auto built = service->build(BuildAction::Build);
    ASSERT_FALSE(built.has_value()) << "a Build was accepted while the switch's configure should run";
    EXPECT_EQ(built.error().code(), ErrorCode::Busy);
    ASSERT_TRUE(fs.waitForHeldRead(kWait));
    EXPECT_TRUE(service->modelLoading());
    fs.release();
    waitIdle();
    EXPECT_FALSE(service->modelLoading());
    EXPECT_EQ(launcher.launches().size(), 2U) << "the switch did not configure the new profile exactly once";
    ASSERT_TRUE(service->lastConfigure().has_value());

    // A reply that does not parse: idle on return while the read is pending; the read configures.
    fs.holdAgain();
    launcher.expect({.expectExecutable = cmake()});
    const std::size_t before = launcher.launches().size();
    ASSERT_TRUE(service->setActiveProfile("Release").has_value());
    ASSERT_TRUE(fs.waitForHeldRead(kWait));
    EXPECT_EQ(service->state(), BuildState::Idle);
    EXPECT_TRUE(service->modelLoading()) << "a pending reply read must count as busy for clangd's restart";
    fs.release();
    waitIdle();
    EXPECT_FALSE(service->modelLoading());
    EXPECT_EQ(launcher.launches().size(), before + 1) << "the unreadable reply was not configured";
}

TEST_F(CMakeServiceTest, NothingRunsBeforeTrust) {
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    EXPECT_EQ(service->state(), BuildState::Idle); // The automatic configure waits for trust silently.
    ASSERT_EQ(service->profiles().size(), 1U);
    EXPECT_EQ(service->activeProfile()->name, "Debug");
    EXPECT_EQ(service->activeBuildDirectory(), buildDir());
    EXPECT_EQ(service->model(), nullptr);

    ASSERT_TRUE(service->configure().has_value()); // Reload asks for trust instead.
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_EQ(service->untrustedRoots(), std::vector<std::string>{folder()});
    EXPECT_EQ(service->configure().error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->build(BuildAction::Build).error().code(), ErrorCode::Busy);
    service->cancel();
    waitIdle();
    EXPECT_EQ(service->state(), BuildState::Idle);
    ASSERT_TRUE(service->lastConfigure().has_value());
    EXPECT_TRUE(service->lastConfigure()->result.cancelled);
    {
        LockGuard lock(events.mutex);
        ASSERT_EQ(events.trustRequests.size(), 1U);
        EXPECT_EQ(events.trustRequests[0], std::vector<std::string>{folder()});
    }

    // An existing reply is shown before trust, still without running anything.
    installReply();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->model()->targets.size(), 4U);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_FALSE(tree.fs.exists(buildDir(".cmake/api/v1/query/client-frostide/query.json")).value_or(false));
}

TEST_F(CMakeServiceTest, PersonalWrapperRunsOnlyAfterTrustAndSaveWithSeparateArguments) {
    installReply();
    auto opts = options();
    const std::string python = MemTree::path("sdk/" + exe("python"));
    opts.buildWrappers.push_back({.root = folder(),
                                  .profile = "Debug",
                                  .executable = python,
                                  .defaultTarget = "FrostTests",
                                  .buildArguments = {"Scripts/Build.py", "--target", "{target}", "--preset",
                                                     "{profile}", "a b;$(not-a-shell)"}});
    start(opts);
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    EXPECT_TRUE(launcher.launches().empty());
    int saves = 0;
    service->setSaveAllHook([&](const SaveAllCompletion& done) {
        ++saves;
        done({});
    });
    ASSERT_TRUE(service->build(BuildAction::Build));
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_TRUE(launcher.launches().empty());
    launcher.expect({.expectExecutable = python, .stdoutBytes = "wrapper output\n"});
    ASSERT_TRUE(service->trust(folder()));
    waitIdle();
    ASSERT_EQ(saves, 1);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1u);
    EXPECT_EQ(launches[0].spec.arguments, (std::vector<std::string>{"Scripts/Build.py", "--target", "FrostTests",
                                                                    "--preset", "Debug", "a b;$(not-a-shell)"}));
    EXPECT_EQ(launches[0].spec.workingDirectory, folder());
    ASSERT_TRUE(service->lastOutcome());
    EXPECT_TRUE(service->lastOutcome()->succeeded());
    {
        LockGuard lock(events.mutex);
        EXPECT_NE(std::ranges::find(events.buildLines, "wrapper output"), events.buildLines.end());
    }
    const auto model = service->model();
    ASSERT_TRUE(model);
    const auto found = std::ranges::find(model->targets, "core", &project::CMakeTarget::name);
    ASSERT_NE(found, model->targets.end());
    const auto selected = static_cast<std::size_t>(found - model->targets.begin());
    launcher.expect({.expectExecutable = python});
    ASSERT_TRUE(service->build(BuildAction::Build, selected, {}, model));
    waitIdle();
    const auto targetLaunches = launcher.launches();
    ASSERT_EQ(targetLaunches.size(), 2u);
    EXPECT_EQ(targetLaunches.back().spec.arguments[2], "core");
    EXPECT_EQ(service->build(BuildAction::Clean).error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(launcher.launches().size(), 2u);
}

TEST_F(CMakeServiceTest, WrapperRequiresAnExistingConfiguredModel) {
    auto opts = options();
    opts.buildWrappers.push_back({.root = folder(),
                                  .profile = "Debug",
                                  .executable = MemTree::path("sdk/python"),
                                  .buildArguments = {"Scripts/Build.py"}});
    start(opts);
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    auto result = service->build(BuildAction::Build);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(launcher.launches().empty());
}

class GatedSuccessProcess final : public io::IProcess {
public:
    GatedSuccessProcess(std::unique_ptr<io::IProcess> inner, std::atomic<bool>& release)
        : m_inner(std::move(inner)), m_release(release) {}
    io::ProcessId pid() const noexcept override { return m_inner->pid(); }
    Result<void> writeStdin(std::string_view bytes) override { return m_inner->writeStdin(bytes); }
    void closeStdin() noexcept override { m_inner->closeStdin(); }
    Result<std::size_t> readStdout(std::span<char> bytes) override { return m_inner->readStdout(bytes); }
    Result<std::size_t> readStderr(std::span<char> bytes) override { return m_inner->readStderr(bytes); }
    Result<io::ExitStatus> wait(std::optional<IClock::Duration> timeout) override {
        if (!m_release.load() && !m_terminated.load()) {
            std::this_thread::sleep_for(5ms);
            return fail(ErrorCode::Timeout, "held by test");
        }
        return m_inner->wait(timeout);
    }
    Result<void> terminate() override {
        m_terminated.store(true);
        return m_inner->terminate();
    }
    Result<io::ProcessResult> communicate(std::string_view bytes, IClock::Duration timeout) override {
        return m_inner->communicate(bytes, timeout);
    }
    void abandonOutput() noexcept override { m_inner->abandonOutput(); }

private:
    std::unique_ptr<io::IProcess> m_inner;
    std::atomic<bool>& m_release;
    std::atomic<bool> m_terminated{false};
};

class GatedSuccessLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher scripted;
    std::atomic<bool> release{false};
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto result = scripted.launch(spec);
        if (!result)
            return result;
        return std::unique_ptr<io::IProcess>(new GatedSuccessProcess(std::move(*result), release));
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return scripted.launchAsync(spec, context);
    }
};

TEST_F(CMakeServiceTest, WrapperTrustRevisionInvalidatesLateSuccess) {
    ASSERT_TRUE(trustStore->trust(folder()));
    installReply();
    auto opts = options();
    const std::string python = MemTree::path("sdk/" + exe("python"));
    opts.buildWrappers.push_back(
        {.root = folder(), .profile = "Debug", .executable = python, .buildArguments = {"Scripts/Build.py"}});
    GatedSuccessLauncher gated;
    service = CMakeService::create(tree.fs, gated, *trustStore, std::move(opts), events.callbacks());
    struct Cleanup {
        std::unique_ptr<CMakeService>& service;
        ~Cleanup() { service.reset(); }
    } cleanup{service};
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    gated.scripted.expect({.expectExecutable = python, .exitCode = 0});
    auto completions = std::make_shared<std::atomic<int>>(0);
    ASSERT_TRUE(
        service->build(BuildAction::Build, std::nullopt, [completions](const CMakeBuildOutcome&) { ++*completions; }));
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline && gated.scripted.launches().empty())
        std::this_thread::sleep_for(10ms);
    ASSERT_FALSE(gated.scripted.launches().empty());
    ASSERT_TRUE(trustStore->untrust(folder()));
    ASSERT_TRUE(trustStore->trust(folder()));
    gated.release.store(true);
    waitIdle();
    ASSERT_TRUE(service->lastOutcome());
    EXPECT_FALSE(service->lastOutcome()->succeeded());
    EXPECT_EQ(service->lastOutcome()->error->code(), ErrorCode::Cancelled);
    EXPECT_EQ(service->lastOutcome()->result.exitCode, 0);
    EXPECT_FALSE(service->lastOutcome()->result.cancelled);
    EXPECT_EQ(completions->load(), 1);
    {
        LockGuard lock(events.mutex);
        EXPECT_TRUE(events.builds.empty()); // Legacy public completion is fenced as well.
    }
}

TEST_F(CMakeServiceTest, WrapperCancellationTerminatesChild) {
    ASSERT_TRUE(trustStore->trust(folder()));
    installReply();
    auto opts = options();
    const std::string python = MemTree::path("sdk/" + exe("python"));
    opts.buildWrappers.push_back(
        {.root = folder(), .profile = "Debug", .executable = python, .buildArguments = {"Scripts/Build.py"}});
    start(opts);
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    launcher.expect({.expectExecutable = python, .neverExits = true});
    ASSERT_TRUE(service->build(BuildAction::Build));
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline && launcher.launches().empty())
        std::this_thread::sleep_for(10ms);
    ASSERT_FALSE(launcher.launches().empty());
    service->cancel();
    waitIdle();
    ASSERT_TRUE(service->lastOutcome());
    EXPECT_FALSE(service->lastOutcome()->succeeded());
    EXPECT_TRUE(launcher.launches().front().terminated);
}

TEST_F(CMakeServiceTest, WrapperOutputFenceSurvivesCompletionButNotSupersession) {
    ASSERT_TRUE(trustStore->trust(folder()));
    installReply();
    auto opts = options();
    const std::string python = MemTree::path("sdk/" + exe("python"));
    opts.buildWrappers.push_back(
        {.root = folder(), .profile = "Debug", .executable = python, .buildArguments = {"Scripts/Build.py"}});
    Mutex fencesMutex;
    std::vector<std::function<bool()>> fences;
    auto callbacks = events.callbacks();
    callbacks.onOutputFenced = [&](BuildOutputKind kind, std::string_view, std::function<bool()> fence) {
        if (kind != BuildOutputKind::Stdout)
            return;
        LockGuard lock(fencesMutex);
        fences.push_back(std::move(fence));
    };
    service = CMakeService::create(tree.fs, cmakeLauncher, *trustStore, std::move(opts), std::move(callbacks));
    ASSERT_TRUE(service->openFolder(folder()));
    waitIdle();
    launcher.expect({.expectExecutable = python, .stdoutBytes = "first output\n"});
    ASSERT_TRUE(service->build(BuildAction::Build));
    waitIdle();
    std::function<bool()> first;
    {
        LockGuard lock(fencesMutex);
        ASSERT_EQ(fences.size(), 1u);
        first = fences.front();
    }
    ASSERT_TRUE(first()); // A finished job's queued GUI output is still current.
    launcher.expect({.expectExecutable = python, .stdoutBytes = "second output\n"});
    ASSERT_TRUE(service->build(BuildAction::Build));
    waitIdle();
    EXPECT_FALSE(first());
    std::function<bool()> second;
    {
        LockGuard lock(fencesMutex);
        ASSERT_EQ(fences.size(), 2u);
        second = fences.back();
    }
    EXPECT_TRUE(second());
    ASSERT_TRUE(service->closeFolder());
    ASSERT_TRUE(service->openFolder(folder()));
    EXPECT_FALSE(second()); // Reopening the same root is a new profile generation.
}

/*
 * state() answers while the service thread is starting a step under its lock: a configure whose
 * process creation is held (as a slow CreateProcess and the spawn log's first digest of cmake.exe
 * held it for 0.32 s in j2_run, with the GUI thread waiting in state()) still reports its state at
 * once from another thread.
 */
TEST_F(CMakeServiceTest, StateAnswersWhileAStepIsBeingStarted) {
    class HeldStartLauncher final : public io::IProcessLauncher {
    public:
        explicit HeldStartLauncher(io::IProcessLauncher& inner) : m_inner(inner) {}
        std::atomic<bool> entered{false}, release{false};
        // BuildSession starts a step with launch(); only the configure step ("-S ...") is held.
        Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
            if (!spec.arguments.empty() && spec.arguments.front() == "-S") {
                entered = true;
                while (!release.load())
                    std::this_thread::sleep_for(5ms);
            }
            return m_inner.launch(spec);
        }
        Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                               const io::AsyncContext& context) override {
            return m_inner.launchAsync(spec, context);
        }

    private:
        io::IProcessLauncher& m_inner;
    } held(cmakeLauncher);
    // A failed assertion returns with the start held: release it and stop the service before
    // `held`, which it launches through, goes.
    struct StopFirst {
        HeldStartLauncher& held;
        std::unique_ptr<CMakeService>& service;
        ~StopFirst() {
            held.release = true;
            service.reset();
        }
    } stopFirst{held, service};
    service = CMakeService::create(tree.fs, held, *trustStore, options(), events.callbacks());
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    installReply();
    ASSERT_TRUE(service->configure().has_value()); // Waits for trust, as in ConfiguresOnceTrusted.
    expectFirstConfigure({.stdoutBytes = "-- Configuring done (0.1s)\n"});
    ASSERT_TRUE(service->trust(folder()).has_value()); // Starts the configure.
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!held.entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(held.entered.load()) << "the configure step never started its process";
    // The service thread is inside the step's start now; state() must not wait for it.
    auto answer = std::async(std::launch::async, [this] { return service->state(); });
    const bool answered = answer.wait_for(2s) == std::future_status::ready;
    held.release = true;
    EXPECT_TRUE(answered) << "state() waited for the step's process start";
    EXPECT_NE(answer.get(), BuildState::Idle);
    EXPECT_TRUE(service->waitForIdle(kWait));
    EXPECT_EQ(service->state(), BuildState::Idle);
    const auto outcome = service->lastConfigure();
    EXPECT_TRUE(outcome.has_value() && outcome->succeeded());
}

TEST_F(CMakeServiceTest, ConfiguresOnceTrusted) {
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    installReply(); // As if cmake wrote it.
    ASSERT_TRUE(service->configure().has_value());
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    expectFirstConfigure({.stdoutBytes = "-- Configuring done (0.1s)\n",
                          .stderrBytes = "CMake Warning at CMakeLists.txt:2 (message):\n  Be careful.\n\n"});
    ASSERT_TRUE(service->trust(folder()).has_value());
    waitIdle();

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_EQ(launches[0].spec.arguments, std::vector<std::string>{"--version"});
    const io::ProcessSpec& spec = launches[1].spec;
    EXPECT_EQ(spec.workingDirectory, folder());
    const std::vector<std::string> expected = {"-S",
                                               folder(),
                                               "-B",
                                               buildDir(),
                                               "-G",
                                               "Ninja",
                                               "-DCMAKE_BUILD_TYPE=Debug",
                                               "-DCMAKE_MAKE_PROGRAM:FILEPATH=" + ninja(),
                                               "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"};
    EXPECT_EQ(spec.arguments, expected);
    if constexpr (kWindows) {
        EXPECT_NE(
            std::ranges::find(spec.environment, std::pair<std::string, std::optional<std::string>>{"VSLANG", "1033"}),
            spec.environment.end());
    }
    EXPECT_TRUE(tree.fs.exists(buildDir(".cmake/api/v1/query/client-frostide/query.json")).value_or(false));

    auto outcome = service->lastConfigure();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->succeeded());
    EXPECT_TRUE(outcome->modelLoaded);
    EXPECT_EQ(outcome->counts.warnings, 1U);
    ASSERT_NE(service->model(), nullptr);
    EXPECT_NE(service->model()->findTarget("core"), nullptr);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].diagnostic.severity, Severity::Warning);
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/CMakeLists.txt"));
    EXPECT_EQ(diagnostics[0].diagnostic.line, 2);
    EXPECT_TRUE(diagnostics[0].jumpable);
    const auto watched = service->watchedFiles();
    EXPECT_NE(std::ranges::find(watched, MemTree::path("tiny/cmake/warnings.cmake")), watched.end());
    EXPECT_NE(std::ranges::find(watched, MemTree::path("tiny/app/CMakeLists.txt")), watched.end());
    {
        LockGuard lock(events.mutex);
        EXPECT_NE(std::ranges::find(events.configureLines, "-- Configuring done (0.1s)"), events.configureLines.end());
        EXPECT_TRUE(events.buildLines.empty());
        ASSERT_EQ(events.configures.size(), 1U);
    }
}

// R0 follow-up 15: trustChanged follows trust granted elsewhere (a parent in Settings, another
// instance) and never writes the store: it neither saves the folder as a root of its own nor
// re-grants a root revoked before the call.
TEST_F(CMakeServiceTest, TrustChangedFollowsTheStoreAndWritesNothing) {
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    service->trustChanged(); // Untrusted: nothing starts.
    waitIdle();
    EXPECT_EQ(service->state(), BuildState::Idle);
    EXPECT_TRUE(launcher.launches().empty());
    ASSERT_TRUE(service->configure().has_value());
    ASSERT_EQ(service->state(), BuildState::NeedsTrust);
    service->trustChanged(); // Still untrusted: the job keeps waiting.
    EXPECT_EQ(service->state(), BuildState::NeedsTrust);
    EXPECT_TRUE(trustStore->roots().empty());
    EXPECT_FALSE(tree.fs.exists(trustStore->storePath()).value_or(true)) << "trustChanged wrote the store";
    service->cancel();
    waitIdle();

    // A parent granted in the store: the folder configures (configure-on-open, never configured yet)
    // and the parent stays the only root.
    const std::string parent(io::parentPath(folder()));
    ASSERT_TRUE(trustStore->trust(parent).has_value());
    installReply(); // As if cmake wrote it.
    expectFirstConfigure();
    service->trustChanged();
    waitIdle();
    EXPECT_EQ(launcher.launches().size(), 2U) << "the store grant did not configure the folder";
    EXPECT_NE(service->model(), nullptr);
    EXPECT_EQ(trustStore->roots(), std::vector<std::string>{parent});
    auto reopened = project::TrustStore::open(tree.fs, trustStore->storePath());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().describe();
    EXPECT_EQ((*reopened)->roots(), std::vector<std::string>{parent}) << "the folder was saved as a root of its own";

    // Revoking the parent untrusts the folder; a trustChanged after it grants nothing back.
    ASSERT_TRUE(trustStore->untrust(parent).has_value());
    service->trustChanged();
    waitIdle();
    EXPECT_FALSE(trustStore->isTrusted(folder()));
    EXPECT_TRUE(trustStore->roots().empty());
    reopened = project::TrustStore::open(tree.fs, trustStore->storePath());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().describe();
    EXPECT_TRUE((*reopened)->roots().empty());
    EXPECT_EQ(launcher.launches().size(), 2U);
}

TEST_F(CMakeServiceTest, ReportsCMakeErrors) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    start();
    expectFirstConfigure({.exitCode = 1,
                          .stderrBytes = "CMake Error at CMakeLists.txt:2 (message):\n  The tiny project is broken.\n\n"
                                         "-- Configuring incomplete, errors occurred!\n"});
    ASSERT_TRUE(service->openFolder(folder()).has_value()); // Trusted: configures at once.
    waitIdle();
    auto outcome = service->lastConfigure();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_EQ(outcome->counts.errors, 1U);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics[0].diagnostic.severity, Severity::Error);
    EXPECT_EQ(diagnostics[0].diagnostic.message, "The tiny project is broken.");
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/CMakeLists.txt"));
    EXPECT_EQ(diagnostics[0].diagnostic.line, 2);
    EXPECT_EQ(service->model(), nullptr);
}

TEST_F(CMakeServiceTest, BuildsCleansAndRebuildsTargets) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    start();
    int saves = 0;
    service->setSaveAllHook([&saves](const SaveAllCompletion& done) {
        ++saves;
        done({});
    });
    expectFirstConfigure();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    const auto& targets = service->model()->targets;
    const auto core =
        static_cast<std::size_t>(std::ranges::find(targets, "core", &project::CMakeTarget::name) - targets.begin());
    const auto current = service->model();
    auto stale = std::make_shared<project::CMakeProject>(*current);
    std::ranges::reverse(stale->targets);
    const auto refused = service->build(BuildAction::Build, core, {}, stale);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    EXPECT_EQ(service->state(), BuildState::Idle);

    const std::string output = "[1/2] Building C object CMakeFiles/core.dir/src/core.c.o\n"
                               "../src/core.c:1:22: warning: unused variable 'x' [-Wunused-variable]\n"
                               "../src/core.c:1:30: error: use of undeclared identifier 'y'\n"
                               "ninja: build stopped: subcommand failed.\n";
    launcher.expect({.expectExecutable = cmake(), .exitCode = 1, .stdoutBytes = output});
    std::optional<CMakeBuildOutcome> completed;
    ASSERT_TRUE(service
                    ->build(
                        BuildAction::Build, core,
                        [&completed](const CMakeBuildOutcome& outcome) { completed = outcome; }, current)
                    .has_value());
    waitIdle();
    ASSERT_TRUE(completed.has_value());
    EXPECT_FALSE(completed->succeeded());
    ASSERT_NE(completed->model, nullptr);
    EXPECT_NE(completed->model->findTarget("core"), nullptr);
    auto failed = service->lastOutcome();
    ASSERT_TRUE(failed.has_value());
    EXPECT_FALSE(failed->succeeded());
    EXPECT_EQ(failed->request.targetName, "core");
    EXPECT_EQ(failed->counts.errors, 1U);
    EXPECT_EQ(failed->counts.warnings, 1U);
    const auto diagnostics = service->diagnostics();
    ASSERT_EQ(diagnostics.size(), 2U);
    EXPECT_EQ(diagnostics[0].path, MemTree::path("tiny/src/core.c")); // Resolved against the build directory.
    EXPECT_EQ(diagnostics[0].diagnostic.code, "-Wunused-variable");
    EXPECT_TRUE(diagnostics[1].jumpable);
    EXPECT_EQ(diagnostics[1].diagnostic.column, 30);
    EXPECT_EQ(service->diagnosticsForDocument(MemTree::path("tiny/src/core.c")).size(), 2U);

    launcher.expect({.expectExecutable = cmake()});
    ASSERT_TRUE(service->build(BuildAction::Clean).has_value());
    waitIdle();
    EXPECT_TRUE(service->lastOutcome()->succeeded());
    EXPECT_TRUE(service->diagnostics().empty()); // Each job starts clean.

    launcher.expect({.expectExecutable = cmake()});
    launcher.expect({.expectExecutable = cmake()});
    ASSERT_TRUE(service->build(BuildAction::Rebuild, core).has_value());
    waitIdle();
    EXPECT_TRUE(service->lastOutcome()->succeeded());

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 6U);
    EXPECT_EQ(launches[2].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "core"}));
    EXPECT_EQ(launches[2].spec.workingDirectory, buildDir());
    EXPECT_EQ(launches[3].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "clean"}));
    EXPECT_EQ(launches[4].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "clean"}));
    EXPECT_EQ(launches[5].spec.arguments, (std::vector<std::string>{"--build", buildDir(), "--target", "core"}));
    EXPECT_EQ(saves, 3);
    EXPECT_EQ(service->build(BuildAction::Build, 99).error().code(), ErrorCode::InvalidArgument);
    LockGuard lock(events.mutex);
    EXPECT_EQ(events.builds.size(), 3U);
    EXPECT_NE(std::ranges::find(events.buildLines, "../src/core.c:1:30: error: use of undeclared identifier 'y'"),
              events.buildLines.end());
}

TEST_F(CMakeServiceTest, ReconfiguresWhenACMakeFileChanges) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    start();
    expectFirstConfigure();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_EQ(launcher.launches().size(), 2U);

    // Unrelated files and the build tree do not reconfigure.
    service->filesChanged(
        {.changes = {io::FileChange{.kind = io::FileChangeKind::Modified, .path = MemTree::path("tiny/src/core.c")},
                     io::FileChange{.kind = io::FileChangeKind::Modified, .path = buildDir("CMakeCache.txt")}},
         .sequence = 1});
    waitIdle();
    EXPECT_EQ(launcher.launches().size(), 2U);
    EXPECT_FALSE(service->modelStale());

    // An included *.cmake file does, once, after the quiet period (the tools are known by now).
    launcher.expect({.expectExecutable = cmake()});
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/cmake/warnings.cmake")}},
                           .sequence = 2});
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/CMakeLists.txt")}},
                           .sequence = 3});
    waitIdle();
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 3U);
    EXPECT_EQ(launches[2].spec.arguments.front(), "-S");
    EXPECT_FALSE(service->modelStale());
    LockGuard lock(events.mutex);
    EXPECT_EQ(events.configures.size(), 2U);
}

TEST_F(CMakeServiceTest, ProfilesFromSettingsArePersisted) {
    settings::RecordList records = {
        settings::Record{settings::RecordField{.name = "name", .value = std::string("Debug")}},
        // Fields sorted by name, as the settings store keeps them.
        settings::Record{settings::RecordField{.name = "buildType", .value = std::string("Release")},
                         settings::RecordField{.name = "cacheVariables", .value = settings::StringList{"A=1"}},
                         settings::RecordField{.name = "jobs", .value = std::int64_t{2}},
                         settings::RecordField{.name = "name", .value = std::string("Release")}},
        settings::Record{settings::RecordField{.name = "buildType", .value = std::string("Nameless")}},
    };
    const auto converted = cmakeProfileRecords(records);
    ASSERT_EQ(converted.size(), 2U);
    EXPECT_EQ(converted[1].buildType, "Release");
    EXPECT_EQ(converted[1].jobs, 2);
    EXPECT_EQ(converted[1].cacheVariables, std::vector<std::string>{"A=1"});

    CMakeServiceOptions withRecords = options();
    withRecords.profileRecords = converted;
    start(withRecords);
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    ASSERT_EQ(service->profiles().size(), 2U);
    EXPECT_EQ(service->activeProfileIndex(), std::optional<std::size_t>(0));
    ASSERT_TRUE(service->setActiveProfile("Release").has_value());
    EXPECT_EQ(service->activeBuildDirectory(), MemTree::path("tiny/cmake-build-release"));
    EXPECT_EQ(service->setActiveProfile("Nope").error().code(), ErrorCode::NotFound);
    waitIdle();
    EXPECT_TRUE(launcher.launches().empty()); // Untrusted: switching runs nothing.

    // A new service (a restart) picks the persisted profile.
    service.reset();
    start(withRecords);
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    EXPECT_EQ(service->activeProfile()->name, "Release");
    ASSERT_TRUE(service->setProfileRecords({}, project::CMakeProfileSource::ApplicationSettings).has_value());
    ASSERT_EQ(service->profiles().size(), 1U); // Back to the default profile.
    EXPECT_EQ(service->activeProfile()->name, "Debug");
    EXPECT_EQ(service->openFolder(MemTree::path("nowhere")).error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(service->closeFolder().has_value());
    EXPECT_EQ(service->activeBuildDirectory(), "");
    EXPECT_EQ(service->configure().error().code(), ErrorCode::InvalidArgument);
}

// CMK-1, stale replies: a build directory whose reply describes another source tree is not this
// folder's model (it counts as never configured).
TEST_F(CMakeServiceTest, AReplyOfAnotherSourceTreeIsNotTheModel) {
    frost::test::installCMakeReply(tree.fs, FROST_CMAKE_REPLY_FIXTURE_DIR, MemTree::path("elsewhere"), buildDir(),
                                   MemTree::path("other"));
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    EXPECT_EQ(service->model(), nullptr) << "a reply of another source tree was published";

    installReply(); // The same directory, configured from this folder: shown.
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->model()->sourceDirectory, folder());
}

/*
 * CMK-1, stale replies: a configure that cmake reports as successful but that left the old reply
 * in place publishes nothing ("no new File API reply"); a failed configure keeps the last model
 * but marks it stale, so the next build configures first.
 */
TEST_F(CMakeServiceTest, AConfigureWithoutANewReplyOrThatFailsLeavesTheModelStale) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    CMakeServiceOptions opts = options();
    opts.configureOnOpen = false;
    service = CMakeService::create(tree.fs, launcher, *trustStore, opts, events.callbacks()); // Writes no reply.
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    const auto shown = service->model();
    ASSERT_NE(shown, nullptr);
    EXPECT_FALSE(service->modelStale());

    expectFirstConfigure();
    ASSERT_TRUE(service->configure().has_value());
    waitIdle();
    auto outcome = service->lastConfigure();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded());
    ASSERT_TRUE(outcome->error.has_value());
    EXPECT_NE(outcome->error->message().find("no new File API reply"), std::string::npos) << outcome->error->describe();
    EXPECT_FALSE(outcome->modelLoaded);
    EXPECT_EQ(service->model(), shown);
    EXPECT_TRUE(service->modelStale());

    // A failing configure (exit 1) keeps the model, stale; Build then configures before building.
    service.reset();
    start(opts); // Through the reply-writing launcher, so a successful configure is fresh again.
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    expectFirstConfigure({.exitCode = 1, .stderrBytes = "CMake Error at CMakeLists.txt:2 (message):\n  Broken.\n\n"});
    ASSERT_TRUE(service->configure().has_value());
    waitIdle();
    ASSERT_TRUE(service->lastConfigure().has_value());
    EXPECT_FALSE(service->lastConfigure()->succeeded());
    ASSERT_NE(service->model(), nullptr);
    EXPECT_TRUE(service->modelStale());
    const std::size_t before = launcher.launches().size();
    launcher.expect({.expectExecutable = cmake()}); // The configure step.
    launcher.expect({.expectExecutable = cmake()}); // The build step.
    ASSERT_TRUE(service->build(BuildAction::Build).has_value());
    waitIdle();
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), before + 2);
    EXPECT_EQ(launches[before].spec.arguments.front(), "-S");
    EXPECT_EQ(launches[before + 1].spec.arguments.front(), "--build");
    EXPECT_FALSE(service->modelStale());
}

// CMK-1, stale replies: a reply older than one of its CMake inputs (edited while FrostIDE was not
// watching) is shown, but stale.
TEST_F(CMakeServiceTest, AReplyOlderThanItsInputsIsStale) {
    installReply();
    tree.clock.advance(5s);
    tree.write("tiny/cmake/warnings.cmake", "# edited after the reply was written\n");
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_TRUE(service->modelStale());

    installReply(); // Written after the edit: current.
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_NE(service->model(), nullptr);
    EXPECT_FALSE(service->modelStale());
}

/*
 * CMK-1, preset-switch state: when a presets file changes the active profile's build directory,
 * the old directory's model is dropped and the new one's reply is read, instead of the old
 * targets staying on screen under the new profile.
 */
TEST_F(CMakeServiceTest, APresetsChangeThatMovesTheBuildDirectoryDropsTheOldModel) {
    tree.write("tiny/CMakePresets.json", R"({"version": 6, "configurePresets": [
        {"name": "dev", "generator": "Ninja", "binaryDir": "${sourceDir}/cmake-build-debug"}]})");
    installReply();
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_EQ(service->activeProfile()->name, "dev");
    ASSERT_NE(service->model(), nullptr);

    tree.write("tiny/CMakePresets.json", R"({"version": 6, "configurePresets": [
        {"name": "dev", "generator": "Ninja", "binaryDir": "${sourceDir}/out/dev"}]})");
    service->filesChanged({.changes = {io::FileChange{.kind = io::FileChangeKind::Modified,
                                                      .path = MemTree::path("tiny/CMakePresets.json")}},
                           .sequence = 1});
    ASSERT_TRUE(service->waitForIdle(kWait));
    EXPECT_EQ(service->activeBuildDirectory(), MemTree::path("tiny/out/dev"));
    EXPECT_EQ(service->model(), nullptr) << "the old build directory's model survived the presets change";
    EXPECT_TRUE(launcher.launches().empty()); // Untrusted: the reload configures nothing.
}

/*
 * CMK-1: profiles of build presets. A folder whose persisted profile is a configure preset name
 * (from before the preset had several build presets) opens on that preset's first profile, and
 * switching between profiles of one configure preset reads the reply in the build preset's
 * configuration.
 */
TEST_F(CMakeServiceTest, BuildPresetProfilesKeepAPersistedConfigurePresetAndReadTheirConfiguration) {
    tree.write("tiny/CMakePresets.json", R"({"version": 6,
      "configurePresets": [{"name": "multi", "generator": "Ninja Multi-Config", "binaryDir": "${sourceDir}/cmake-build-debug"}],
      "buildPresets": [{"name": "multi-debug", "configurePreset": "multi", "configuration": "Debug"},
                       {"name": "multi-release", "configurePreset": "multi", "configuration": "Release"}]})");
    tree.write("data/cmake-profiles.txt", "# FrostIDE CMake profiles v1\n" + folder() + "\tmulti\n");
    installReply();
    start();
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    ASSERT_EQ(service->profiles().size(), 2U);
    EXPECT_EQ(service->activeProfile()->name, "multi-debug");
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->model()->configuration, "Debug");
    ASSERT_TRUE(service->setActiveProfile("multi-release").has_value());
    waitIdle();
    EXPECT_EQ(service->activeBuildDirectory(), buildDir());
    // The synthetic reply has only Debug: the read falls back to its first configuration.
    ASSERT_NE(service->model(), nullptr);
    EXPECT_EQ(service->activeProfile()->buildPreset, "multi-release");
    EXPECT_EQ(service->activeProfile()->effectiveConfiguration(), "Release");
}

/*
 * CMK-15: the progress a configure reports, from the job's state and cmake's own phase marker:
 * Preparing (tools), Configuring, Generating (after "-- Configuring done"), ReadingModel (the
 * reply), then Idle; each change has a greater sequence number.
 */
TEST_F(CMakeServiceTest, AConfigureReportsItsPhases) {
    ASSERT_TRUE(trustStore->trust(folder()).has_value());
    installReply();
    std::vector<CMakeProgress> seen;
    Mutex seenMutex;
    CMakeServiceCallbacks callbacks = events.callbacks();
    callbacks.onProgress = [&](const CMakeProgress& progress) {
        LockGuard lock(seenMutex);
        seen.push_back(progress);
    };
    CMakeServiceOptions opts = options();
    opts.configureOnOpen = false;
    service = CMakeService::create(tree.fs, cmakeLauncher, *trustStore, opts, std::move(callbacks));
    ASSERT_TRUE(service->openFolder(folder()).has_value());
    waitIdle();
    expectFirstConfigure({.stdoutBytes = "-- The C compiler identification is Clang\n-- Configuring done (0.1s)\n"
                                         "-- Generating done (0.0s)\n"});
    ASSERT_TRUE(service->configure().has_value());
    waitIdle();
    ASSERT_TRUE(service->lastConfigure().has_value());
    EXPECT_TRUE(service->lastConfigure()->succeeded());
    EXPECT_EQ(service->progress().activity, CMakeActivity::Idle);

    LockGuard lock(seenMutex);
    std::vector<CMakeActivity> activities;
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (i > 0) {
            EXPECT_GT(seen[i].sequence, seen[i - 1].sequence);
        }
        if (activities.empty() || activities.back() != seen[i].activity) {
            activities.push_back(seen[i].activity);
        }
    }
    // The open's own reply read comes first.
    EXPECT_EQ(activities,
              (std::vector<CMakeActivity>{CMakeActivity::ReadingModel, CMakeActivity::Idle, CMakeActivity::Preparing,
                                          CMakeActivity::Configuring, CMakeActivity::Generating,
                                          CMakeActivity::ReadingModel, CMakeActivity::Idle}));
    const auto configuring = std::ranges::find(seen, CMakeActivity::Configuring, &CMakeProgress::activity);
    ASSERT_NE(configuring, seen.end());
    EXPECT_TRUE(configuring->cancellable);
    EXPECT_EQ(configuring->profile, "Debug");
}

TEST(CMakeServiceInternal, StringAndCommandLineHelpersFormatAndQuoteArguments) {
    EXPECT_EQ(joinStrings({}), "");
    EXPECT_EQ(joinStrings({"only"}), "only");
    EXPECT_EQ(joinStrings({"a", "b", "c"}), "a, b, c");

    EXPECT_EQ(seconds(std::chrono::nanoseconds(1'500'000'000)), "1.5 s");
    EXPECT_EQ(plural(1, "error"), "1 error");
    EXPECT_EQ(plural(0, "error"), "0 errors");
    EXPECT_EQ(plural(3, "warning"), "3 warnings");

    EXPECT_EQ(commandLine(MemTree::path("bin/cmake"), {"--build", "/tmp/my build", "-G", "Ninja"}),
              R"(cmake --build "/tmp/my build" -G Ninja)");
    EXPECT_EQ(commandLine(MemTree::path("Tools/cmake.exe"), {"-S", "src", "-B", "build dir"}),
              R"(cmake.exe -S src -B "build dir")");
}

TEST(CMakeServiceInternal, StateFileAndSettingsHelpersExtractPersistedFields) {
    MemTree mem;
    mem.write("state/profiles.txt",
              std::string(kStateHeader) + "\n/proj/a\tDebug\r\n/proj/b\tRelease\nextra_without_tab\n");
    const std::string stateFile = mem.path("state/profiles.txt");
    const auto entries = readStateFile(mem.fs, stateFile);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0], (std::pair<std::string, std::string>{"/proj/a", "Debug"}));
    EXPECT_EQ(entries[1], (std::pair<std::string, std::string>{"/proj/b", "Release"}));

    mem.write("state/bad.txt", "wrong header\n/proj/a\tDebug\n");
    EXPECT_TRUE(readStateFile(mem.fs, mem.path("state/bad.txt")).empty());
    EXPECT_TRUE(readStateFile(mem.fs, "").empty());
    EXPECT_TRUE(readStateFile(mem.fs, mem.path("state/missing.txt")).empty());

    // Fields sorted alphabetically by name, matching settings::Record's invariant.
    const settings::Record record = {
        settings::RecordField{.name = "args", .value = settings::StringList{"-DFOO=1", "-DBAR=2"}},
        settings::RecordField{.name = "count", .value = std::int64_t{3}},
        settings::RecordField{.name = "name", .value = std::string("Debug")},
    };
    EXPECT_EQ(fieldString(record, "name"), "Debug");
    EXPECT_EQ(fieldString(record, "count"), "");
    EXPECT_EQ(fieldString(record, "missing"), "");
    EXPECT_EQ(fieldList(record, "args"), (std::vector<std::string>{"-DFOO=1", "-DBAR=2"}));
    EXPECT_TRUE(fieldList(record, "name").empty());
    EXPECT_TRUE(fieldList(record, "missing").empty());
}

TEST(CMakeServiceInternal, WatchedPathTouchAndPresetFileHelpersMatchCMakeFiles) {
    const std::string root = kWindows ? R"(C:\work\proj)" : "/work/proj";
    const std::string subFile = kWindows ? R"(C:\work\proj\src\CMakeLists.txt)" : "/work/proj/src/CMakeLists.txt";
    const std::string other = kWindows ? R"(C:\work\other\CMakeLists.txt)" : "/work/other/CMakeLists.txt";
    const std::vector<std::string> watched = {documentKey(subFile)};

    EXPECT_TRUE(touches(io::FileChange{io::FileChangeKind::Modified, subFile, {}}, watched));
    EXPECT_TRUE(touches(io::FileChange{io::FileChangeKind::Created, root, {}}, watched));
    EXPECT_FALSE(touches(io::FileChange{io::FileChangeKind::Modified, root, {}}, watched));
    EXPECT_FALSE(touches(io::FileChange{io::FileChangeKind::Modified, other, {}}, watched));

    EXPECT_TRUE(isPresetsFileName(MemTree::path("proj/CMakePresets.json")));
    EXPECT_TRUE(isPresetsFileName(MemTree::path("proj/CMakeUserPresets.json")));
    EXPECT_FALSE(isPresetsFileName(MemTree::path("proj/CMakeLists.txt")));

    const Diagnostic relDiag = withCMakePath(Diagnostic{.file = "sub/CMakeLists.txt"}, MemTree::path("proj"));
    EXPECT_EQ(relDiag.file, MemTree::path("proj/sub/CMakeLists.txt"));
    const Diagnostic bareDiag = withCMakePath(Diagnostic{.file = "CMakeLists.txt"}, MemTree::path("proj"));
    EXPECT_EQ(bareDiag.file, "CMakeLists.txt");

    project::CMakeProfile a;
    a.buildDirectory = "/build";
    a.generator = "Ninja";
    a.buildType = "Debug";
    project::CMakeProfile b = a;
    EXPECT_TRUE(sameReply(a, b));
    b.generator = "Ninja Multi-Config";
    EXPECT_FALSE(sameReply(a, b));
    a.generator = "Ninja Multi-Config";
    EXPECT_TRUE(sameReply(a, b));
    b.buildType = "Release";
    EXPECT_FALSE(sameReply(a, b));
}

} // namespace
} // namespace frost::build
