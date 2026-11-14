// SPDX-License-Identifier: Apache-2.0
//
// FND-9a: opening a CMake project spawns exactly one clangd. The real cmake configures a tiny C
// project and the real (pinned) clangd starts on its compile_commands.json; every process goes
// through a counting IProcessLauncher over the real launcher, which is the spawn log. Covered: a
// trusted folder whose configure-on-open writes the first database (clangd waits for it), the
// reopen of the configured folder (clangd starts at once and the configure's model does not
// restart it), a later profile switch, which moves the database and must restart clangd exactly
// once (R0 follow-up 10), a trust grant on an untrusted configured folder, which must restart the
// restricted clangd (--enable-config=false) once in full mode, and its revocation, which restricts
// it again; a grant made straight in the trust store, which configures a never-configured folder
// as the prompt does, a trusted parent folder that the watch never saves as a root of the project's
// own, and a revocation during a long build, which restricts clangd at once (R0 follow-up 15).
// CPP-3: clangd reads the IDE's own directory for each configuration (a copy of CMake's database,
// under the profile's cache), so a preset switch restarts it once onto the other directory and a
// switch back finds that configuration's background-index shards there (clangd's verbose log
// shows the index loaded from disk), none of them in the build tree; and the resolve context of a
// header that two targets list: choosing a target makes the running clangd reparse the header with
// that target's defines (its diagnostics follow, checked against `clangd --check` on the owned
// database), without a restart.

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/lang/resolve_context.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "support/clangd_check.hpp"
#include "workbench_rig.hpp"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// The real launcher, counting the clangd processes it starts.
class CountingLauncher final : public frost::io::IProcessLauncher {
public:
    frost::Result<std::unique_ptr<frost::io::IProcess>> launch(const frost::io::ProcessSpec& spec) override {
        count(spec);
        return frost::io::realProcessLauncher().launch(spec);
    }
    frost::Result<std::unique_ptr<frost::io::IAsyncProcess>>
    launchAsync(const frost::io::ProcessSpec& spec, const frost::io::AsyncContext& context) override {
        count(spec);
        return frost::io::realProcessLauncher().launchAsync(spec, context);
    }
    [[nodiscard]] int clangdStarts() const { return m_clangd.load(); }
    // The arguments of the latest clangd start (empty before the first).
    [[nodiscard]] std::vector<std::string> lastClangdArguments() const {
        const std::scoped_lock lock(m_mutex);
        return m_lastArguments;
    }

private:
    void count(const frost::io::ProcessSpec& spec) {
        const std::string_view name = frost::io::fileName(spec.executable);
        if (name == "clangd" || name == "clangd.exe") {
            const std::scoped_lock lock(m_mutex);
            m_lastArguments = spec.arguments;
            ++m_clangd;
        }
    }
    std::atomic<int> m_clangd{0};
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lastArguments;
};

/*
 * The real file system, holding every read of a CMake File API reply (a path below
 * .cmake/api/v1/reply) on a thread other than the GUI thread while hold() is on, as a slow read of
 * a large reply does on the service thread. Never holds the GUI thread (the test runs there and
 * would deadlock); it records whether that thread read a reply at all. A held read goes on after
 * release() or after 120 s, so a failing test cannot hang.
 */
class HeldReplyReads final : public frost::io::IFileSystem {
public:
    explicit HeldReplyReads(std::thread::id gui) : m_gui(gui) {}

