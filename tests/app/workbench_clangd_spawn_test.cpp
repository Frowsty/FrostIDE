// SPDX-License-Identifier: Apache-2.0
//
// FND-9a: opening a CMake project spawns exactly one clangd. The real cmake configures a tiny C
// project and the real (pinned) clangd starts on its compile_commands.json; every process goes
// through a counting IProcessLauncher over the real launcher, which is the spawn log. Covered: a
// trusted folder whose configure-on-open writes the first database (clangd waits for it), the
// reopen of the configured folder (clangd starts at once and the configure's model does not
// restart it), a later profile switch, which moves the database and must restart clangd, and a
// trust grant on an untrusted configured folder, which must restart the restricted clangd
// (--enable-config=false) once in full mode, and its revocation, which restricts it again.

#include "frost/build/cmake_service.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "workbench_rig.hpp"
#include <QApplication>
#include <QPushButton>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
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

bool restricted(const std::vector<std::string>& arguments) {
    return std::ranges::find(arguments, std::string("--enable-config=false")) != arguments.end();
}

// A C project with a Debug and a Release Ninja profile; the compiler is the build's own.
void writeProject(WorkDir& dir) {
    std::string compiler = FROST_TEST_C_COMPILER;
    std::ranges::replace(compiler, '\\', '/');
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\n"
                                "set(CMAKE_C_COMPILER \"" +
                                    compiler +
                                    "\" CACHE FILEPATH \"\" FORCE)\n"
                                    "project(Spawn C)\nadd_executable(hello main.c)\n");
    dir.write("main.c", "int main(void) { return 0; }\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[
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

// Waits until clangd runs on `buildDir`'s database, then lets queued model and state events
// arrive, so a restart they would cause is counted too.
bool clangdSettledOn(WorkbenchRig& rig, const std::string& buildDir) {
    const bool running = waitFor(
        [&] {
            const auto* clangd = rig.workbench->clangd();
            const auto location = clangd ? clangd->compileCommands() : std::nullopt;
            return clangd && clangd->state() == frost::lang::ClangdState::Running && location &&
                   location->directory == buildDir && rig.workbench->cmakeService() &&
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
        EXPECT_GE(launcher.clangdStarts(), 2);
    }
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

} // namespace
