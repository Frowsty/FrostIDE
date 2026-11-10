// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for FrostEngine .fmeta-safe file operations and `assets.find-references` (FEC-9a).

#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/lang/frost_engine_assets.hpp"
#include "frost/lang/frost_engine_lua.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace frost;
using namespace frost::lang;

#ifdef _WIN32
constexpr std::string_view kRoot = "C:\\engine";
constexpr std::string_view kEditorRel = "build/windows-debug/bin/FrostEditor.exe";
#else
constexpr std::string_view kRoot = "/engine";
constexpr std::string_view kEditorRel = "build/linux-debug/bin/FrostEditor";
#endif

std::string join(std::string_view relative) { return io::joinPath(kRoot, relative).value(); }

void writeFile(io::MemFs& fs, std::string_view relative, std::string_view content) {
    const std::string path = join(relative);
    ASSERT_TRUE(fs.createDirectories(io::parentPath(path)).has_value()) << path;
    ASSERT_TRUE(fs.writeFileAtomic(path, content).has_value()) << path;
}

TEST(FrostEngineAssets, RecognizesFmetaPathsAndComputesSidecarPaths) {
    EXPECT_TRUE(isFmetaPath(join("Game/Assets/Scripts/Hud.lua.fmeta")));
    EXPECT_TRUE(isFmetaPath(join("Game/Assets/Click.WAV.FMETA")));
    EXPECT_FALSE(isFmetaPath(join("Game/Assets/.fmeta")));
    EXPECT_FALSE(isFmetaPath(join("Game/Assets/Scripts/Hud.lua")));

    auto sidecar = fmetaSidecarPath(join("Game/Assets/Scripts/Hud.lua"));
    ASSERT_TRUE(sidecar.has_value()) << sidecar.error().describe();
    EXPECT_EQ(*sidecar, join("Game/Assets/Scripts/Hud.lua.fmeta"));

    auto nestedSidecar = fmetaSidecarPath(*sidecar);
    ASSERT_FALSE(nestedSidecar.has_value());
    EXPECT_EQ(nestedSidecar.error().code(), ErrorCode::InvalidArgument);

    auto relativeSidecar = fmetaSidecarPath("Assets/Scripts/Hud.lua");
    ASSERT_FALSE(relativeSidecar.has_value());
    EXPECT_EQ(relativeSidecar.error().code(), ErrorCode::InvalidArgument);
}

TEST(FrostEngineAssets, MovesAssetAndItsFmetaSidecarTogether) {
    ManualClock clock;
    io::MemFs fs(clock);
    writeFile(fs, "Game/Assets/Scripts/Hud.lua", "return {}\n");
    writeFile(fs, "Game/Assets/Scripts/Hud.lua.fmeta", "Handle: 3484102375034304068\n");
    ASSERT_TRUE(fs.createDirectories(join("Game/Assets/UI")).has_value());

    auto moved = moveWithFmeta(fs, join("Game/Assets/Scripts/Hud.lua"), join("Game/Assets/UI/MainHud.lua"));
    ASSERT_TRUE(moved.has_value()) << moved.error().describe();
    EXPECT_TRUE(moved->hasSidecar());
    EXPECT_EQ(moved->sourceSidecarPath, join("Game/Assets/Scripts/Hud.lua.fmeta"));
    EXPECT_EQ(moved->targetSidecarPath, join("Game/Assets/UI/MainHud.lua.fmeta"));

    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/Hud.lua")).value_or(true));
    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(true));
    EXPECT_EQ(fs.readFile(join("Game/Assets/UI/MainHud.lua")).value_or(""), "return {}\n");
    EXPECT_EQ(fs.readFile(join("Game/Assets/UI/MainHud.lua.fmeta")).value_or(""), "Handle: 3484102375034304068\n");
}

