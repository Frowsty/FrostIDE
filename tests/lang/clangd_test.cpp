// SPDX-License-Identifier: Apache-2.0
//
// End-to-end tests of ClangdService with the real clangd of the pinned LLVM (label "clangd"):
// a CMake-style compile_commands.json, where an undeclared identifier is reported at the right
// line and clears after the fix; the compile commands synthesized for a tiny .vcxproj (include
// directory and defines from its ItemDefinitionGroup), where only the planted error remains; and
// a restart after clangd is killed, after which the open document is diagnosed again; and (ADR
// 0047) semantic tokens for a local, a parameter, a field and a macro plus the inactive region of
// an #ifdef block. Each test starts one clangd (two for the restart) and stops it before returning.

#include "frost/core/log.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/semantic_tokens.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winioctl.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::lang::ClangdService;
using frost::lang::ClangdServiceOptions;
using frost::lang::ClangdState;
using frost::lang::DiagnosticSeverity;
using frost::lang::FileDiagnostics;

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

// Synthesis options for `configuration` (and system include directories).
frost::lang::SynthesisOptions forConfiguration(std::string configuration, std::vector<std::string> system = {}) {
    frost::lang::SynthesisOptions options;
    options.configuration = std::move(configuration);
    options.systemIncludeDirectories = std::move(system);
    return options;
}

constexpr auto kPatience = 60s; // clangd parses these files in well under a second; this is slack.

std::string fixture(std::string relative) {
    if (kWindows) {
        std::ranges::replace(relative, '/', '\\');
    }
    const std::string base = frost::io::normalizePath(FROST_LANG_FIXTURE_DIR).value();
    return frost::io::normalizePath(frost::io::joinPath(base, relative).value()).value();
}

template<class Condition>
bool waitFor(Condition condition, std::chrono::milliseconds timeout = kPatience) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// The reactor and pool a ClangdService runs on, shut down in the documented order.
class Env {
public:
    Env() {
        frost::test::disableCrashDialogs(); // clangd inherits the error mode: no WER box if it dies.
        auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{1, "frost-test-clangd"});
        if (!reactor) {
            ADD_FAILURE() << reactor.error().describe();
            return;
        }
        m_reactor = std::move(*reactor);
        m_pool = std::make_unique<frost::sched::ThreadPool>(frost::sched::ThreadPoolOptions{.workerCount = 2});
    }
    ~Env() {
        if (m_reactor) {
            m_reactor->shutdown();
        }
        if (m_pool) {
            m_pool->shutdown();
        }
    }
    Env(const Env&) = delete ("owns the reactor");
    Env& operator=(const Env&) = delete ("owns the reactor");

    [[nodiscard]] bool ok() const { return m_reactor && m_pool; }
    [[nodiscard]] frost::io::AsyncContext context() { return frost::io::AsyncContext{*m_reactor, *m_pool}; }
    void run(frost::sched::Task<void> task) { m_pool->spawn(frost::sched::Lane::UserVisible, std::move(task)).join(); }

private:
    std::unique_ptr<frost::io::Reactor> m_reactor;
    std::unique_ptr<frost::sched::ThreadPool> m_pool;
};

std::string clangdPath() {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    EXPECT_TRUE(found.has_value()) << found.error().describe();
    return found ? found->path : std::string();
}

// A service started on `env`; stopped (with the clean shutdown) by stopAndCheck().
std::unique_ptr<ClangdService> startService(Env& env, std::string root,
                                            std::shared_ptr<frost::lang::ICompileCommandsProvider> provider,
                                            frost::lang::RestartPolicy restart = {}) {
    ClangdServiceOptions options;
    options.clangdPath = clangdPath();
    options.workspaceRoot = std::move(root);
    options.launch.jobs = 2;
    options.compileCommands = std::move(provider);
    options.restart = restart;
    auto service = std::make_unique<ClangdService>(frost::io::realProcessLauncher(), env.context(), std::move(options));
    auto started = service->start();
    EXPECT_TRUE(started.has_value()) << started.error().describe();
    return service;
}