    frost::Result<std::string> readFile(std::string_view path) override {
        wait(path);
        return m_inner.readFile(path);
    }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_inner.writeFileAtomic(path, data);
    }
    frost::Result<void> writeFileAtomicWithMtime(std::string_view path, std::string_view data,
                                                 std::filesystem::file_time_type mtime) override {
        return m_inner.writeFileAtomicWithMtime(path, data, mtime);
    }
    frost::Result<frost::io::SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                                        const frost::io::SafeWriteOptions& options) override {
        return m_inner.safeWrite(path, data, options);
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override { return m_inner.status(path); }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        wait(path);
        return m_inner.listDirectory(path);
    }
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        wait(path);
        return m_inner.listDirectoryWithDiagnostics(path);
    }
    frost::Result<void> createDirectories(std::string_view path) override { return m_inner.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override { return m_inner.removeFile(path); }
    frost::Result<void> removeDirectory(std::string_view path) override { return m_inner.removeDirectory(path); }
    frost::Result<void> rename(std::string_view from, std::string_view to) override { return m_inner.rename(from, to); }
    frost::Result<void> remove(std::string_view path) override { return m_inner.remove(path); }
    frost::Result<void> removeAll(std::string_view path) override { return m_inner.removeAll(path); }
    frost::Result<bool> exists(std::string_view path) override { return m_inner.exists(path); }
    frost::Result<std::string> canonicalPath(std::string_view path) override { return m_inner.canonicalPath(path); }
    frost::Result<std::string> physicalPath(std::string_view path) override { return m_inner.physicalPath(path); }

    void hold() {
        const std::scoped_lock lock(m_mutex);
        m_holding = true;
    }
    void release() {
        {
            const std::scoped_lock lock(m_mutex);
            m_holding = false;
        }
        m_changed.notify_all();
    }
    [[nodiscard]] int held() const {
        const std::scoped_lock lock(m_mutex);
        return m_held;
    }
    [[nodiscard]] bool guiThreadReadAReply() const {
        const std::scoped_lock lock(m_mutex);
        return m_guiRead;
    }

private:
    void wait(std::string_view path) {
        if (path.find(frost::io::kNativePathStyle == frost::io::PathStyle::Windows ? "\\v1\\reply" : "/v1/reply") ==
            std::string_view::npos)
            return;
        std::unique_lock lock(m_mutex);
        if (std::this_thread::get_id() == m_gui) {
            m_guiRead = true;
            return;
        }
        if (!m_holding)
            return;
        ++m_held;
        (void)m_changed.wait_for(lock, 120s, [this] { return !m_holding; });
        --m_held;
    }

    frost::io::IFileSystem& m_inner = frost::io::realFileSystem();
    const std::thread::id m_gui;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_holding = false;
    bool m_guiRead = false;
    int m_held = 0;
};

bool restricted(const std::vector<std::string>& arguments) {
    return std::ranges::find(arguments, std::string("--enable-config=false")) != arguments.end();
}

// A C project with a Debug and a Release Ninja profile; the compiler is the build's own. In the
// folder `sub` below `dir` (with a trailing '/') when given.
void writeProject(WorkDir& dir, const std::string& sub = {}) {
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write(sub + "CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                      "set(CMAKE_C_COMPILER \"" +
                                          compiler +
                                          "\" CACHE FILEPATH \"\" FORCE)\n"
                                          "project(Spawn C)\nadd_executable(hello main.c)\n");
    dir.write(sub + "main.c", "int main(void) { return 0; }\n");
    dir.write(sub + "CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-debug"},
        {"name":"Release","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-release"}]})");
}

RigOptions clangdOptions(CountingLauncher& launcher) {
    RigOptions options;
    options.workbench.clangd = true;
    options.workbench.clangdJobs = 1;
    options.launcher = &launcher;
    return options;
}

// Waits until clangd runs on `buildDir`'s database (CPP-3: in the IDE's own directory for that
// configuration, which holds a copy of it, never the build directory itself), then lets queued
// model and state events arrive, so a restart they would cause is counted too.
bool clangdSettledOn(WorkbenchRig& rig, const std::string& buildDir) {
    const bool running = waitFor(
        [&] {
            const auto* clangd = rig.workbench->clangd();
            const auto location = clangd ? clangd->compileCommands() : std::nullopt;
            return clangd && clangd->state() == frost::lang::ClangdState::Running && location &&
                   location->origin == buildDir && location->directory != buildDir && rig.workbench->cmakeService() &&
                   rig.workbench->cmakeService()->state() == frost::build::BuildState::Idle;
        },
        240s);
    (void)waitFor([] { return false; }, 2s); // Drain the UI queue: no late restart may follow.
    return running;
}

