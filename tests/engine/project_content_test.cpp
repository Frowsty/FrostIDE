// SPDX-License-Identifier: Apache-2.0
//
// FEC-3 validation and trust boundaries. Real CLI comparison belongs to the corpus acceptance leg.
#include "frost/engine/project_content.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <string>

namespace frost::engine {
namespace {

using rpc::Json;
constexpr EngineHandle kScene{16273909378015378282ULL};

Json settings() {
    return Json::object()
        .set("Version", 1)
        .set("General", Json::object().set("Name", "Game").set("StartScene", toJson(kScene)));
}

Json listing() {
    return Json::object()
        .set("count", 2)
        .set("assets",
             Json::array(
                 {Json::object()
                      .set("handle", toJson(kScene))
                      .set("path", "Assets/Scenes/Main.fscene")
                      .set("type", "Scene"),
                  Json::object().set("handle", 42).set("path", "Assets/Scripts/Player.lua").set("type", "Script")}));
}

class ProjectContentTest : public testing::Test {
protected:
    test::ScratchDir root{"project-content"};
    io::IFileSystem& fs = io::realFileSystem();
    io::ScriptedProcessLauncher launcher;

    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(root.join("Assets/Scenes")));
        ASSERT_TRUE(fs.createDirectories(root.join("Assets/Scripts")));
        ASSERT_TRUE(fs.writeFileAtomic(root.join("Assets/Scenes/Main.fscene"), "{}"));
        ASSERT_TRUE(fs.writeFileAtomic(root.join("Assets/Scripts/Player.lua"), "return {}\n"));
        ASSERT_TRUE(fs.writeFileAtomic(root.join("Game.frostproj"), settings().dump()));
        ASSERT_TRUE(fs.writeFileAtomic(root.join("FrostEditor"), "scripted tool"));
    }

    Json info() const {
        return Json::object()
            .set("name", "Game")
            .set("version", "0.1")
            .set("directory", root.path())
            .set("projectFile", root.join("Game.frostproj"))
            .set("assetsDirectory", root.join("Assets"))
            .set("assetCount", 2)
            .set("startScene", Json::object().set("handle", toJson(kScene)).set("path", "Assets/Scenes/Main.fscene"));
    }

    ProjectContentRequest request() const {
        return {root.join("FrostEditor"), root.join("Game.frostproj"), std::chrono::seconds(5)};
    }

    void reply(std::string command, Json result) {
        launcher.expect(io::ScriptedProcess{.stdoutBytes = Json::object()
                                                               .set("ok", true)
                                                               .set("command", std::move(command))
                                                               .set("result", std::move(result))
                                                               .dump()});
    }
};

// Stop while launch is returning, before the CLI can enter communicate(). A registered stop
// callback must notice this race and terminate the child rather than wait for its full timeout.
class StopAfterLaunch final : public io::IProcessLauncher {
public:
    StopAfterLaunch(io::IProcessLauncher& inner, std::stop_source& stop) : m_inner(inner), m_stop(stop) {}
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        auto process = m_inner.launch(spec);
        m_stop.request_stop();
        return process;
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec&, const io::AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "this test uses synchronous CLI launches");
    }

private:
    io::IProcessLauncher& m_inner;
    std::stop_source& m_stop;
};