void stopAndCheck(Env& env, ClangdService& service) {
    const auto begin = std::chrono::steady_clock::now();
    env.run(service.stop());
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 15s);
    EXPECT_EQ(service.state(), ClangdState::Stopped);
}

bool running(const ClangdService& service) { return service.state() == ClangdState::Running; }

class HeldCommands final : public frost::lang::ICompileCommandsProvider {
public:
    frost::Result<std::optional<frost::lang::CompileCommandsLocation>> locate() override {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        return std::optional<frost::lang::CompileCommandsLocation>{};
    }
    bool waitUntilEntered() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

TEST(ClangdE2E, DestroyDuringHeldDatabaseLookupDoesNotStartClangd) {
    frost::io::ScriptedProcessLauncher launcher;
    auto provider = std::make_shared<HeldCommands>();
    {
        Env env;
        ASSERT_TRUE(env.ok());
        ClangdServiceOptions options;
        options.clangdPath = fixture("dummy-clangd");
        options.workspaceRoot = fixture(".");
        options.compileCommands = provider;
        auto service = std::make_unique<ClangdService>(launcher, env.context(), std::move(options));
        ASSERT_TRUE(service->start());
        const bool entered = provider->waitUntilEntered();
        service.reset();
        provider->release();
        EXPECT_TRUE(entered);
    }
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(ClangdE2E, StopDuringHeldDatabaseLookupDoesNotStartClangd) {
    frost::io::ScriptedProcessLauncher launcher;
    auto provider = std::make_shared<HeldCommands>();
    Env env;
    ASSERT_TRUE(env.ok());
    ClangdServiceOptions options;
    options.clangdPath = fixture("dummy-clangd");
    options.workspaceRoot = fixture(".");
    options.compileCommands = provider;
    ClangdService service(launcher, env.context(), std::move(options));
    ASSERT_TRUE(service.start());
    const bool entered = provider->waitUntilEntered();
    auto stopping = service.stop();
    provider->release();
    env.run(std::move(stopping));
    EXPECT_TRUE(entered);
    EXPECT_EQ(service.state(), ClangdState::Stopped);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(ClangdE2E, GeneratedDatabaseLeaseWaitsForCopiesAndConfirmedExit) {
    frost::test::ScratchDir scratch("generated-database-lease");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const std::string directory = scratch.join("owned");
    const std::string file = scratch.join("owned/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]"));
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    frost::lang::CompileCommandsLocation first{.directory = directory,
                                               .file = file,
                                               .source = frost::lang::CompileCommandsSource::Evaluated,
                                               .generatedLease = lease};
    auto copy = first;
    lease->beginUse();
    lease->confirmExit();
    lease.reset();
    retirement->waitForIdle();
    first.generatedLease.reset();
    EXPECT_TRUE(fs.exists(file).value_or(false));
    copy.generatedLease.reset();
    retirement->waitForIdle();
    if constexpr (kWindows) {
        EXPECT_FALSE(fs.exists(file).value_or(true));
        EXPECT_FALSE(fs.exists(directory).value_or(true));
    } else {
        EXPECT_TRUE(fs.exists(file).value_or(false));
        ASSERT_TRUE(fs.removeFile(file));
        ASSERT_TRUE(fs.removeDirectory(directory));
    }

    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]"));
    auto uncertain = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                           std::vector<std::string>{}, retirement);
    uncertain->beginUse();
    uncertain->abandonUse();
    uncertain.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(fs.exists(file).value_or(false));
    ASSERT_TRUE(fs.removeFile(file));
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]"));
    const std::string unknown = scratch.join("owned/unrelated.txt");
    ASSERT_TRUE(fs.writeFileAtomic(unknown, "keep"));
    auto owned = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    owned.reset();
    retirement->waitForIdle();
    if constexpr (kWindows)
        EXPECT_FALSE(fs.exists(file).value_or(true));
    else {
        EXPECT_TRUE(fs.exists(file).value_or(false));
        ASSERT_TRUE(fs.removeFile(file));
    }
    EXPECT_TRUE(fs.exists(unknown).value_or(false));
    EXPECT_TRUE(fs.exists(directory).value_or(false));
    ASSERT_TRUE(fs.removeFile(unknown));
    auto empty = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    empty.reset();
    retirement->waitForIdle();
    if constexpr (kWindows)
        EXPECT_FALSE(fs.exists(directory).value_or(true));
    else
        EXPECT_TRUE(fs.exists(directory).value_or(false));
    const std::string boundaryFile = scratch.join("compile_commands.json");
    ASSERT_TRUE(fs.writeFileAtomic(boundaryFile, "keep"));
    auto boundary = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), scratch.path(),
                                                                          std::vector<std::string>{}, retirement);
    boundary.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(fs.exists(boundaryFile).value_or(false));
}