TEST(WorkbenchClangdSpawn, OpeningACMakeProjectStartsOneClangd) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-spawn");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    {
        // First open of a trusted folder: configure-on-open writes the first compile_commands.json.
        CountingLauncher launcher;
        WorkbenchRig rig(dir, clangdOptions(launcher));
        ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
        ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
        ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the configured database";
        EXPECT_EQ(launcher.clangdStarts(), 1) << "the first open spawned more than one clangd";
    }
    {
        // Reopen: the database exists, clangd starts at once and the reconfigure keeps it.
        CountingLauncher launcher;
        WorkbenchRig rig(dir, clangdOptions(launcher));
        ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
        ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the existing database";
        EXPECT_EQ(launcher.clangdStarts(), 1) << "the reopen spawned more than one clangd";

        // A profile switch moves the database: that is a real configuration change and restarts clangd.
        auto* service = rig.workbench->cmakeService();
        ASSERT_NE(service, nullptr);
        ASSERT_TRUE(service->setActiveProfile(std::string_view("Release")).has_value());
        ASSERT_TRUE(clangdSettledOn(rig, dir.join("cmake-build-release")))
            << "clangd did not follow the Release profile's database";
        (void)waitFor([] { return false; }, 2s); // The configure's end and two watch ticks: no late restart.
        EXPECT_EQ(launcher.clangdStarts(), 2) << "the profile switch restarted clangd more than once";
        EXPECT_FALSE(restricted(launcher.lastClangdArguments()));
    }
}

/*
 * R0 follow-up 10, with a busy service thread: the switch's reply read is held (as a large reply
 * read, or the view model built for a large project just before, holds the service thread). The
 * Release profile was never configured, so the switch must not look idle with a database-less
 * build directory while the read waits: the 1 s trust watch restarted clangd without a database
 * in that window and the configure's publication restarted it again (three clangd starts). The
 * hold lasts more than two watch ticks; afterwards the configure writes Release's database and
 * clangd restarts on it exactly once. No reply is ever read on the GUI thread (R0 stall criterion).
 */
TEST(WorkbenchClangdSpawn, AProfileSwitchWithABusyServiceThreadRestartsClangdOnce) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-held-switch");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    HeldReplyReads fs(std::this_thread::get_id());
    const auto releaseReads = qScopeGuard([&] { fs.release(); });
    CountingLauncher launcher;
    RigOptions options = clangdOptions(launcher);
    options.fileSystem = &fs;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the configured database";
    ASSERT_EQ(launcher.clangdStarts(), 1);

    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    fs.hold();
    ASSERT_TRUE(service->setActiveProfile(std::string_view("Release")).has_value());
    ASSERT_TRUE(waitFor([&] { return fs.held() > 0; }, 60s)) << "the switch never read Release's reply";
    (void)waitFor([] { return false; }, 2500ms); // At least two trust-watch ticks with the read held.
    EXPECT_EQ(launcher.clangdStarts(), 1) << "clangd restarted while the switch's reply read was pending";
    fs.release();

    ASSERT_TRUE(clangdSettledOn(rig, dir.join("cmake-build-release")))
        << "clangd did not follow the Release profile's database";
    (void)waitFor([] { return false; }, 2s); // The configure's end and two watch ticks: no late restart.
    EXPECT_EQ(launcher.clangdStarts(), 2) << "the profile switch did not restart clangd exactly once";
    EXPECT_FALSE(restricted(launcher.lastClangdArguments()));
    EXPECT_FALSE(fs.guiThreadReadAReply()) << "a File API reply was read on the GUI thread";
}