TEST_F(ProjectContentTest, CancellationBeforeLoadNeverLaunchesAChild) {
    std::stop_source stop;
    stop.request_stop();
    auto pending = request();
    pending.stopToken = stop.get_token();
    auto result = loadProjectContent(fs, launcher, pending, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST_F(ProjectContentTest, CancellationAfterLaunchTerminatesActiveChildAndNeverLaunchesAssets) {
    std::stop_source stop;
    auto pending = request();
    pending.stopToken = stop.get_token();
    launcher.expect(io::ScriptedProcess{.neverExits = true});
    StopAfterLaunch cancelling(launcher, stop);
    auto result = loadProjectContent(fs, cancelling, pending, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches.front().spec.arguments.front(), "project.info");
    EXPECT_TRUE(launches.front().spec.killOnClose);
    EXPECT_TRUE(launches.front().terminated);
}

TEST_F(ProjectContentTest, CancellationBeforePublicationDiscardsCompletedSnapshot) {
    reply("project.info", info());
    reply("assets.list", listing());
    std::stop_source stop;
    auto pending = request();
    pending.stopToken = stop.get_token();
    int trustChecks = 0;
    auto result = loadProjectContent(fs, launcher, pending, [&] {
        if (++trustChecks == 4)
            stop.request_stop();
        return true;
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(launcher.launches().size(), 2U);
}

TEST_F(ProjectContentTest, PreservesExactStartSceneAndEngineAssetOrder) {
    auto doc = parseProjectDocument(settings());
    ASSERT_TRUE(doc) << doc.error().message();
    ASSERT_EQ(doc->startScene, kScene);
    auto model = parseProjectContent(*doc, info(), listing());
    ASSERT_TRUE(model) << model.error().message();
    EXPECT_EQ(model->startScene, kScene);
    ASSERT_EQ(model->assets.size(), 2U);
    EXPECT_EQ(model->assets[0].handle, kScene);
    EXPECT_EQ(model->assets[1].path, "Assets/Scripts/Player.lua");
    EXPECT_EQ(model->sceneCount(), 1U);
}

TEST_F(ProjectContentTest, MissingOptionalSettingsDoNotInventEngineDefaults) {
    auto doc = parseProjectDocument(Json::object().set("Version", 1));
    ASSERT_TRUE(doc);
    EXPECT_FALSE(doc->name);
    EXPECT_FALSE(doc->startScene);
    EXPECT_TRUE(parseProjectContent(*doc, info(), listing()));
}

TEST_F(ProjectContentTest, RefusesUnsupportedVersionsAndLossyHandles) {
    auto future = settings().set("Version", 2);
    EXPECT_EQ(parseProjectDocument(future).error().code(), ErrorCode::Unsupported);
    auto doc = settings();
    doc.find("General")->set("StartScene", Json(1e19));
    EXPECT_EQ(parseProjectDocument(doc).error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(readProjectDocument(fs, "relative/Game.frostproj").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(readProjectDocument(fs, root.join("Missing.frostproj")).error().code(), ErrorCode::NotFound);
}

TEST_F(ProjectContentTest, RefusesInconsistentCountsSettingsAndDuplicateAssets) {
    const auto doc = parseProjectDocument(settings());
    ASSERT_TRUE(doc);
    EXPECT_FALSE(parseProjectContent(*doc, info(), listing().set("count", 1)));
    EXPECT_FALSE(parseProjectContent(*doc, info().set("assetCount", 3), listing()));
    EXPECT_FALSE(parseProjectContent(*doc, info().set("name", "Other"), listing()));
    auto duplicate = listing();
    duplicate.find("assets")->push(duplicate.find("assets")->items().front());
    duplicate.set("count", 3);
    EXPECT_FALSE(parseProjectContent(*doc, info().set("assetCount", 3), duplicate));
}

TEST_F(ProjectContentTest, RefusesTraversalAbsoluteAndNonAssetPathsFromEngine) {
    const auto doc = parseProjectDocument(Json::object().set("Version", 1));
    ASSERT_TRUE(doc);
    const auto projectInfo =
        info().set("assetCount", 1).set("startScene", Json::object().set("handle", 0).set("path", ""));
    for (const auto bad : {"../outside.lua", "Assets/../outside.lua", "Assets//Player.lua", "Assets/",
                           "Assets\\Player.lua", "Assets/Player.lua:stream"}) {
        auto assets = listing();
        Json malformed = Json::array({Json::object().set("handle", 42).set("path", bad).set("type", "Script")});
        assets.set("assets", std::move(malformed)).set("count", 1);
        const auto result = parseProjectContent(*doc, projectInfo, assets);
        ASSERT_FALSE(result) << bad;
        EXPECT_NE(result.error().message().find("invalid or duplicate asset"), std::string::npos) << bad;
    }
}

TEST_F(ProjectContentTest, UntrustedAndMissingToolNeverLaunch) {
    auto result = loadProjectContent(fs, launcher, request(), [] { return false; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    auto missing = request();
    missing.program = root.join("unavailable");
    result = loadProjectContent(fs, launcher, missing, [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST_F(ProjectContentTest, ChecksTrustBeforeEveryCommandAndPublication) {
    reply("project.info", info());
    int checks = 0;
    auto result = loadProjectContent(fs, launcher, request(), [&] { return ++checks < 3; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(launcher.launches().size(), 1U);
}

TEST_F(ProjectContentTest, RevokedTrustPreventsPublicationAfterCommandsFinish) {
    reply("project.info", info());
    reply("assets.list", listing());
    int checks = 0;
    auto result = loadProjectContent(fs, launcher, request(), [&] { return ++checks < 4; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(launcher.launches().size(), 2U);
}

TEST_F(ProjectContentTest, NavigationRevalidatesFilesystemAndAssetsContainment) {
    const auto doc = parseProjectDocument(settings());
    ASSERT_TRUE(doc);
    auto model = parseProjectContent(*doc, info(), listing());
    ASSERT_TRUE(model);
    test::ScratchDir outside("project-content-outside");
    model->assetsDirectory = outside.path();
    EXPECT_EQ(resolveProjectAsset(fs, *model, "Assets/Scripts/Player.lua").error().code(), ErrorCode::PermissionDenied);
    model->assetsDirectory = root.join("Assets");
    ASSERT_TRUE(fs.removeFile(root.join("Assets/Scripts/Player.lua")));
    EXPECT_EQ(resolveProjectAsset(fs, *model, "Assets/Scripts/Player.lua").error().code(), ErrorCode::NotFound);
}

TEST_F(ProjectContentTest, LoadsOnlyRealCommandResultsAndResolvesListedAssets) {
    reply("project.info", info());
    reply("assets.list", listing());
    auto model = loadProjectContent(fs, launcher, request(), [] { return true; });
    ASSERT_TRUE(model) << model.error().message();
    ASSERT_EQ(launcher.launches().size(), 2U);
    EXPECT_EQ(launcher.launches()[0].spec.arguments[0], "project.info");
    EXPECT_EQ(launcher.launches()[1].spec.arguments[0], "assets.list");
    auto path = resolveProjectAsset(fs, *model, "Assets/Scripts/Player.lua");
    ASSERT_TRUE(path) << path.error().message();
    EXPECT_EQ(*path, fs.canonicalPath(root.join("Assets/Scripts/Player.lua")).value());
    EXPECT_EQ(resolveProjectAsset(fs, *model, "Assets/Scripts/Unlisted.lua").error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(resolveProjectAsset(fs, *model, "../outside.lua").error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ProjectContentTest, RejectsDifferentPhysicalProjectBeforeAssetCommand) {
    test::ScratchDir other("project-content-other");
    ASSERT_TRUE(fs.writeFileAtomic(other.join("Other.frostproj"), settings().dump()));
    reply("project.info", info().set("directory", other.path()).set("projectFile", other.join("Other.frostproj")));
    auto result = loadProjectContent(fs, launcher, request(), [] { return true; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Corrupt);
    EXPECT_EQ(launcher.launches().size(), 1U);
}

} // namespace
} // namespace frost::engine