class FixedCommands final : public frost::lang::ICompileCommandsProvider {
public:
    explicit FixedCommands(frost::lang::CompileCommandsLocation location) : m_location(std::move(location)) {}
    frost::Result<std::optional<frost::lang::CompileCommandsLocation>> locate() override { return m_location; }

private:
    frost::lang::CompileCommandsLocation m_location;
};

TEST(ClangdE2E, GeneratedDatabaseSurvivesRealClangdUntilLastLocationReleased) {
    frost::test::ScratchDir scratch("generated-database-real-clangd");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const std::string directory = scratch.join("owned");
    const std::string file = scratch.join("owned/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]"));
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    frost::lang::CompileCommandsLocation location{.directory = directory,
                                                  .file = file,
                                                  .source = frost::lang::CompileCommandsSource::Evaluated,
                                                  .generatedLease = lease};
    auto provider = std::make_shared<FixedCommands>(location);
    auto service = startService(env, scratch.path(), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service); }));
    stopAndCheck(env, *service);
    service.reset();
    provider.reset();
    lease.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(fs.exists(file).value_or(false));
    location.generatedLease.reset();
    retirement->waitForIdle();
    EXPECT_EQ(fs.exists(file).value_or(!kWindows), !kWindows);
}

TEST(ClangdE2E, GeneratedDatabaseCleanupRefusesLinkedAncestor) {
    frost::test::ScratchDir scratch("generated-database-link");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const std::string target = scratch.join("target");
    const std::string linked = scratch.join("link");
    const std::string directory = scratch.join("link/owned");
    const std::string file = scratch.join("target/owned/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(scratch.join("target/owned")));
    ASSERT_TRUE(fs.writeFileAtomic(file, "[]"));
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0u);
    frost::io::ProcessSpec spec;
    spec.executable = frost::io::fromWide(std::wstring_view(system.data(), length)).value() + "\\cmd.exe";
    spec.arguments = {"/c", "mklink", "/J", linked, target};
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    const auto junction = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 30s);
    ASSERT_TRUE(junction);
    ASSERT_EQ(junction->exitCode, 0) << junction->stdoutBytes;
#else
    std::error_code error;
    auto targetPath = frost::io::toStdPath(target);
    auto linkPath = frost::io::toStdPath(linked);
    ASSERT_TRUE(targetPath && linkPath);
    std::filesystem::create_directory_symlink(*targetPath, *linkPath, error);
    if (error)
        GTEST_SKIP() << "cannot create directory symlink: " << error.message();
#endif
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    lease.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(fs.exists(file).value_or(false));
}

TEST(ClangdE2E, GeneratedSiblingDirectoriesRetireOnlyAfterBothLeases) {
    frost::test::ScratchDir scratch("generated-database-siblings");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const std::string root = scratch.join("generation");
    const std::string left = scratch.join("generation/left");
    const std::string right = scratch.join("generation/right");
    ASSERT_TRUE(fs.createDirectories(left));
    ASSERT_TRUE(fs.createDirectories(right));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("generation/left/compile_commands.json"), "[]"));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("generation/right/compile_commands.json"), "[]"));
    auto first = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), left,
                                                                       std::vector<std::string>{root}, retirement);
    auto second = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), right,
                                                                        std::vector<std::string>{root}, retirement);
    first.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(fs.exists(root).value_or(false));
    EXPECT_TRUE(fs.exists(scratch.join("generation/right/compile_commands.json")).value_or(false));
    second.reset();
    retirement->waitForIdle();
    EXPECT_EQ(fs.exists(root).value_or(!kWindows), !kWindows);
}