TEST(WorkbenchClangdSpawn, TrustingAConfiguredFolderRestartsTheRestrictedClangdOnceInFullModeAndRevokingRestrictsIt) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-trust");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    {
        // Configure once as a trusted folder so the database exists, then forget the grant.
        CountingLauncher launcher;
        WorkbenchRig rig(dir, clangdOptions(launcher));
        ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
        ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
        ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the configured database";
        ASSERT_TRUE(rig.trust->untrust(dir.path()).has_value());
    }
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the existing database untrusted";
    ASSERT_EQ(launcher.clangdStarts(), 1);
    EXPECT_TRUE(restricted(launcher.lastClangdArguments())) << "an untrusted folder's clangd must be restricted";

    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr) << "opening an untrusted folder offers trust";
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 2; }, 60s)) << "the trust grant did not restart clangd";
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "the restarted clangd never ran";
    EXPECT_EQ(launcher.clangdStarts(), 2) << "the trust grant restarted clangd more than once";
    EXPECT_FALSE(restricted(launcher.lastClangdArguments())) << "the restarted clangd is still restricted";

    // Revoking the trust (outside the dialog) restarts clangd once more, restricted again.
    ASSERT_TRUE(rig.trust->untrust(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 3; }, 60s)) << "revoking trust did not restart clangd";
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "the clangd restarted after revocation never ran";
    EXPECT_EQ(launcher.clangdStarts(), 3) << "the revocation restarted clangd more than once";
    EXPECT_TRUE(restricted(launcher.lastClangdArguments())) << "an untrusted folder's clangd must be restricted again";
}

// An untrusted CMake folder that was never configured: clangd starts restricted without a
// database, and the trust grant's configure writes the first one. The configure's publication
// restarts clangd once, trusted, on that database; the 1 s trust watch must not add a restart
// while the configure runs (FND-9a).
TEST(WorkbenchClangdSpawn, TrustingAnUnconfiguredFolderRestartsClangdOnceOnTheNewDatabase) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-trust-unconfigured");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 1; }, 60s)) << "no clangd for the untrusted folder";
    (void)waitFor([] { return false; }, 2s); // No configure may run before trust.
    ASSERT_EQ(launcher.clangdStarts(), 1);
    EXPECT_TRUE(restricted(launcher.lastClangdArguments()));

    auto* dialog = rig.window->findChild<frost::ui::TrustDialog*>(QStringLiteral("trustDialog"));
    ASSERT_NE(dialog, nullptr) << "opening an untrusted folder offers trust";
    dialog->trustButton()->click();
    ASSERT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the database the configure wrote";
    (void)waitFor([] { return false; }, 2s); // Two more trust-watch ticks: no late restart.
    EXPECT_EQ(launcher.clangdStarts(), 2) << "the trust grant and its configure restarted clangd more than once";
    EXPECT_FALSE(restricted(launcher.lastClangdArguments()));
}

// R0 follow-up 15: trust granted straight to the trust store (Settings, another instance) for a
// never-configured CMake folder gets the configure-on-open the trust prompt gets: the trust
// watch asks CMakeService::trustChanged, the configure writes the first database, and its publication
// restarts the restricted clangd once, trusted, on it.
TEST(WorkbenchClangdSpawn, TrustGrantedInTheStoreConfiguresAndRestartsClangdOnce) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-store-trust");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 1; }, 60s)) << "no clangd for the untrusted folder";
    (void)waitFor([] { return false; }, 2s); // No configure may run before trust.
    ASSERT_EQ(launcher.clangdStarts(), 1);
    EXPECT_TRUE(restricted(launcher.lastClangdArguments()));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    EXPECT_EQ(service->model(), nullptr);

    // The Settings route: the store only, no dialog and no CMakeService call by the test.
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "the grant did not configure the folder for clangd";
    (void)waitFor([] { return false; }, 2s); // Two more trust-watch ticks: no late restart.
    EXPECT_NE(service->model(), nullptr) << "the configure-on-open did not run";
    EXPECT_EQ(launcher.clangdStarts(), 2) << "the grant and its configure restarted clangd more than once";
    EXPECT_FALSE(restricted(launcher.lastClangdArguments()));
}

