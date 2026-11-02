// SPDX-License-Identifier: Apache-2.0
// Real secondary frost process forwards a FrostEngine open request to a project-scoped window.
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/json.hpp"

#include "../../src/app/src/project_open_broker.hpp"
#include "workbench_rig.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace {
using namespace std::chrono_literals;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
TEST(ProjectOpenProcess, SymlinkedProjectUsesTargetDirectoryAsIdentity) {
    WorkDir dir("engine-project-symlink");
    dir.write("target/Game.frostproj", "{}");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("alias")));
    auto source = frost::io::toStdPath(dir.join("target/Game.frostproj"));
    auto link = frost::io::toStdPath(dir.join("alias/Game.frostproj"));
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(link.has_value());
    std::error_code error;
    std::filesystem::create_symlink(*source, *link, error);
    if (error)
        GTEST_SKIP() << "symbolic links unavailable: " << error.message();
    auto direct = frost::app::externalProjectRoot(dir.join("target/Game.frostproj"));
    auto throughLink = frost::app::externalProjectRoot(dir.join("alias/Game.frostproj"));
    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(throughLink.has_value());
    EXPECT_EQ(frost::app::externalProjectKey(*direct), frost::app::externalProjectKey(*throughLink));
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openExternalProject(dir.join("alias/Game.frostproj")));
    EXPECT_EQ(frost::app::externalProjectKey(rig.workbench->projectRoot()), frost::app::externalProjectKey(*direct));
}

TEST(ProjectOpenProcess, OrdinaryAliasProjectReusesDirtyAliasTabForRealProjectRequest) {
    WorkDir dir("engine-ordinary-alias");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/script.lua", "local first = 1\nlocal second = 2\n");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("profile")));
    const auto target = frost::io::toStdPath(dir.join("project"));
    const auto alias = frost::io::toStdPath(dir.join("alias"));
    ASSERT_TRUE(target && alias);
    std::error_code error;
    std::filesystem::create_directory_symlink(*target, *alias, error);
    if (error)
        GTEST_SKIP() << "directory symbolic links unavailable: " << error.message();
    WorkbenchRig rig(dir);
    frost::app::ProjectOpenBroker broker(*rig.workbench, *rig.window, {}, {});
    ASSERT_TRUE(broker.start());
    ASSERT_TRUE(rig.workbench->openPath(dir.join("alias")));
    broker.projectChanged();
    ASSERT_TRUE(rig.workbench->openPath(dir.join("alias/script.lua")));
    auto* original = rig.workbench->currentView();
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->editor().typeText("-- unsaved\n"));
    ASSERT_TRUE(original->document().dirty());

    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(FROST_EXE_PATH).value();
    spec.arguments = {"--project", dir.join("project"), dir.join("project/script.lua") + ":2:3"};
    spec.environment = {{"QT_QPA_PLATFORM", "offscreen"},
                        {"APPDATA", dir.join("profile")},
                        {"LOCALAPPDATA", dir.join("profile")},
                        {"XDG_CONFIG_HOME", dir.join("profile")},
                        {"XDG_DATA_HOME", dir.join("profile")}};
    std::atomic<bool> done{false};
    frost::Result<frost::io::ProcessResult> result = frost::fail(frost::ErrorCode::Unknown, "not launched");
    std::jthread client([&] {
        result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 15s);
        done.store(true, std::memory_order_release);
    });
    const bool finished = waitFor([&] { return done.load(std::memory_order_acquire); }, 20s);
    client.join();
    ASSERT_TRUE(finished);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(rig.workbench->currentView(), original);
    EXPECT_EQ(original->status().line, 2);
    EXPECT_EQ(original->status().column, 3);
    EXPECT_TRUE(original->document().dirty());
    EXPECT_EQ(rig.window->tabs().count(), 1);
}