TEST(ClangdE2E, GeneratedCleanupRetainsWhenRetirementAuthorityClosed) {
    frost::test::ScratchDir scratch("generated-retirement-closed");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const auto directory = scratch.join("owned");
    const auto file = scratch.join("owned/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.writeFileAtomic(file, "keep"));
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    retirement->close();
    lease.reset();
    retirement->waitForIdle();
    EXPECT_EQ(fs.readFile(file).value_or(""), "keep");
}

TEST(ClangdE2E, GeneratedCleanupRunsOffLastReleaseThread) {
#ifndef _WIN32
    GTEST_SKIP() << "handle-bound deletion is implemented on Windows";
#else
    frost::test::ScratchDir scratch("generated-retirement-thread");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const auto directory = scratch.join("owned");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("owned/compile_commands.json"), "[]"));
    const auto releasing = std::this_thread::get_id();
    std::thread::id cleaning;
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement,
                                                                       [&] { cleaning = std::this_thread::get_id(); });
    lease.reset();
    retirement->waitForIdle();
    EXPECT_NE(cleaning, std::thread::id{});
    EXPECT_NE(cleaning, releasing);
#endif
}

TEST(ClangdE2E, GeneratedCleanupRetainsOnPendingOpenWithoutBlocking) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows pending-open handling is unavailable";
#else
    frost::test::ScratchDir scratch("generated-retirement-pending");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    for (auto probe : {frost::lang::GeneratedCommandsLease::PendingProbe::Timeout,
                       frost::lang::GeneratedCommandsLease::PendingProbe::StillPending}) {
        const auto directory = scratch.join(
            probe == frost::lang::GeneratedCommandsLease::PendingProbe::Timeout ? "timeout" : "still-pending");
        const auto file = frost::io::joinPath(directory, "compile_commands.json").value();
        ASSERT_TRUE(fs.createDirectories(directory));
        ASSERT_TRUE(fs.writeFileAtomic(file, "keep"));
        auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(
            fs, scratch.path(), directory, std::vector<std::string>{}, retirement, std::function<void()>{}, probe);
        const auto start = std::chrono::steady_clock::now();
        lease.reset();
        retirement->waitForIdle();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
        EXPECT_EQ(fs.readFile(file).value_or(""), "keep");
    }
#endif
}

TEST(ClangdE2E, GeneratedCleanupPreventsAncestorReplacementDuringDeletion) {
#ifndef _WIN32
    GTEST_SKIP() << "handle-bound deletion is implemented on Windows";
#else
    frost::test::ScratchDir scratch("generated-database-rename-race");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const auto ancestor = scratch.join("generation");
    const auto moved = scratch.join("moved");
    const auto directory = scratch.join("generation/owned");
    const auto outside = scratch.join("outside/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.createDirectories(scratch.join("outside")));
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("generation/owned/compile_commands.json"), "[]"));
    ASSERT_TRUE(fs.writeFileAtomic(outside, "keep"));
    auto from = frost::io::toWin32Path(ancestor);
    auto to = frost::io::toWin32Path(moved);
    ASSERT_TRUE(from && to);
    bool attempted = false;
    bool renamed = false;
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(
        fs, scratch.path(), directory, std::vector<std::string>{ancestor}, retirement, [&] {
            attempted = true;
            renamed = MoveFileExW(from->c_str(), to->c_str(), 0) != FALSE;
        });
    lease.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(attempted);
    EXPECT_FALSE(renamed);
    EXPECT_EQ(fs.readFile(outside).value_or(""), "keep");
    EXPECT_FALSE(fs.exists(ancestor).value_or(true));
