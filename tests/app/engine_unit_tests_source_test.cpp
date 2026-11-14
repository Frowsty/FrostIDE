// SPDX-License-Identifier: Apache-2.0
// FER-10 source admission and actual Workbench provider composition over explicitly scripted CLI replies.
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "debug/debug_test_util.hpp"
#include "engine_unit_tests_source.hpp"
#include "workbench_rig.hpp"
#include "support/directory_link.hpp"
#include <filesystem>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stop_token>
#include <thread>

namespace frost::app {
namespace {
using debug::test::workPath;

class EngineUnitTestsSourceTest : public testing::Test {
protected:
    void SetUp() override {
        root = workPath("EngineTests");
        ASSERT_TRUE(fs.createDirectories(root + "/tools"));
        ASSERT_TRUE(fs.writeFileAtomic(root + "/Game.frostproj", "{}"));
        ASSERT_TRUE(fs.writeFileAtomic(root + "/tools/FrostEditor-Cli", "actual locator input"));
        auto opened = project::TrustStore::open(fs, root + "/trust.txt");
        ASSERT_TRUE(opened);
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(root));
        auto physical = fs.canonicalPath(root);
        ASSERT_TRUE(physical);
        input = {root,
                 *physical,
                 lang::documentKey(*physical),
                 fs.canonicalPath(root + "/Game.frostproj").value_or(""),
                 fs.canonicalPath(root + "/tools/FrostEditor-Cli").value_or(""),
                 trust->snapshot(root).revision,
                 std::make_shared<std::atomic<std::uint64_t>>(1),
                 std::make_shared<std::atomic<bool>>(true),
                 stop.get_token(),
                 fs.status(root + "/tools/FrostEditor-Cli").value(),
                 fs.physicalPath(root + "/tools/FrostEditor-Cli").value()};
    }
    Result<EngineUnitTestsSource> capture() { return captureEngineTestsSource(fs, *trust, input); }
    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    std::string root;
    std::stop_source stop;
    EngineUnitTestsCapture input;
};

TEST_F(EngineUnitTestsSourceTest, ImmutableCanonicalSnapshotAdmitsWorkerWithoutAnyQObject) {
    auto source = capture();
    ASSERT_TRUE(source);
    EXPECT_EQ(source->options.program, fs.canonicalPath(input.program).value_or(""));
    EXPECT_EQ(source->options.project, fs.canonicalPath(input.project).value_or(""));
    EXPECT_EQ(source->options.expectedProjectRoot, input.physicalRoot);
    EXPECT_EQ(source->options.expectedProjectFile, fs.canonicalPath(input.project).value_or(""));
    bool admitted = false;
    std::thread worker([&] { admitted = source->admitted() && source->validatePhysical().has_value(); });
    worker.join();
    EXPECT_TRUE(admitted);
    input.sourceRevision->fetch_add(1);
    EXPECT_FALSE(source->admitted());
    auto replacement = capture();
    ASSERT_TRUE(replacement);
    EXPECT_NE(replacement->revision, source->revision);
    EXPECT_TRUE(replacement->admitted());
}

TEST_F(EngineUnitTestsSourceTest, TrustRevocationAndRegrantNeverReviveOldSource) {
    auto source = capture();
    ASSERT_TRUE(source);
    ASSERT_TRUE(trust->untrust(root));
    EXPECT_FALSE(source->admitted());
    ASSERT_TRUE(trust->trust(root));
    EXPECT_FALSE(source->admitted());
    EXPECT_FALSE(capture()); // Owner must recapture the actual trust revision.
    input.trustRevision = trust->snapshot(root).revision;
    EXPECT_TRUE(capture());
}

TEST_F(EngineUnitTestsSourceTest, GuiCaptureAndLogicalAdmissionNeverReadFilesystemMetadata) {
    fs.injectFault(io::FsOp::Status, "", ErrorCode::IoError);
    auto source = capture();
    ASSERT_TRUE(source);
    EXPECT_TRUE(source->admitted());
    EXPECT_EQ(fs.pendingFaults(), 1U);
    EXPECT_FALSE(source->validatePhysical());
    EXPECT_EQ(fs.pendingFaults(), 0U);
}

TEST_F(EngineUnitTestsSourceTest, ToolReplacementAndProjectLossRevokeDispatchAndPublication) {
    auto source = capture();
    ASSERT_TRUE(source);
    ASSERT_TRUE(fs.writeFileAtomic(input.program, "different installed CLI bytes"));
    EXPECT_TRUE(source->admitted()); // Logical GUI checks perform no metadata I/O.
    EXPECT_FALSE(source->validatePhysical());
    EXPECT_FALSE(source->admitted());
    EXPECT_GT(input.sourceRevision->load(), source->revision);
    input.expectedProgramStatus = fs.status(input.program).value();
    auto current = capture();
    ASSERT_TRUE(current);
    ASSERT_TRUE(fs.rename(root, root + "-moved"));
    EXPECT_FALSE(current->validatePhysical());
    EXPECT_FALSE(current->admitted());
    EXPECT_FALSE(input.physicalAdmission->load());
    ASSERT_TRUE(fs.rename(root + "-moved", root));
    EXPECT_FALSE(current->admitted()); // Restoring a spelling does not restore the revoked physical proof.
    stop.request_stop();
    EXPECT_FALSE(capture());
}

TEST_F(EngineUnitTestsSourceTest, MissingToolAndForeignProjectAreErrorsWithoutFallback) {
    input.program = root + "/missing-cli";
    auto absent = capture();
    ASSERT_TRUE(absent); // Physical availability is checked by a worker, never the GUI factory.
    EXPECT_FALSE(absent->validatePhysical());
    input.program = fs.canonicalPath(root + "/tools/FrostEditor-Cli").value_or("");
    const auto other = workPath("OtherEngineTests");
    ASSERT_TRUE(fs.createDirectories(other));
    ASSERT_TRUE(fs.writeFileAtomic(other + "/Other.frostproj", "{}"));
    input.project = other + "/Other.frostproj";
    auto foreign = capture();
    ASSERT_TRUE(foreign); // GUI capture performs no physical classification.
    EXPECT_EQ(foreign->validatePhysical().error().code(), ErrorCode::PermissionDenied);
}

class ComposedEngineTestsLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher tests;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (!spec.arguments.empty() && (spec.arguments.front() == "test.list" || spec.arguments.front() == "test.run"))
            return tests.launch(spec);
        return fail(ErrorCode::Unsupported,
                    "This provider-composition fixture supplies only actual test command replies");
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "The composition fixture does not launch background language tools");
    }
};