TEST(FrostEngineAssets, RefusesStandaloneFmetaRenameOrMove) {
    ManualClock clock;
    io::MemFs fs(clock);
    writeFile(fs, "Game/Assets/Scripts/Hud.lua", "return {}\n");
    writeFile(fs, "Game/Assets/Scripts/Hud.lua.fmeta", "Handle: 1\n");

    auto directSidecarMove =
        moveWithFmeta(fs, join("Game/Assets/Scripts/Hud.lua.fmeta"), join("Game/Assets/Scripts/Other.lua.fmeta"));
    ASSERT_FALSE(directSidecarMove.has_value());
    EXPECT_EQ(directSidecarMove.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(directSidecarMove.error().message().find("asset itself"), std::string::npos);
    EXPECT_TRUE(fs.exists(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(false));

    auto moveToFmeta = moveWithFmeta(fs, join("Game/Assets/Scripts/Hud.lua"), join("Game/Assets/Scripts/Hud.fmeta"));
    ASSERT_FALSE(moveToFmeta.has_value());
    EXPECT_EQ(moveToFmeta.error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(fs.exists(join("Game/Assets/Scripts/Hud.lua")).value_or(false));
}

TEST(FrostEngineAssets, RollsBackMainAssetMoveWhenSidecarMoveFails) {
    ManualClock clock;
    io::MemFs fs(clock);
    writeFile(fs, "Game/Assets/Scripts/Hud.lua", "return { version = 1 }\n");
    writeFile(fs, "Game/Assets/Scripts/Hud.lua.fmeta", "Handle: 42\n");

    auto plan = planFmetaMove(fs, join("Game/Assets/Scripts/Hud.lua"), join("Game/Assets/Scripts/HudRenamed.lua"));
    ASSERT_TRUE(plan.has_value()) << plan.error().describe();
    ASSERT_TRUE(plan->hasSidecar());

    // Fault the second rename (the .fmeta sidecar rename) after the main file has already moved.
    fs.injectFault(io::FsOp::Rename, *plan->sourceSidecarPath, ErrorCode::PermissionDenied);

    auto executed = executeFmetaMove(fs, *plan);
    ASSERT_FALSE(executed.has_value());
    EXPECT_EQ(executed.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(executed.error().context().empty());

    // Both the main asset and its .fmeta sidecar must remain paired at the original paths.
    EXPECT_TRUE(fs.exists(join("Game/Assets/Scripts/Hud.lua")).value_or(false));
    EXPECT_TRUE(fs.exists(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(false));
    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/HudRenamed.lua")).value_or(true));
    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/HudRenamed.lua.fmeta")).value_or(true));
    EXPECT_EQ(fs.readFile(join("Game/Assets/Scripts/Hud.lua")).value_or(""), "return { version = 1 }\n");
    EXPECT_EQ(fs.readFile(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(""), "Handle: 42\n");
}

TEST(FrostEngineAssets, DeletesAssetAndSidecarTogetherAndRollsBackOnSidecarFailure) {
    ManualClock clock;
    io::MemFs fs(clock);
    writeFile(fs, "Game/Assets/Scripts/Hud.lua", "return { hud = true }\n");
    writeFile(fs, "Game/Assets/Scripts/Hud.lua.fmeta", "Handle: 99\n");

    // Fault removing the sidecar first: primary file must be restored so the pair is not split.
    fs.injectFault(io::FsOp::RemoveFile, join("Game/Assets/Scripts/Hud.lua.fmeta"), ErrorCode::PermissionDenied);
    auto failedDelete = deleteWithFmeta(fs, join("Game/Assets/Scripts/Hud.lua"));
    ASSERT_FALSE(failedDelete.has_value());
    EXPECT_EQ(failedDelete.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(fs.readFile(join("Game/Assets/Scripts/Hud.lua")).value_or(""), "return { hud = true }\n");
    EXPECT_EQ(fs.readFile(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(""), "Handle: 99\n");

    // Without the fault, both files are removed together.
    auto deleted = deleteWithFmeta(fs, join("Game/Assets/Scripts/Hud.lua"));
    ASSERT_TRUE(deleted.has_value()) << deleted.error().describe();
    EXPECT_TRUE(deleted->hasSidecar());
    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/Hud.lua")).value_or(true));
    EXPECT_FALSE(fs.exists(join("Game/Assets/Scripts/Hud.lua.fmeta")).value_or(true));
}

TEST(FrostEngineAssets, LocatesProjectAssetAndFindsFrostEditorInEngineCheckoutOnly) {
    ManualClock clock;
    io::MemFs fs(clock);
    writeFile(fs, kFrostEngineScriptApiReference, "Generated by ScriptApiDocs::GenerateMarkdown()\n");
    writeFile(fs, kEditorRel, "binary");
    writeFile(fs, "Samples/MiniMMO/MiniMMO.frostproj", "{}");
    writeFile(fs, "Samples/MiniMMO/Assets/Scripts/Hud.lua", "return {}\n");
    writeFile(fs, "Samples/MiniMMO/Assets/Scripts/Hud.lua.fmeta", "Handle: 1\n");
    // A planted binary in the project root must never be picked up.
    writeFile(fs, "Samples/MiniMMO/FrostEditor.exe", "planted");
    writeFile(fs, "Samples/MiniMMO/FrostEditor", "planted");

    auto loc = locateFrostEngineAsset(fs, join("Samples/MiniMMO/Assets/Scripts/Hud.lua"));
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->projectRoot, join("Samples/MiniMMO"));
    EXPECT_EQ(loc->assetPath, "Scripts/Hud.lua");
    EXPECT_EQ(loc->projectAssetPath, "Assets/Scripts/Hud.lua");

    // .fmeta sidecars and files outside Assets/ are not asset locations.
    EXPECT_FALSE(locateFrostEngineAsset(fs, join("Samples/MiniMMO/Assets/Scripts/Hud.lua.fmeta")).has_value());
    EXPECT_FALSE(locateFrostEngineAsset(fs, join("Samples/MiniMMO/MiniMMO.frostproj")).has_value());

    auto editor = findFrostEditorExecutable(fs, loc->projectRoot);
    ASSERT_TRUE(editor.has_value()) << editor.error().describe();
    EXPECT_EQ(*editor, join(kEditorRel));

    // Without the engine checkout reference marker, planted project binaries are ignored.
    ASSERT_TRUE(fs.removeFile(join(kFrostEngineScriptApiReference)).has_value());
    auto missingEditor = findFrostEditorExecutable(fs, loc->projectRoot);
    ASSERT_FALSE(missingEditor.has_value());
    EXPECT_EQ(missingEditor.error().code(), ErrorCode::NotFound);
}

TEST(FrostEngineAssets, ParsesFindReferencesEnvelopeAndBareObjectAndFormatsWarnings) {
    constexpr std::string_view kEnvelopeJson = R"({
        "command": "assets.find-references",
        "ok": true,
        "result": {
            "asset": "Assets/Scripts/Hud.lua",
            "count": 2,
            "references": [
                {
                    "count": 1,
                    "handle": 10096165994471302638,
                    "path": "Assets/Prefabs/UI/Hud.fprefab",
                    "targets": ["Assets/Scripts/Hud.lua"],
                    "type": "Prefab",
                    "unsaved": false
                },
                {
                    "count": 2,
                    "handle": 10315235424234383286,
                    "path": "Assets/Scenes/MiniMMO.fscene",
                    "targets": ["Assets/Scripts/Hud.lua"],
                    "type": "Scene",
                    "unsaved": true
                }
            ],
            "targets": [
                {
                    "handle": 3484102375034304068,
                    "path": "Assets/Scripts/Hud.lua",
                    "type": "Script"
                }
            ],
            "unreadable": ["Assets/Scenes/Corrupt.fscene"]
        }
    })";

    auto report = parseAssetReferenceReport(kEnvelopeJson);
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    EXPECT_EQ(report->asset, "Assets/Scripts/Hud.lua");
    EXPECT_EQ(report->count, 2);
    ASSERT_EQ(report->targets.size(), 1u);
    EXPECT_EQ(report->targets[0].path, "Assets/Scripts/Hud.lua");
    EXPECT_EQ(report->targets[0].type, "Script");
    ASSERT_EQ(report->references.size(), 2u);
    EXPECT_EQ(report->references[0].path, "Assets/Prefabs/UI/Hud.fprefab");
    EXPECT_EQ(report->references[0].type, "Prefab");
    EXPECT_EQ(report->references[0].count, 1);
    EXPECT_FALSE(report->references[0].unsaved);
    EXPECT_EQ(report->references[1].path, "Assets/Scenes/MiniMMO.fscene");
    EXPECT_EQ(report->references[1].type, "Scene");
    EXPECT_EQ(report->references[1].count, 2);
    EXPECT_TRUE(report->references[1].unsaved);
    EXPECT_EQ(report->unreadable, (std::vector<std::string>{"Assets/Scenes/Corrupt.fscene"}));

    const auto warnings = report->warningLines();
    ASSERT_EQ(warnings.size(), 3u);
    EXPECT_EQ(warnings[0], "Assets/Prefabs/UI/Hud.fprefab (Prefab, 1 reference)");
    EXPECT_EQ(warnings[1], "Assets/Scenes/MiniMMO.fscene (Scene, 2 references, unsaved)");
    EXPECT_NE(warnings[2].find("Assets/Scenes/Corrupt.fscene"), std::string::npos);

    const std::string summary = report->formatWarningSummary("Renaming");
    EXPECT_NE(summary.find("Assets/Prefabs/UI/Hud.fprefab"), std::string::npos);
    EXPECT_NE(summary.find("Assets/Scenes/MiniMMO.fscene"), std::string::npos);
    EXPECT_NE(summary.find("Assets/Scenes/Corrupt.fscene"), std::string::npos);

    // Bare result JSON (without the CLI envelope) also parses identically.
    constexpr std::string_view kBareJson = R"({
        "asset": "Assets/Scripts/Hud.lua",
        "targets": [{"path": "Assets/Scripts/Hud.lua", "type": "Script"}],
        "references": [{"path": "Assets/Prefabs/UI/Hud.fprefab", "type": "Prefab", "count": 1, "targets": ["Assets/Scripts/Hud.lua"], "unsaved": false}],
        "count": 1,
        "unreadable": []
    })";
    auto bareReport = parseAssetReferenceReport(kBareJson);
    ASSERT_TRUE(bareReport.has_value()) << bareReport.error().describe();
    EXPECT_EQ(bareReport->asset, "Assets/Scripts/Hud.lua");
    ASSERT_EQ(bareReport->references.size(), 1u);
    EXPECT_EQ(bareReport->references[0].path, "Assets/Prefabs/UI/Hud.fprefab");

    // CLI error envelope maps to ErrorCode.
    constexpr std::string_view kErrorJson =
        R"({"command":"assets.find-references","ok":false,"error":{"code":"not_found","message":"Asset not found"}})";
    auto errorReport = parseAssetReferenceReport(kErrorJson);
    ASSERT_FALSE(errorReport.has_value());
    EXPECT_EQ(errorReport.error().code(), ErrorCode::NotFound);
}

TEST(FrostEngineAssets, TrustGateBlocksProcessSpawnInUntrustedWorkspaceAndRunsWhenTrusted) {
    io::ScriptedProcessLauncher launcher;
    const std::string exePath = join(kEditorRel);
    const std::string projectPath = join("Samples/MiniMMO");

    // Untrusted request must fail with Untrusted and never touch IProcessLauncher.
    AssetFindReferencesRequest untrustedReq;
    untrustedReq.executablePath = exePath;
    untrustedReq.projectPath = projectPath;
    untrustedReq.assetPath = "Scripts/Hud.lua";
    untrustedReq.trusted = false;
    auto untrusted = runAssetFindReferences(launcher, untrustedReq);
    ASSERT_FALSE(untrusted.has_value());
    EXPECT_EQ(untrusted.error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());

    // Trusted request spawns FrostEditor with exact arguments and parses the JSON response.
    io::ScriptedProcess script;
    script.expectExecutable = exePath;
    script.exitCode = 0;
    script.stdoutBytes =
        R"({"command":"assets.find-references","ok":true,"result":{"asset":"Assets/Scripts/Hud.lua","count":1,"references":[{"count":1,"path":"Assets/Prefabs/UI/Hud.fprefab","targets":["Assets/Scripts/Hud.lua"],"type":"Prefab","unsaved":false}],"targets":[{"path":"Assets/Scripts/Hud.lua","type":"Script"}],"unreadable":[]}})"
        "\n";
    launcher.expect(std::move(script));

    AssetFindReferencesRequest trustedReq;
    trustedReq.executablePath = exePath;
    trustedReq.projectPath = projectPath;
    trustedReq.assetPath = "Scripts/Hud.lua";
    trustedReq.trusted = true;
    auto trusted = runAssetFindReferences(launcher, trustedReq);
    ASSERT_TRUE(trusted.has_value()) << trusted.error().describe();
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_EQ(launcher.launches()[0].spec.executable, exePath);
    EXPECT_EQ(
        launcher.launches()[0].spec.arguments,
        (std::vector<std::string>{"assets.find-references", "--project", projectPath, "--asset", "Scripts/Hud.lua"}));
    ASSERT_EQ(trusted->references.size(), 1u);
    EXPECT_EQ(trusted->references[0].path, "Assets/Prefabs/UI/Hud.fprefab");
}

} // namespace