TEST(ProjectOpenProcess, SymlinkedScriptUsesDirtyTargetAndCannotAliasProject) {
    WorkDir dir("engine-script-symlink");
    dir.write("target/Game.frostproj", "{}");
    dir.write("target/script.lua", "local a = 1\nlocal b = 2\n");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("aliases")));
    const auto target = frost::io::toStdPath(dir.join("target/script.lua"));
    const auto project = frost::io::toStdPath(dir.join("target/Game.frostproj"));
    const auto scriptLink = frost::io::toStdPath(dir.join("aliases/script.lua"));
    const auto projectLink = frost::io::toStdPath(dir.join("aliases/project.lua"));
    ASSERT_TRUE(target && project && scriptLink && projectLink);
    std::error_code error;
    std::filesystem::create_symlink(*target, *scriptLink, error);
    if (error)
        GTEST_SKIP() << "symbolic links unavailable: " << error.message();
    std::filesystem::create_symlink(*project, *projectLink, error);
    ASSERT_FALSE(error) << error.message();
    const std::string root = dir.join("target");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("target/script.lua")));
    auto* original = rig.workbench->currentView();
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->editor().typeText("-- unsaved\n"));
    ASSERT_TRUE(original->document().dirty());
    auto canonical = frost::app::externalProjectRoot(root);
    ASSERT_TRUE(canonical.has_value());
    auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(*canonical));
    ASSERT_TRUE(owner.has_value());
    frost::app::ProjectOpenBroker broker(*rig.workbench, *rig.window, *canonical, std::move(*owner));
    ASSERT_TRUE(broker.start());
    const auto forward = [&](std::string file, int line, int column) {
        std::atomic<bool> done{false};
        frost::Result<void> result = frost::fail(frost::ErrorCode::Unknown, "not sent");
        std::jthread client([&] {
            result = frost::app::forwardExternalOpen(*canonical, file, line, column);
            done.store(true, std::memory_order_release);
        });
        const bool finished = waitFor([&] { return done.load(std::memory_order_acquire); }, 8s);
        client.join();
        EXPECT_TRUE(finished);
        return result;
    };
    auto opened = forward(dir.join("aliases/script.lua"), 2, 3);
    ASSERT_TRUE(opened.has_value()) << opened.error().describe();
    EXPECT_EQ(rig.workbench->currentView(), original);
    EXPECT_EQ(original->status().line, 2);
    EXPECT_EQ(original->status().column, 3);
    EXPECT_TRUE(original->document().dirty());
    EXPECT_EQ(rig.window->tabs().count(), 1);
    auto refused = forward(dir.join("aliases/project.lua"), 1, 1);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(rig.workbench->currentView(), original);
    EXPECT_EQ(rig.workbench->projectRoot(), root);
    EXPECT_TRUE(original->document().dirty());
    EXPECT_EQ(rig.window->tabs().count(), 1);
}