/* R0 follow-up 15: a parent folder trusted in the store (Settings) while a CMake folder below it
 * is open configures the folder and restarts clangd trusted, but the watch saves nothing: the
 * parent stays the only trusted root (in this store and in the file a new instance reads), so
 * revoking the parent untrusts the project and clangd restarts restricted. */
TEST(WorkbenchClangdSpawn, RevokingATrustedParentUntrustsTheOpenProject) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-parent-trust");
    writeProject(dir, "work/proj/");
    const std::string parent = dir.join("work");
    const std::string project = dir.join("work/proj");
    const std::string debugDir = dir.join("work/proj/cmake-build-debug");
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_TRUE(rig.workbench->openPath(project).has_value());
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 1; }, 60s)) << "no clangd for the untrusted folder";
    ASSERT_TRUE(restricted(launcher.lastClangdArguments()));

    ASSERT_TRUE(rig.trust->trust(parent).has_value()); // The Settings route: the store only.
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "the parent grant did not configure the folder";
    ASSERT_NE(rig.workbench->cmakeService()->model(), nullptr);
    EXPECT_FALSE(restricted(launcher.lastClangdArguments()));
    const auto onlyTheParent = [&](const frost::project::TrustStore& store) {
        const std::vector<std::string> roots = store.roots();
        return roots.size() == 1 && frost::lang::documentKey(roots[0]) == frost::lang::documentKey(parent);
    };
    EXPECT_TRUE(onlyTheParent(*rig.trust)) << "the trust watch saved a root of its own";
    {
        auto reopened = frost::project::TrustStore::open(frost::io::realFileSystem(), rig.trust->storePath());
        ASSERT_TRUE(reopened.has_value()) << reopened.error().describe();
        EXPECT_TRUE(onlyTheParent(**reopened)) << "the store file holds more than the parent";
    }

    const int startsBefore = launcher.clangdStarts();
    ASSERT_TRUE(rig.trust->untrust(parent).has_value());
    EXPECT_FALSE(rig.trust->isTrusted(project)) << "revoking the parent left the project trusted";
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() > startsBefore; }, 60s))
        << "revoking the parent did not restart clangd";
    EXPECT_TRUE(restricted(launcher.lastClangdArguments()));
    ASSERT_TRUE(clangdSettledOn(rig, debugDir));
    EXPECT_FALSE(rig.trust->isTrusted(project)) << "the project became trusted again";
    EXPECT_TRUE(rig.trust->roots().empty());
    auto reopened = frost::project::TrustStore::open(frost::io::realFileSystem(), rig.trust->storePath());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().describe();
    EXPECT_TRUE((*reopened)->roots().empty()) << "the store file still trusts something";
    EXPECT_TRUE(restricted(launcher.lastClangdArguments()));
}