#endif
}

TEST(ClangdE2E, GeneratedCleanupDoesNotFollowRetargetedHeldDirectory) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows reparse points are not available";
#else
    frost::test::ScratchDir scratch("generated-database-retarget-race");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const auto directory = scratch.join("generation/owned");
    const auto original = scratch.join("generation/owned/compile_commands.json");
    const auto saved = scratch.join("saved.json");
    const auto foreignDirectory = scratch.join("foreign");
    const auto foreign = scratch.join("foreign/compile_commands.json");
    ASSERT_TRUE(fs.createDirectories(directory));
    ASSERT_TRUE(fs.createDirectories(foreignDirectory));
    ASSERT_TRUE(fs.writeFileAtomic(original, "owned"));
    ASSERT_TRUE(fs.writeFileAtomic(foreign, "foreign"));
    auto directoryWide = frost::io::toWin32Path(directory);
    auto originalWide = frost::io::toWin32Path(original);
    auto savedWide = frost::io::toWin32Path(saved);
    auto foreignWide = frost::io::toWide(foreignDirectory);
    ASSERT_TRUE(directoryWide && originalWide && savedWide && foreignWide);
    const std::wstring substitute = L"\\??\\" + *foreignWide;
    const std::size_t subBytes = substitute.size() * sizeof(wchar_t);
    const std::size_t targetBytes = foreignWide->size() * sizeof(wchar_t);
    const std::size_t pathBytes = subBytes + sizeof(wchar_t) + targetBytes + sizeof(wchar_t);
    std::vector<unsigned char> data(16 + pathBytes, 0);
    const auto put = [&](std::size_t offset, auto value) { std::memcpy(data.data() + offset, &value, sizeof(value)); };
    put(0, static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT));
    put(4, static_cast<WORD>(8 + pathBytes));
    put(8, static_cast<WORD>(0));
    put(10, static_cast<WORD>(subBytes));
    put(12, static_cast<WORD>(subBytes + sizeof(wchar_t)));
    put(14, static_cast<WORD>(targetBytes));
    std::memcpy(data.data() + 16, substitute.data(), subBytes);
    std::memcpy(data.data() + 16 + subBytes + sizeof(wchar_t), foreignWide->data(), targetBytes);
    bool moved = false, retargeted = false;
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(
        fs, scratch.path(), directory, std::vector<std::string>{scratch.join("generation")}, retirement, [&] {
            moved = MoveFileW(originalWide->c_str(), savedWide->c_str()) != FALSE;
            HANDLE writer = CreateFileW(directoryWide->c_str(), FILE_WRITE_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (writer != INVALID_HANDLE_VALUE) {
                DWORD returned = 0;
                retargeted = DeviceIoControl(writer, FSCTL_SET_REPARSE_POINT, data.data(),
                                             static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr) != FALSE;
                CloseHandle(writer);
            }
        });
    lease.reset();
    retirement->waitForIdle();
    EXPECT_TRUE(moved);
    EXPECT_TRUE(retargeted);
    EXPECT_EQ(fs.readFile(foreign).value_or(""), "foreign");
    EXPECT_EQ(fs.readFile(saved).value_or(""), "owned");
#endif
}

TEST(ClangdE2E, GeneratedCleanupPreservesDistinctCaseForeignFile) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows case-sensitive directories are not available";
#else
    frost::test::ScratchDir scratch("generated-database-case");
    Env env;
    ASSERT_TRUE(env.ok());
    auto retirement = std::make_shared<frost::lang::GeneratedCommandsRetirement>(env.context().scheduler);
    auto& fs = frost::io::realFileSystem();
    const auto directory = scratch.join("owned");
    const auto foreign = scratch.join("owned/COMPILE_COMMANDS.JSON");
    ASSERT_TRUE(fs.createDirectories(directory));
    auto wide = frost::io::toWin32Path(directory);
    ASSERT_TRUE(wide);
    HANDLE handle =
        CreateFileW(wide->c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = SetFileInformationByHandle(handle, FileCaseSensitiveInfo, &info, sizeof(info)) != FALSE;
    CloseHandle(handle);
    if (!enabled)
        GTEST_SKIP() << "case-sensitive directories are unavailable";
    ASSERT_TRUE(fs.writeFileAtomic(foreign, "foreign"));
    auto lease = std::make_shared<frost::lang::GeneratedCommandsLease>(fs, scratch.path(), directory,
                                                                       std::vector<std::string>{}, retirement);
    lease.reset();
    retirement->waitForIdle();
    EXPECT_EQ(fs.readFile(foreign).value_or(""), "foreign");
#endif
}