TEST(ProjectOpenProcess, ForwardsUnicodePositionWithoutClosingDirtyTab) {
    WorkDir dir("engine-open");
    dir.write("\xC3\x85 project/Game.frostproj", "{}");
    dir.write("\xC3\x85 project/Unrelated.sln", "not a solution");
    dir.write("\xC3\x85 project/Assets/my script.lua", "local first = 1\nlocal second = 2\n");
    dir.write("\xC3\x85 project/dirty.txt", "original\n");
    const std::string root = dir.join("\xC3\x85 project");
    const std::string script = dir.join("\xC3\x85 project/Assets/my script.lua");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    ASSERT_EQ(rig.workbench->solution(), nullptr);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("\xC3\x85 project/dirty.txt")));
    auto* dirty = rig.workbench->currentView();
    ASSERT_NE(dirty, nullptr);
    ASSERT_TRUE(dirty->editor().typeText("unsaved "));
    ASSERT_TRUE(dirty->document().dirty());
    auto canonical = frost::app::externalProjectRoot(root);
    ASSERT_TRUE(canonical.has_value());
    auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(*canonical));
    ASSERT_TRUE(owner.has_value());
    frost::app::ProjectOpenBroker broker(*rig.workbench, *rig.window, *canonical, std::move(*owner));
    ASSERT_TRUE(broker.start());

    frost::io::ProcessSpec spec;
    spec.executable = frost::io::normalizePath(FROST_EXE_PATH).value();
    spec.arguments = {"--project", root, script + ":2:7"};
    spec.environment = {{"QT_QPA_PLATFORM", "offscreen"},
                        {"APPDATA", dir.join("profile")},
                        {"LOCALAPPDATA", dir.join("profile")},
                        {"XDG_CONFIG_HOME", dir.join("profile")},
                        {"XDG_DATA_HOME", dir.join("profile")}};
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("profile")));
    std::atomic<bool> done{false};
    frost::Result<frost::io::ProcessResult> result = frost::fail(frost::ErrorCode::Unknown, "not launched");
    std::jthread client([&] {
        result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 15s);
        done.store(true, std::memory_order_release);
    });
    const bool finished = waitFor([&] { return done.load(std::memory_order_acquire); }, 20s);
    client.join();
    ASSERT_TRUE(finished);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << result->stderrBytes;
    ASSERT_NE(rig.workbench->currentView(), nullptr);
    EXPECT_EQ(rig.workbench->currentView()->path(), script);
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_EQ(rig.workbench->currentView()->status().column, 7);
    ASSERT_TRUE(frost::app::openExternalFile(*rig.workbench, script, 0, 0));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_EQ(rig.workbench->currentView()->status().column, 7);
    ASSERT_TRUE(frost::app::openExternalFile(*rig.workbench, script, 0, 3));
    EXPECT_EQ(rig.workbench->currentView()->status().line, 2);
    EXPECT_EQ(rig.workbench->currentView()->status().column, 3);
    EXPECT_TRUE(dirty->document().dirty());
    EXPECT_EQ(rig.window->tabs().count(), 2);
    EXPECT_EQ(rig.workbench->solution(), nullptr);

    // A client that reaches this endpoint but claims another root cannot open or switch anything.
    const std::string wrong =
        frost::lang::Json::object().set("root", dir.path()).set("file", script).set("line", 1).set("column", 1).dump();
    std::atomic<bool> refusedDone{false};
    frost::Result<std::string> refused = frost::fail(frost::ErrorCode::Unknown, "not sent");
    std::jthread adversary([&] {
        refused = frost::io::requestLocalIpc(frost::app::externalProjectKey(*canonical), wrong, 5s);
        refusedDone.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(waitFor([&] { return refusedDone.load(std::memory_order_acquire); }, 8s));
    adversary.join();
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(rig.workbench->currentView()->path(), script);
    EXPECT_TRUE(dirty->document().dirty());
}
TEST(ProjectOpenProcess, TwoRealProcessesReuseProjectAndReleaseOnCrash) {
    WorkDir dir("engine-two-process");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/Assets/script.lua", "local value = 1\n");
    const std::string root = dir.join("project");
    const std::string script = dir.join("project/Assets/script.lua");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("profile")));
    auto canonical = frost::app::externalProjectRoot(root);
    ASSERT_TRUE(canonical.has_value());
    frost::io::ProcessSpec first;
    first.executable = frost::io::normalizePath(FROST_EXE_PATH).value();
    first.arguments = {"--project", root};
    first.stdoutMode = frost::io::Stdio::Null;
    first.stderrMode = frost::io::Stdio::Null;
    first.environment = {{"QT_QPA_PLATFORM", "offscreen"},
                         {"APPDATA", dir.join("profile")},
                         {"LOCALAPPDATA", dir.join("profile")},
                         {"XDG_CONFIG_HOME", dir.join("profile")},
                         {"XDG_DATA_HOME", dir.join("profile")}};
    auto primary = frost::io::realProcessLauncher().launch(first);
    ASSERT_TRUE(primary.has_value());
    const auto payload =
        frost::lang::Json::object().set("root", *canonical).set("file", "").set("line", 0).set("column", 0).dump();
    const std::string id = frost::app::externalProjectKey(*canonical);
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    bool ready = false;
    while (!ready && std::chrono::steady_clock::now() < deadline) {
        ready = frost::io::requestLocalIpc(id, payload, 500ms).has_value();
    }
    ASSERT_TRUE(ready) << "first frost process did not publish its endpoint";
    frost::io::ProcessSpec second = first;
    second.arguments = {"--project", root, script + ":1:4"};
    second.stdoutMode = frost::io::Stdio::Pipe;
    second.stderrMode = frost::io::Stdio::Pipe;
    auto forwarded = frost::io::runToCompletion(frost::io::realProcessLauncher(), second, {}, 12s);
    ASSERT_TRUE(forwarded.has_value()) << forwarded.error().describe();
    EXPECT_EQ(forwarded->exitCode, 0) << forwarded->stderrBytes;
    ASSERT_TRUE((*primary)->terminate());
    ASSERT_TRUE((*primary)->wait(5s));
    auto recovered = frost::io::LocalIpcOwner::claim(id);
    ASSERT_TRUE(recovered.has_value()) << recovered.error().describe();
}