// R0 follow-up 15: revoking trust while a long CMake build runs restricts clangd at once (it used
// to keep .clangd and --query-driver until the service was idle), and the build's end adds no
// further restart.
TEST(WorkbenchClangdSpawn, RevokingTrustDuringALongBuildRestrictsClangdAtOnce) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-revoke-build");
    writeProject(dir);
    // A build step that runs until it is cancelled (the real cmake -E sleep, through Ninja).
    dir.write("CMakeLists.txt",
              dir.read("CMakeLists.txt") + "add_custom_target(slow ALL COMMAND \"${CMAKE_COMMAND}\" -E sleep 120)\n");
    const std::string debugDir = dir.join("cmake-build-debug");
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on the configured database";
    ASSERT_EQ(launcher.clangdStarts(), 1);
    ASSERT_FALSE(restricted(launcher.lastClangdArguments()));

    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->build(frost::build::BuildAction::Build).has_value());
    const auto cancelBuild = qScopeGuard([&] {
        service->cancel();
        (void)waitFor([&] { return service->state() == frost::build::BuildState::Idle; }, 120s);
    });
    ASSERT_TRUE(waitFor([&] { return service->state() == frost::build::BuildState::Running; }, 120s));
    ASSERT_TRUE(rig.trust->untrust(dir.path()).has_value());
    ASSERT_TRUE(waitFor([&] { return launcher.clangdStarts() >= 2; }, 30s))
        << "revoking trust during the build did not restart clangd";
    EXPECT_NE(service->state(), frost::build::BuildState::Idle) << "the build ended before the check";
    EXPECT_TRUE(restricted(launcher.lastClangdArguments())) << "the restarted clangd is not restricted";

    service->cancel();
    ASSERT_TRUE(waitFor([&] { return service->state() == frost::build::BuildState::Idle; }, 120s));
    ASSERT_TRUE(clangdSettledOn(rig, debugDir));
    (void)waitFor([] { return false; }, 2s); // The build's end and two watch ticks: no late restart.
    EXPECT_EQ(launcher.clangdStarts(), 2) << "clangd restarted again after the build";
    EXPECT_TRUE(restricted(launcher.lastClangdArguments()));
}

// Every clangd log line the workbench passes on (WorkbenchOptions::clangdLog), thread-safe.
class ClangdLog {
public:
    void add(std::string_view line) {
        const std::scoped_lock lock(m_mutex);
        m_lines.emplace_back(line);
    }
    void clear() {
        const std::scoped_lock lock(m_mutex);
        m_lines.clear();
    }
    [[nodiscard]] bool contains(std::string_view text) const {
        const std::scoped_lock lock(m_mutex);
        return std::ranges::any_of(m_lines,
                                   [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
    [[nodiscard]] std::string matching(std::string_view text) const {
        const std::scoped_lock lock(m_mutex);
        std::string out;
        for (const std::string& line : m_lines)
            if (line.find(text) != std::string::npos)
                out += line + "\n";
        return out;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
};

// Whether clangd's background index wrote a shard into `directory`/.cache/clangd/index.
bool hasShards(const std::string& directory) {
    std::error_code error;
    const std::filesystem::path index = std::filesystem::path(directory) / ".cache" / "clangd" / "index";
    for (const auto& entry : std::filesystem::directory_iterator(index, error))
        if (entry.path().extension() == ".idx")
            return true;
    return false;
}

/*
 * CPP-3's first acceptance: a preset switch restarts clangd once and the clangd log shows shards
 * loaded. Debug's clangd indexes main.c into Debug's own directory; the switch to Release restarts
 * clangd once onto Release's directory, which indexes too; the switch back restarts it once onto
 * Debug's directory again, and clangd's own verbose log says it rebuilt its index "after loading
 * index from disk" (clangd's BackgroundIndexRebuilder logs that only when it loaded shards).
 * Both directories are under the profile's compile-commands cache and hold copies of CMake's
 * databases; no build directory gets a .cache.
 */
TEST(WorkbenchClangdSpawn, APresetSwitchRestartsClangdOnceAndSwitchingBackLoadsThatConfigurationsShards) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-owned-shards");
    writeProject(dir);
    const std::string debugDir = dir.join("cmake-build-debug");
    const std::string releaseDir = dir.join("cmake-build-release");
    const std::string cache = dir.join("data/compile-commands");
    CountingLauncher launcher;
    ClangdLog log;
    RigOptions options = clangdOptions(launcher);
    options.workbench.clangdLogLevel = "verbose";
    options.workbench.clangdLog = [&log](std::string_view line) { log.add(line); };
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd never ran on Debug's database";
    ASSERT_EQ(launcher.clangdStarts(), 1);
    auto& fs = frost::io::realFileSystem();
    const auto debug = rig.workbench->clangd()->compileCommands();
    ASSERT_TRUE(debug.has_value());
    EXPECT_TRUE(debug->directory.starts_with(cache)) << debug->directory << " is not under " << cache;
    EXPECT_EQ(debug->source, frost::lang::CompileCommandsSource::CMakeCopy);
    EXPECT_EQ(fs.readFile(debug->file).value_or("owned"),
              fs.readFile(debugDir + "/compile_commands.json").value_or(""));
    // clangd loads the database, and indexes it, for the first open document.
    ASSERT_TRUE(rig.workbench->openPath(dir.join("main.c")).has_value());
    ASSERT_TRUE(waitFor([&] { return hasShards(debug->directory); }, 240s)) << "no shard in " << debug->directory;

    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->setActiveProfile(std::string_view("Release")).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, releaseDir)) << "clangd did not follow the Release profile";
    EXPECT_EQ(launcher.clangdStarts(), 2) << "the switch to Release did not restart clangd exactly once";
    const auto release = rig.workbench->clangd()->compileCommands();
    ASSERT_TRUE(release.has_value());
    EXPECT_NE(release->directory, debug->directory) << "one directory per configuration";
    EXPECT_TRUE(release->directory.starts_with(cache)) << release->directory;
    ASSERT_TRUE(waitFor([&] { return hasShards(release->directory); }, 240s)) << "no shard in " << release->directory;

    log.clear();
    ASSERT_TRUE(service->setActiveProfile(std::string_view("Debug")).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, debugDir)) << "clangd did not follow the Debug profile back";
    EXPECT_EQ(launcher.clangdStarts(), 3) << "the switch back did not restart clangd exactly once";
    EXPECT_EQ(rig.workbench->clangd()->compileCommands()->directory, debug->directory);
    EXPECT_TRUE(waitFor([&] { return log.contains("after loading index from disk"); }, 120s))
        << "clangd's log shows no index loaded from disk; its BackgroundIndex lines:\n"
        << log.matching("BackgroundIndex");
    EXPECT_FALSE(fs.exists(debugDir + "/.cache").value_or(true)) << "clangd wrote into the build tree";
    EXPECT_FALSE(fs.exists(releaseDir + "/.cache").value_or(true)) << "clangd wrote into the build tree";
}