std::vector<frost::lang::LiveDiagnostic> errorsIn(ClangdService& service, const std::string& path) {
    std::vector<frost::lang::LiveDiagnostic> out;
    if (const auto set = service.diagnostics().forFile(path)) {
        for (const auto& d : set->diagnostics) {
            if (d.severity == DiagnosticSeverity::Error) {
                out.push_back(d);
            }
        }
    }
    return out;
}

std::string describe(ClangdService& service) {
    std::string out = std::string("state ") + std::string(frost::lang::toString(service.state()));
    if (auto error = service.lastError()) {
        out += "; last error: " + error->describe();
    }
    for (const std::string& line : service.recentStderr()) {
        out += "\n  " + line;
    }
    return out;
}

// The CMake-style fixture: compile_commands.json under <scratch>/build naming the fixture sources.
void writeCmakeDatabase(const frost::test::ScratchDir& scratch) {
    const std::string source = fixture("cmake_project/src/main.cpp");
    const std::vector<frost::lang::CompileCommand> commands = {frost::lang::CompileCommand{
        .directory = scratch.path(),
        .file = source,
        .arguments = {"clang++", "-std=c++20", "-I" + fixture("cmake_project/include"), "-c", source}}};
    ASSERT_TRUE(
        frost::lang::writeCompileCommands(frost::io::realFileSystem(), scratch.join("build"), commands).has_value());
}

frost::text::Document load(const std::string& path) {
    auto text = frost::io::realFileSystem().readFile(path);
    EXPECT_TRUE(text.has_value());
    auto document = frost::text::Document::fromUtf8(text ? *text : std::string());
    EXPECT_TRUE(document.has_value());
    return std::move(*document);
}

TEST(ClangdE2E, CmakeDatabaseReportsAndClearsAnUndeclaredIdentifier) {
    frost::test::ScratchDir scratch("lang-clangd-cmake");
    writeCmakeDatabase(scratch);
    Env env;
    ASSERT_TRUE(env.ok());
    auto provider =
        std::make_shared<frost::lang::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), scratch.path());
    auto service = startService(env, scratch.path(), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    ASSERT_TRUE(service->compileCommands().has_value());
    EXPECT_EQ(service->compileCommands()->directory, scratch.join("build"));
    EXPECT_EQ(service->capabilities()->positionEncoding, frost::lang::PositionEncoding::Utf8);

    const std::string path = fixture("cmake_project/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto errors = errorsIn(*service, path);
    ASSERT_EQ(errors.size(), 1U) << errors.front().message;
    EXPECT_EQ(errors[0].line, 5);
    EXPECT_EQ(errors[0].column, 14);
    EXPECT_EQ(errors[0].code, "undeclared_var_use");
    EXPECT_EQ(errors[0].source, "clang");
    EXPECT_NE(errors[0].message.find("missingName"), std::string::npos);
    ASSERT_TRUE(errors[0].range.has_value());
    EXPECT_EQ(document.text().text(*errors[0].range), "missingName");

    // The fix, sent as an incremental didChange.
    const std::string text = document.text().text();
    const std::size_t at = text.find("missingName");
    auto changes =
        document.replace(frost::text::ByteRange{.begin = frost::ByteOffset{at}, .end = frost::ByteOffset{at + 11}}, "1",
                         frost::text::EditContext{});
    ASSERT_TRUE(changes.has_value());
    ASSERT_TRUE(service->changeDocument(path, *changes, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] {
        const auto set = service->diagnostics().forFile(path);
        return !set || (set->lspVersion == 2 && set->diagnostics.empty());
    })) << describe(*service);
    EXPECT_TRUE(errorsIn(*service, path).empty());
    ASSERT_TRUE(service->saveDocument(path).has_value());
    ASSERT_TRUE(service->closeDocument(path).has_value());
    stopAndCheck(env, *service);
    EXPECT_FALSE(service->serverPid().has_value());
}