TEST(ProjectOpenProcess, OrdinaryAliasStartupClaimsRealProjectEndpoint) {
    WorkDir dir("engine-alias-startup");
    dir.write("project/Game.frostproj", "{}");
    dir.write("project/script.lua", "return {}\n");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.join("profile")));
    auto target = frost::io::toStdPath(dir.join("project"));
    auto alias = frost::io::toStdPath(dir.join("alias"));
    ASSERT_TRUE(target && alias);
    std::error_code error;
    std::filesystem::create_directory_symlink(*target, *alias, error);
    if (error)
        GTEST_SKIP() << "directory symbolic links unavailable: " << error.message();
    auto canonical = frost::app::externalProjectRoot(dir.join("project"));
    ASSERT_TRUE(canonical);
    frost::io::ProcessSpec first;
    first.executable = frost::io::normalizePath(FROST_EXE_PATH).value();
    first.arguments = {dir.join("alias")};
    first.stdoutMode = frost::io::Stdio::Null;
    first.stderrMode = frost::io::Stdio::Null;
    first.environment = {{"QT_QPA_PLATFORM", "offscreen"},
                         {"APPDATA", dir.join("profile")},
                         {"LOCALAPPDATA", dir.join("profile")},
                         {"XDG_CONFIG_HOME", dir.join("profile")},
                         {"XDG_DATA_HOME", dir.join("profile")}};
    auto primary = frost::io::realProcessLauncher().launch(first);
    ASSERT_TRUE(primary);
    const auto payload =
        frost::lang::Json::object().set("root", *canonical).set("file", "").set("line", 0).set("column", 0).dump();
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    bool ready = false;
    while (!ready && std::chrono::steady_clock::now() < deadline)
        ready = frost::io::requestLocalIpc(frost::app::externalProjectKey(*canonical), payload, 500ms).has_value();
    EXPECT_TRUE(ready) << "ordinary alias project did not publish the canonical endpoint";
    ASSERT_TRUE((*primary)->terminate());
    ASSERT_TRUE((*primary)->wait(5s));
}

TEST(ProjectOpenProcess, Utf16ColumnsUseDirtyBufferAndClampSurrogates) {
    WorkDir dir("utf16-external");
    dir.write("project/Assets/Example.cs", "\xC3\xA9"
                                           "\xF0\x9F\x98\x80"
                                           "target\n");
    const std::string root = dir.join("project");
    const std::string path = dir.join("project/Assets/Example.cs");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    ASSERT_TRUE(rig.workbench->openPath(path));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("x"));
    const auto byteColumn = [&] {
        const auto& editor = view->editor().editor();
        return editor.text().pointAt(editor.state().primary().head).column.value();
    };
    ASSERT_TRUE(frost::app::openExternalFile(*rig.workbench, path, 1, 5, true));
    EXPECT_EQ(byteColumn(), 7u); // x + ? + the complete astral character.
    ASSERT_TRUE(frost::app::openExternalFile(*rig.workbench, path, 1, 4, true));
    EXPECT_EQ(byteColumn(), 3u); // An interior UTF-16 surrogate floors to the character start.
    ASSERT_TRUE(frost::app::openExternalFile(*rig.workbench, path, 1, 999, true));
    EXPECT_EQ(byteColumn(), 13u); // Beyond the line clamps to its end.
    EXPECT_TRUE(view->document().dirty());
}