TEST(EngineUnitTestsComposition, CTestRemainsFirstAndEngineSourceOptionsMustMatchBeforeDebug) {
    test::WorkDir dir("engine-tests-composition");
    dir.write("Game.frostproj", "{}");
#ifdef _WIN32
    const auto cli = dir.join("tools/FrostEditor-Cli.exe");
    dir.write("tools/FrostEditor-Cli.exe", "locator fixture, never executed");
#else
    const auto cli = dir.join("tools/FrostEditor");
    dir.write("tools/FrostEditor", "locator fixture, never executed");
#endif
    const auto physical = io::realFileSystem().canonicalPath(dir.path());
    ASSERT_TRUE(physical);
    const auto projectFile = io::realFileSystem().canonicalPath(dir.join("Game.frostproj"));
    ASSERT_TRUE(projectFile);
    auto listing =
        rpc::Json::object()
            .set("version", 1)
            .set("granularity", "scene")
            .set("projectPath", *physical)
            .set("projectFile", *projectFile)
            .set("count", 1)
            .set("tests",
                 rpc::Json::array(
                     {rpc::Json::object().set("id", "Assets/Tests/A.fscene").set("scene", "Assets/Tests/A.fscene")}));
    ComposedEngineTestsLauncher launcher;
    io::ScriptedProcess reply;
    reply.stdoutBytes = rpc::Json::object().set("command", "test.list").set("ok", true).set("result", listing).dump();
    launcher.tests.expect(std::move(reply));
    test::RigOptions options;
    options.disableProjectEvaluation = true;
    options.launcher = &launcher;
    options.workbench.frostEditorPath = cli;
    options.workbench.engineAutomationDirectory = dir.join("automation");
    test::WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    ASSERT_TRUE(test::waitFor([&] { return rig.workbench->engineState().boolAt("discoveryBusy") == false; }));
    rig.window->toolWindows().show(kUnitTestsToolWindow, false);
    auto* content = rig.window->toolWindows().content(kUnitTestsToolWindow);
    ASSERT_NE(content, nullptr);
    auto* tabs = qobject_cast<QTabWidget*>(content);
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 2);
    EXPECT_EQ(tabs->currentIndex(), 0);
    EXPECT_EQ(tabs->widget(0)->objectName(), "unitTestsToolWindow");
    EXPECT_EQ(tabs->widget(1)->objectName(), "engineUnitTestsProvider");
    auto* tree = tabs->widget(1)->findChild<QTreeWidget*>("engineTestsTree");
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(test::waitFor([&] { return tree->topLevelItemCount() == 1; }));
    EXPECT_EQ(tree->topLevelItem(0)->text(0), "Assets/Tests/A.fscene");
    engine::EngineTestProviderOptions wrong;
    wrong.program = io::realFileSystem().canonicalPath(cli).value_or("");
    wrong.project = *physical;
    wrong.expectedProjectRoot = *physical;
    wrong.program += ".another-tool";
    auto refused = rig.workbench->debugEngineTestScene(wrong, {"Assets/Tests/A.fscene", "Assets/Tests/A.fscene"});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
    ASSERT_EQ(launcher.tests.launches().size(), 1U);
    auto* debug = tabs->widget(1)->findChild<QPushButton*>("engineTestsDebug");
    ASSERT_NE(debug, nullptr);
    tree->setCurrentItem(tree->topLevelItem(0));
    ASSERT_TRUE(rig.trust->untrust(dir.path()));
    debug->click();
    EXPECT_EQ(launcher.tests.launches().size(), 1U);
    EXPECT_TRUE(test::waitFor([&] { return tree->topLevelItemCount() == 0; }));
}