TEST(ClangdE2E, SynthesizedVcxprojCommandsCarryIncludesAndDefines) {
    frost::test::ScratchDir scratch("lang-clangd-vcx");
    auto& fs = frost::io::realFileSystem();
    auto model = frost::project::SolutionModel::load(fs, fixture("msvc_solution/Tiny.sln"));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    std::function<frost::Result<frost::lang::MsvcEnvironment>()> msvc;
    if constexpr (kWindows) {
        msvc = [&fs] { return frost::lang::findMsvcEnvironment(frost::io::realProcessLauncher(), fs); };
    }
    auto provider = std::make_shared<frost::lang::SolutionCompileCommandsProvider>(
        fs, std::make_shared<const frost::project::SolutionModel>(std::move(*model)), scratch.join("ccdb"),
        forConfiguration("Debug|x64"), msvc);
    Env env;
    ASSERT_TRUE(env.ok());
    auto service = startService(env, fixture("msvc_solution"), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    ASSERT_TRUE(service->compileCommands().has_value());
    EXPECT_EQ(service->compileCommands()->source, frost::lang::CompileCommandsSource::Synthesized);

    const std::string path = fixture("msvc_solution/Tiny/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto errors = errorsIn(*service, path);
    std::string all;
    for (const auto& e : errors) {
        all += std::to_string(e.line) + ": " + e.message + "\n";
    }
    // tiny.h was found (no "file not found") and TINY_DEBUG was defined (no #error from tiny.h).
    ASSERT_EQ(errors.size(), 1U) << all;
    EXPECT_EQ(errors[0].line, 5);
    EXPECT_EQ(errors[0].code, "undeclared_var_use");
    EXPECT_NE(errors[0].message.find("notDeclaredAnywhere"), std::string::npos);
    stopAndCheck(env, *service);
}

TEST(ClangdE2E, RestartsAfterAKillAndDiagnosesAgain) {
    frost::test::ScratchDir scratch("lang-clangd-restart");
    writeCmakeDatabase(scratch);
    Env env;
    ASSERT_TRUE(env.ok());
    auto provider =
        std::make_shared<frost::lang::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), scratch.path());
    auto service =
        startService(env, scratch.path(), provider,
                     frost::lang::RestartPolicy{
                         .initialDelay = 100ms, .maxDelay = 1s, .maxConsecutiveCrashes = 3, .stableAfter = 60s});
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    const std::string path = fixture("cmake_project/src/main.cpp");
    frost::text::Document document = load(path);
    ASSERT_TRUE(service->openDocument(path, document.text()).has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    const auto firstPid = service->serverPid();
    ASSERT_TRUE(firstPid.has_value());

    // Clear the store so the republish after the restart is visible.
    service->diagnostics().clear();
    ASSERT_TRUE(service->killServerForTesting().has_value());
    ASSERT_TRUE(waitFor([&] { return service->restarts() == 1 && running(*service); })) << describe(*service);
    EXPECT_NE(service->serverPid(), firstPid);
    ASSERT_TRUE(service->lastError().has_value());
    ASSERT_TRUE(waitFor([&] { return !errorsIn(*service, path).empty(); })) << describe(*service);
    EXPECT_EQ(errorsIn(*service, path).front().line, 5);
    stopAndCheck(env, *service);
}

TEST(ClangdE2E, SemanticTokensAndInactiveRegionsOnAFixture) {
    namespace fl = frost::lang;
    frost::test::ScratchDir scratch("lang-clangd-semantic");
    const std::string source = fixture("semantic/semantic.cpp");
    const std::vector<fl::CompileCommand> commands = {fl::CompileCommand{
        .directory = scratch.path(), .file = source, .arguments = {"clang++", "-std=c++20", "-c", source}}};
    ASSERT_TRUE(fl::writeCompileCommands(frost::io::realFileSystem(), scratch.join("build"), commands).has_value());
    Env env;
    ASSERT_TRUE(env.ok());
    auto provider = std::make_shared<fl::ExistingCompileCommandsProvider>(frost::io::realFileSystem(), scratch.path());
    auto service = startService(env, scratch.path(), provider);
    ASSERT_TRUE(waitFor([&] { return running(*service) || service->state() == ClangdState::Failed; }));
    ASSERT_TRUE(running(*service)) << describe(*service);
    const std::optional<fl::SemanticTokensSupport> support = fl::semanticTokensSupport(*service->capabilities());
    ASSERT_TRUE(support.has_value());
    EXPECT_TRUE(support->full);

    frost::text::Document document = load(source);
    const frost::text::Snapshot& snapshot = document.text();
    ASSERT_TRUE(service->openDocument(source, snapshot).has_value());

    // The first AST build sends the inactive regions (the extension this client announces).
    ASSERT_TRUE(waitFor([&] {
        const auto regions = service->inactiveRegions(source);
        return regions && !regions->ranges.empty();
    })) << describe(*service);
    const fl::InactiveRegions regions = *service->inactiveRegions(source);
    EXPECT_EQ(regions.textVersion, snapshot.version());
    EXPECT_TRUE(std::ranges::any_of(regions.ranges, [&](const frost::text::ByteRange& range) {
        return snapshot.text(range).find("disabledCode") != std::string::npos;
    }));

    frost::Result<fl::SemanticTokensData> data;
    env.run([&]() -> frost::sched::Task<void> {
        data = co_await fl::requestSemanticTokens(*service, source, std::nullopt, support->delta);
    }());
    ASSERT_TRUE(data.has_value()) << data.error().describe();
    const auto tokens = fl::decodeSemanticTokens(data->data, support->legend, snapshot, service->encoding());
    ASSERT_TRUE(tokens.has_value()) << tokens.error().describe();
    const auto has = [&](std::string_view name, fl::SemanticTokenType type,
                         std::optional<fl::SemanticModifier> modifier = std::nullopt) {
        return std::ranges::any_of(*tokens, [&](const fl::SemanticToken& token) {
            return snapshot.text(token.range) == name && token.type == type &&
                   (!modifier || fl::hasModifier(token.modifiers, *modifier));
        });
    };
    EXPECT_TRUE(has("local", fl::SemanticTokenType::Variable, fl::SemanticModifier::FunctionScope));
    EXPECT_TRUE(has("factor", fl::SemanticTokenType::Parameter));
    EXPECT_TRUE(has("x", fl::SemanticTokenType::Property));
    EXPECT_TRUE(has("SCALE", fl::SemanticTokenType::Macro));
    EXPECT_TRUE(has("Point", fl::SemanticTokenType::Class) || has("Point", fl::SemanticTokenType::Struct));
    EXPECT_FALSE(std::ranges::any_of(*tokens, [&](const fl::SemanticToken& token) {
        return snapshot.text(token.range) == "disabledCode" && token.type != fl::SemanticTokenType::Comment;
    }));

    // A delta against the unchanged text gives the same tokens.
    if (support->delta && !data->resultId.empty()) {
        frost::Result<fl::SemanticTokensData> again;
        env.run([&]() -> frost::sched::Task<void> {
            again = co_await fl::requestSemanticTokens(*service, source, *data, true);
        }());
        ASSERT_TRUE(again.has_value()) << again.error().describe();
        EXPECT_EQ(again->data, data->data);
    }
    ASSERT_TRUE(service->closeDocument(source).has_value());
    stopAndCheck(env, *service);
}

} // namespace