// The resolve-context fixture: two C++ libraries list shared.h with different defines (the
// compiler is the build's own C++ compiler).
void writeContextsProject(WorkDir& dir) {
    std::string compiler = FROST_TEST_CXX_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                "set(CMAKE_CXX_COMPILER \"" +
                                    compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\n"
                                    "project(Contexts CXX)\n"
                                    "add_library(alpha STATIC alpha.cpp shared.h)\n"
                                    "target_compile_definitions(alpha PRIVATE CONTEXT_ALPHA=1)\n"
                                    "add_library(beta STATIC beta.cpp shared.h)\n"
                                    "target_compile_definitions(beta PRIVATE CONTEXT_BETA=1)\n");
    dir.write("shared.h", "#pragma once\n#if defined(CONTEXT_ALPHA)\n#error \"compiled for alpha\"\n"
                          "#elif defined(CONTEXT_BETA)\n#error \"compiled for beta\"\n#endif\n");
    dir.write("alpha.cpp", "int alpha() { return CONTEXT_ALPHA; }\n");
    dir.write("beta.cpp", "int beta() { return CONTEXT_BETA; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
        {"name":"Debug","generator":"Ninja","binaryDir":"${sourceDir}/cmake-build-debug"}]})");
}

// Whether the live diagnostics clangd published for `file` mention `text`.
bool liveDiagnosticsMention(WorkbenchRig& rig, const std::string& file, std::string_view text) {
    auto* clangd = rig.workbench->clangd();
    const auto set = clangd ? clangd->diagnostics().forFile(file) : std::nullopt;
    return set && std::ranges::any_of(set->diagnostics, [&](const frost::lang::LiveDiagnostic& d) {
               return d.message.find(text) != std::string::npos;
           });
}