TEST(ProjectOpenProcess, Utf16RequestIsRejectedByLegacyOwner) {
    WorkDir dir("utf16-old-owner");
    auto root = frost::app::externalProjectRoot(dir.path());
    ASSERT_TRUE(root);
    auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(*root));
    ASSERT_TRUE(owner);
    std::atomic<int> requests{0};
    ASSERT_TRUE((*owner)->start([&](std::string request) -> frost::Result<std::string> {
        ++requests;
        auto payload = frost::lang::parseJson(request);
        if (!payload || payload->integerAt("line").value_or(-1) < 0)
            return frost::fail(frost::ErrorCode::InvalidArgument, "legacy line rejected");
        return std::string("unexpected legacy open");
    }));
    EXPECT_FALSE(frost::app::forwardExternalOpen(*root, dir.join("Example.cs"), 3, 40, true));
    EXPECT_EQ(requests.load(), 1);
}

TEST(ProjectOpenProcess, UnityEditorBridgeReusesDirtyScriptAtLocation) {
    const std::string unity = qgetenv("FROST_TEST_UNITY_EDITOR").toStdString();
    const std::string root = qgetenv("FROST_TEST_UNITY_PROJECT").toStdString();
    const std::string profile = qgetenv("FROST_TEST_UNITY_PROFILE").toStdString();
    if (unity.empty() || root.empty() || profile.empty())
        GTEST_SKIP() << "set isolated Unity scratch project and editor paths";
    WorkDir dir("unity-real-bridge");
    const std::string script = frost::io::joinPath(root, "Assets/Probe/FrostUnityProbe.cs").value_or("");
    auto original = frost::io::realFileSystem().readFile(script);
    ASSERT_TRUE(original);
    WorkbenchRig rig(dir);
    auto canonical = frost::app::externalProjectRoot(root);
    ASSERT_TRUE(canonical);
    auto owner = frost::io::LocalIpcOwner::claim(frost::app::externalProjectKey(*canonical));
    ASSERT_TRUE(owner);
    frost::app::ProjectOpenBroker broker(*rig.workbench, *rig.window, *canonical, std::move(*owner));
    ASSERT_TRUE(broker.start());
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    ASSERT_TRUE(rig.workbench->openPath(script));
    auto* view = rig.workbench->currentView();
    ASSERT_NE(view, nullptr);
    view->editor().goToLine(3, 1);
    ASSERT_TRUE(view->editor().typeText("/*\xC3\xA9*/"));
    ASSERT_TRUE(view->document().dirty());
    const std::string sentinel = dir.join("unity-open-sentinel.txt");
    frost::io::ProcessSpec spec;
    spec.executable = unity;
    spec.arguments = {"-batchmode",
                      "-nographics",
                      "-quit",
                      "-job-worker-count",
                      "2",
                      "-projectPath",
                      root,
                      "-executeMethod",
                      "FrostBridgeScratchProbe.OpenIDE",
                      "-logFile",
                      dir.join("unity.log")};
    spec.environment = {{"APPDATA", profile},
                        {"LOCALAPPDATA", profile},
                        {"FROST_UNITY_CAPTURE_EXE", frost::io::normalizePath(FROST_EXE_PATH).value()},
                        {"FROST_UNITY_OPEN_SENTINEL", sentinel},
                        {"QT_QPA_PLATFORM", "offscreen"}};
    std::atomic<bool> done{false};
    frost::Result<frost::io::ProcessResult> result = frost::fail(frost::ErrorCode::Unknown, "not launched");
    std::jthread client([&] {
        result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 90s);
        done.store(true, std::memory_order_release);
    });
    const bool finished = waitFor([&] { return done.load(std::memory_order_acquire); }, 100s);
    client.join();
    ASSERT_TRUE(finished);
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    ASSERT_TRUE(frost::io::realFileSystem().exists(sentinel).value_or(false));
    ASSERT_TRUE(waitFor(
        [&] {
            const auto& editor = view->editor().editor();
            const auto point = editor.text().pointAt(editor.state().primary().head);
            return point.line.value() == 2 && point.column.value() == 42;
        },
        10s));
    EXPECT_EQ(rig.workbench->currentView(), view);
    EXPECT_EQ(rig.window->tabs().count(), 1);
    EXPECT_TRUE(view->document().dirty());
    auto after = frost::io::realFileSystem().readFile(script);
    ASSERT_TRUE(after);
    EXPECT_EQ(*after, *original);
}
} // namespace