TEST(EngineUnitTestsPhysicalSource, DiscoveryCliAliasRetargetBeforeFirstWorkerRefusesWithoutLaunch) {
    test::WorkDir dir("engine-tests-cli-alias");
    dir.write("project/Game.frostproj", "{}");
    dir.write("first/FrostEditor-Cli", "same bytes");
    dir.write("second/FrostEditor-Cli", "same bytes");
    auto& fs = io::realFileSystem();
    std::error_code timeError;
    const auto firstTime = std::filesystem::last_write_time(io::toStdPath(dir.join("first/FrostEditor-Cli")).value(), timeError);
    ASSERT_FALSE(timeError) << timeError.message();
    std::filesystem::last_write_time(io::toStdPath(dir.join("second/FrostEditor-Cli")).value(), firstTime, timeError);
    ASSERT_FALSE(timeError) << timeError.message();
    ASSERT_EQ(fs.status(dir.join("first/FrostEditor-Cli")).value(), fs.status(dir.join("second/FrostEditor-Cli")).value());
    const auto alias = dir.join("tools");
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("first")));
    auto trusted = project::TrustStore::open(fs, dir.join("trust.txt"));
    ASSERT_TRUE(trusted);
    const auto root = dir.join("project");
    ASSERT_TRUE((*trusted)->trust(root));
    EngineUnitTestsCapture input{root, fs.physicalPath(root).value(), lang::documentKey(fs.physicalPath(root).value()),
                                dir.join("project/Game.frostproj"), io::joinPath(alias, "FrostEditor-Cli").value(),
                                (*trusted)->snapshot(root).revision,
                                std::make_shared<std::atomic<std::uint64_t>>(1),
                                std::make_shared<std::atomic<bool>>(true), {},
                                fs.status(dir.join("first/FrostEditor-Cli")).value(),
                                fs.physicalPath(dir.join("first/FrostEditor-Cli")).value()};
    auto source = captureEngineTestsSource(fs, **trusted, input);
    ASSERT_TRUE(source);
    ASSERT_TRUE(fs.removeFile(alias));
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("second")));
    io::ScriptedProcessLauncher launcher;
    const auto validation = source->validatePhysical();
    EXPECT_FALSE(validation);
    if (validation)
        (void)engine::discoverEngineSceneTests(fs, launcher, source->options, source->admitted);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_FALSE(source->admitted());
    ASSERT_TRUE(fs.removeFile(alias));
    ASSERT_NO_FATAL_FAILURE(frost::test::makeDirectoryLink(alias, dir.join("first")));
    EXPECT_FALSE(source->validatePhysical());
    EXPECT_FALSE(source->admitted());
}