/*
 * CPP-3's second acceptance: a header resolve-context choice changes its diagnostics. shared.h is
 * listed by alpha and beta, so the status bar's selector offers both (and Automatic, where clangd
 * infers the command). Choosing beta from its menu makes the running clangd reparse the header with
 * beta's defines: its live diagnostics name beta; choosing alpha names alpha. The oracle for each
 * choice is `clangd --check` on the owned database the choice rewrote (the chosen command comes
 * first), which must report the same #error. No choice restarts clangd.
 */
TEST(WorkbenchClangdSpawn, AHeaderResolveContextChoiceChangesItsDiagnostics) {
    auto found = frost::lang::findClangd(frost::io::realFileSystem(), frost::lang::defaultClangdSearch());
    ASSERT_TRUE(found.has_value()) << "the pinned clangd is required: " << found.error().describe();
    WorkDir dir("cmake-clangd-resolve-context");
    writeContextsProject(dir);
    const std::string buildDir = dir.join("cmake-build-debug");
    const std::string header = dir.join("shared.h");
    CountingLauncher launcher;
    WorkbenchRig rig(dir, clangdOptions(launcher));
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    ASSERT_TRUE(clangdSettledOn(rig, buildDir)) << "clangd never ran on the configured database";
    ASSERT_TRUE(rig.workbench->openPath(header).has_value());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->clangd()->diagnostics().forFile(header).has_value(); }, 120s))
        << "clangd published nothing for the header";

    auto* selector = rig.window->findChild<frost::ui::lang::ResolveContextButton*>(QStringLiteral("resolveContext"));
    ASSERT_NE(selector, nullptr) << "the status bar has no resolve-context selector";
    ASSERT_TRUE(waitFor([&] { return !selector->isHidden(); }, 10s))
        << "the selector stays hidden for a header two targets list";
    EXPECT_EQ(selector->targets(), (QStringList{QStringLiteral("alpha"), QStringLiteral("beta")}));
    EXPECT_EQ(selector->current(), -1) << "automatic until the user chooses";
    const std::string owned = rig.workbench->clangd()->compileCommands()->directory;

    for (const char* target : {"beta", "alpha"}) {
        SCOPED_TRACE(target);
        const std::string expected = std::string("compiled for ") + target;
        const std::string other =
            std::string("compiled for ") + (std::string_view(target) == "beta" ? "alpha" : "beta");
        QAction* action = nullptr;
        for (QAction* candidate : selector->menu()->actions())
            if (candidate->text() == QString::fromUtf8(target))
                action = candidate;
        ASSERT_NE(action, nullptr) << "the menu has no " << target;
        action->trigger(); // As a click on the menu item.
        ASSERT_TRUE(waitFor([&] { return liveDiagnosticsMention(rig, header, expected); }, 120s))
            << "the header's diagnostics never named " << target;
        EXPECT_FALSE(liveDiagnosticsMention(rig, header, other));
        EXPECT_EQ(selector->text(), QStringLiteral("Context: ") + QString::fromUtf8(target));
        // The oracle: clangd itself on the owned database the choice rewrote.
        ASSERT_TRUE(waitFor(
            [&] {
                const auto text = frost::io::realFileSystem().readFile(owned + "/compile_commands.json");
                const std::size_t at = text ? text->find("shared.h") : std::string::npos;
                return at != std::string::npos && at < text->find("alpha.cpp") && at < text->find("beta.cpp");
            },
            60s))
            << "the owned database does not lead with the chosen context";
        auto check = frost::test::runClangdCheck(found->path, header, owned);
        ASSERT_TRUE(check.has_value()) << check.error().describe();
        EXPECT_EQ(check->origin, "from CDB") << check->log;
        EXPECT_TRUE(std::ranges::any_of(check->errors, [&](const std::string& e) {
            return e.find(expected) != std::string::npos;
        })) << check->log;
    }
    EXPECT_EQ(launcher.clangdStarts(), 1) << "a resolve-context choice restarted clangd";
}

} // namespace
