// SPDX-License-Identifier: Apache-2.0
// FER-10 real composition order: destroy Workbench before ShellWindow while the instantiated
// provider has blocked physical work. The filesystem gate is a regression seam, not a real network.
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "engine_unit_tests_window.hpp"
#include "workbench_rig.hpp"
#include <QPointer>
#include <QPushButton>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace frost::app {
namespace {
class GatedFileSystem final : public io::IFileSystem {
public:
    std::string tool;
    std::thread::id gui = std::this_thread::get_id();
    std::atomic<bool> enabled = false, entered = false, returned = false, guiTouched = false;
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false, destroyedBeforeReturn = false;
    Result<std::string> readFile(std::string_view p) override { return io::realFileSystem().readFile(p); }
    Result<void> writeFileAtomic(std::string_view p, std::string_view s) override {
        return io::realFileSystem().writeFileAtomic(p, s);
    }
    Result<io::FileStatus> status(std::string_view p) override { return io::realFileSystem().status(p); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override {
        return io::realFileSystem().listDirectory(p);
    }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override {
        return io::realFileSystem().listDirectoryWithDiagnostics(p);
    }
    Result<void> createDirectories(std::string_view p) override { return io::realFileSystem().createDirectories(p); }
    Result<void> removeFile(std::string_view p) override { return io::realFileSystem().removeFile(p); }
    Result<void> removeDirectory(std::string_view p) override { return io::realFileSystem().removeDirectory(p); }
    Result<bool> exists(std::string_view p) override { return io::realFileSystem().exists(p); }
    Result<std::string> canonicalPath(std::string_view p) override {
        if (enabled.load() && io::normalizePath(p).value_or("") == tool) {
            if (std::this_thread::get_id() == gui)
                guiTouched = true;
            else {
                entered = true;
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::seconds(2), [&] { return released; });
                returned = true;
            }
        }
        return io::realFileSystem().canonicalPath(p);
    }
};
class UnavailableCli final : public io::IProcessLauncher {
public:
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec&) override {
        return fail(ErrorCode::Unsupported, "lifetime fixture provides no engine CLI output");
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "no child needed for provider retirement");
    }
};
TEST(EngineUnitTestsComposition, WorkbenchDestroyedBeforeWindowDeletesProviderBeforeBorrowedServiceDrain) {
    test::WorkDir dir("engine-provider-workbench-lifetime");
    dir.write("project/Game.frostproj", "{}");
#ifdef _WIN32
    dir.write("bin/FrostEditor.exe", "locator input");
    dir.write("bin/FrostEditor.com", "locator CLI input");
    const auto cli = dir.join("bin/FrostEditor.com");
#else
    dir.write("bin/FrostEditor", "locator input");
    const auto cli = dir.join("bin/FrostEditor");
#endif
    GatedFileSystem fs;
    UnavailableCli launcher;
    test::RigOptions options;
    options.disableProjectEvaluation = true;
    options.fileSystem = &fs;
    options.launcher = &launcher;
    options.workbench.frostEditorPath = cli;
    options.workbench.engineConfigDirectory = dir.join("private-profile");
    test::WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("project")));
    ASSERT_TRUE(test::waitFor([&] {
        const auto* runs = rig.workbench->runController();
        return runs && std::ranges::any_of(runs->configurations().all(),
                                           [](const auto& c) { return c.type == run::RunType::FrostEditor; });
    }));
    rig.window->toolWindows().show("frostEngine", false);
    auto* enginePanel = rig.window->toolWindows().content("frostEngine");
    ASSERT_NE(enginePanel, nullptr);
    auto* debugLua = enginePanel->findChild<QPushButton*>("engineDebugLuaButton");
    ASSERT_NE(debugLua, nullptr);
    EXPECT_FALSE(debugLua->isEnabled()) << "Unpaired engine cannot start Lua debugging";
    fs.tool = io::normalizePath(cli).value_or("");
    fs.enabled = true;
    rig.window->toolWindows().show(kUnitTestsToolWindow, false);
    auto* content = rig.window->toolWindows().content(kUnitTestsToolWindow);
    ASSERT_NE(content, nullptr);
    QPointer<QWidget> provider = content->findChild<QWidget*>("engineUnitTestsProvider");
    ASSERT_TRUE(provider);
    QObject::connect(provider.data(), &QObject::destroyed, [&] {
        const std::lock_guard lock(fs.mutex);
        fs.destroyedBeforeReturn = !fs.returned.load();
        fs.released = true;
        fs.changed.notify_all();
    });
    ASSERT_TRUE(test::waitFor([&] { return fs.entered.load(); }));
    EXPECT_FALSE(fs.guiTouched.load());
    rig.workbench.reset(); // The window is intentionally still alive, unlike WorkbenchRig's usual order.
    EXPECT_TRUE(provider.isNull());
    EXPECT_TRUE(fs.destroyedBeforeReturn) << "Provider must retire before Workbench drains borrowed services";
    EXPECT_TRUE(fs.returned.load());
    ASSERT_TRUE(rig.window);
    rig.window->toolWindows().show(kUnitTestsToolWindow, false);
    QApplication::processEvents();
    rig.window.reset();
}
} // namespace
} // namespace frost::app