#ifdef _WIN32
DWORD engineTestsShortName(const std::filesystem::path& path, const wchar_t* name) {
    const HANDLE file = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();
    const BOOL changed = SetFileShortNameW(file, name);
    const DWORD result = changed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    return result;
}
#endif

TEST(EngineUnitTestsPhysicalSource, SameParentSelectedFileAliasLatchesOnFirstWorkerAndNeverRevivesAfterRetarget) {
    test::WorkDir dir("engine-tests-selected-file-alias");
    dir.write("project/FirstProject.frostproj", "{}");
    dir.write("project/SecondProject.frostproj", "{}");
    dir.write("FrostEditor-Cli", "validator input");
    auto& fs = io::realFileSystem();
    const auto root = fs.physicalPath(dir.join("project")).value();
    const auto first = dir.join("project/FirstProject.frostproj");
    const auto second = dir.join("project/SecondProject.frostproj");
#ifdef _WIN32
    const auto alias = dir.join("project/SELECTED.FRO");
    ASSERT_EQ(engineTestsShortName(io::toStdPath(first).value(), L"SELECTED.FRO"), ERROR_SUCCESS);
#else
    const auto alias = dir.join("project/Selected.frostproj");
    std::error_code error;
    std::filesystem::create_symlink(io::toStdPath(first).value(), io::toStdPath(alias).value(), error);
    ASSERT_FALSE(error) << error.message();
#endif
    auto trusted = project::TrustStore::open(fs, dir.join("trust.txt"));
    ASSERT_TRUE(trusted);
    ASSERT_TRUE((*trusted)->trust(root));
    EngineUnitTestsCapture input{root, root, lang::documentKey(root), alias, dir.join("FrostEditor-Cli"),
                                (*trusted)->snapshot(root).revision,
                                std::make_shared<std::atomic<std::uint64_t>>(1),
                                std::make_shared<std::atomic<bool>>(true), {}, fs.status(dir.join("FrostEditor-Cli")).value(),
                                fs.physicalPath(dir.join("FrostEditor-Cli")).value()};
    auto source = captureEngineTestsSource(fs, **trusted, input);
    ASSERT_TRUE(source);
    const auto physicalFirst = fs.physicalPath(alias);
    ASSERT_TRUE(physicalFirst);
    EXPECT_EQ(lang::documentKey(*physicalFirst), lang::documentKey(fs.physicalPath(first).value()));
    ASSERT_TRUE(source->validatePhysical()); // The initial WORKER admission boundary, not GUI project open.
#ifdef _WIN32
    ASSERT_EQ(engineTestsShortName(io::toStdPath(first).value(), L""), ERROR_SUCCESS);
    ASSERT_EQ(engineTestsShortName(io::toStdPath(second).value(), L"SELECTED.FRO"), ERROR_SUCCESS);
#else
    ASSERT_TRUE(fs.removeFile(alias));
    std::filesystem::create_symlink(io::toStdPath(second).value(), io::toStdPath(alias).value(), error);
    ASSERT_FALSE(error) << error.message();
#endif
    const auto physicalSecond = fs.physicalPath(alias);
    ASSERT_TRUE(physicalSecond);
    EXPECT_NE(lang::documentKey(*physicalFirst), lang::documentKey(*physicalSecond));
    EXPECT_EQ(lang::documentKey(io::parentPath(*physicalFirst)), lang::documentKey(io::parentPath(*physicalSecond)));
    EXPECT_TRUE(fs.exists(first).value()); // Original file remains: a root-only guard would incorrectly pass.
    EXPECT_FALSE(source->validatePhysical());
    EXPECT_FALSE(source->admitted());
#ifdef _WIN32
    ASSERT_EQ(engineTestsShortName(io::toStdPath(second).value(), L""), ERROR_SUCCESS);
    ASSERT_EQ(engineTestsShortName(io::toStdPath(first).value(), L"SELECTED.FRO"), ERROR_SUCCESS);
#else
    ASSERT_TRUE(fs.removeFile(alias));
    std::filesystem::create_symlink(io::toStdPath(first).value(), io::toStdPath(alias).value(), error);
    ASSERT_FALSE(error) << error.message();
#endif
    EXPECT_FALSE(source->validatePhysical());
    EXPECT_FALSE(source->admitted());
}

} // namespace
} // namespace frost::app
