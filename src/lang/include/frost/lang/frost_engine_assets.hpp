// SPDX-License-Identifier: Apache-2.0
//
// FrostEngine .fmeta-safe file operations and `assets.find-references` CLI runner
// (FEC-9a, PLAN.md Appendix C).
//
/*
 * Every asset or script `<path>` below `Assets/` in a FrostEngine project has a sidecar
 * `<path>.fmeta` carrying its 64-bit asset handle and import metadata. Moving, renaming, or
 * deleting the asset without its `.fmeta` sidecar breaks references across scenes and prefabs:
 *   - Renaming or moving `<file>` pairs an existing regular `<file>.fmeta` sibling and moves both
 *     atomically, rolling back the primary move if the sidecar move fails so the pair is never
 *     left split.
 *   - Deleting `<file>` pairs an existing regular `<file>.fmeta` sibling and removes both,
 *     restoring the primary file if removing the sidecar fails.
 *   - Direct rename or move of a `.fmeta` file alone is refused with InvalidArgument, instructing
 *     the caller to rename or move the asset itself.
 *
 * Before renaming, moving, or deleting an asset in a trusted FrostEngine project, the IDE can run:
 *   FrostEditor assets.find-references --project <project-dir-or-.frostproj> --asset <asset-path>
 * through IProcessLauncher (with absolute paths only) and collect referencing assets into a
 * structured warning report. Untrusted workspaces never spawn FrostEditor (ErrorCode::Untrusted).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

inline constexpr std::string_view kFmetaExtension = ".fmeta";

// True when `path`'s file name has a non-empty stem and ends with `.fmeta` (ASCII case-insensitive).
[[nodiscard]] bool isFmetaPath(std::string_view path) noexcept;

// Returns the `<assetPath>.fmeta` sidecar path for a normalised absolute asset path.
// Errors: InvalidArgument when `assetPath` is not a valid absolute path or already ends with `.fmeta`.
[[nodiscard]] Result<std::string> fmetaSidecarPath(std::string_view assetPath);

// Planned rename/move of a file (or directory) and its optional `<file>.fmeta` sibling.
struct FmetaMovePlan {
    std::string sourcePath;
    std::string targetPath;
    std::optional<std::string> sourceSidecarPath;
    std::optional<std::string> targetSidecarPath;

    [[nodiscard]] bool hasSidecar() const noexcept { return sourceSidecarPath.has_value(); }

    friend bool operator==(const FmetaMovePlan&, const FmetaMovePlan&) = default;
};

/*
 * Plans a rename or move from `sourcePath` to `targetPath` on `fs`.
 *   - Refuses direct rename/move of a `.fmeta` file alone (or renaming an asset to `*.fmeta`) with
 *     ErrorCode::InvalidArgument, telling the user to rename or move the asset itself.
 *   - When `sourcePath` is a regular file and `<sourcePath>.fmeta` is a regular file on `fs`,
 *     includes `<sourcePath>.fmeta` -> `<targetPath>.fmeta` in the plan and fails beforehand with
 *     ErrorCode::AlreadyExists if `<targetPath>.fmeta` already exists.
 */
[[nodiscard]] Result<FmetaMovePlan> planFmetaMove(io::IFileSystem& fs, std::string_view sourcePath,
                                                  std::string_view targetPath);

/*
 * Executes `plan` on `fs`. Moves `sourcePath` -> `targetPath`, then moves `sourceSidecarPath` ->
 * `targetSidecarPath` when present. If the sidecar move fails, rolls back `targetPath` ->
 * `sourcePath` so the asset and its `.fmeta` sidecar are never left split.
 */
[[nodiscard]] Result<FmetaMovePlan> executeFmetaMove(io::IFileSystem& fs, const FmetaMovePlan& plan);

// Convenience: plans and executes a `.fmeta`-safe rename or move on `fs`.
[[nodiscard]] Result<FmetaMovePlan> moveWithFmeta(io::IFileSystem& fs, std::string_view sourcePath,
                                                  std::string_view targetPath);

// Planned deletion of a file (or directory) and its optional `<file>.fmeta` sibling.
struct FmetaDeletePlan {
    std::string targetPath;
    std::optional<std::string> sidecarPath;

    [[nodiscard]] bool hasSidecar() const noexcept { return sidecarPath.has_value(); }

    friend bool operator==(const FmetaDeletePlan&, const FmetaDeletePlan&) = default;
};

// Plans a deletion of `targetPath` on `fs`, pairing `<targetPath>.fmeta` when both are regular files.
[[nodiscard]] Result<FmetaDeletePlan> planFmetaDelete(io::IFileSystem& fs, std::string_view targetPath);

// Executes `plan` on `fs`, rolling back the primary file if removing the `.fmeta` sidecar fails.
[[nodiscard]] Result<FmetaDeletePlan> executeFmetaDelete(io::IFileSystem& fs, const FmetaDeletePlan& plan);

// Convenience: plans and executes a `.fmeta`-safe deletion on `fs`.
[[nodiscard]] Result<FmetaDeletePlan> deleteWithFmeta(io::IFileSystem& fs, std::string_view targetPath);

// Location of an asset below `<projectRoot>/Assets/` inside a FrostEngine project.
struct FrostEngineAssetLocation {
    std::string projectRoot;      // Normalised absolute path of the FrostEngine project root.
    std::string assetPath;        // '/'-separated path below Assets/, e.g. "Scripts/Hud.lua".
    std::string projectAssetPath; // '/'-separated path from projectRoot, e.g. "Assets/Scripts/Hud.lua".

    friend bool operator==(const FrostEngineAssetLocation&, const FrostEngineAssetLocation&) = default;
};

/*
 * If `path` is an asset path strictly below `<projectRoot>/Assets/` (and not itself a `.fmeta`
 * sidecar) where `<projectRoot>` is a FrostEngine project root (isFrostEngineProjectRoot), returns
 * the project root and `/`-separated relative asset paths; otherwise returns std::nullopt.
 */
[[nodiscard]] std::optional<FrostEngineAssetLocation> locateFrostEngineAsset(io::IFileSystem& fs,
                                                                             std::string_view path);

/*
 * Resolves the absolute FrostEditor CLI executable for `projectRoot`:
 *   - When `configuredPath` is non-empty, validates that it is an absolute regular file on `fs`.
 *   - Otherwise, searches `projectRoot` and up to kMaxReferenceDepth ancestor directories for a
 *     FrostEngine checkout (marked by Docs/ScriptingAPI.md) containing a built FrostEditor binary
 *     (`build/windows-{debug,release,dist}/bin/FrostEditor.exe` on Windows or
 *     `build/linux-{debug,release,dist}/bin/FrostEditor` on POSIX). Never reads `.frostproj`
 *     executable fields, bare project root binaries, or PATH.
 */
[[nodiscard]] Result<std::string> findFrostEditorExecutable(io::IFileSystem& fs, std::string_view projectRoot,
                                                            std::string_view configuredPath = {});

// One target resolved by `FrostEditor assets.find-references`.
struct AssetReferenceTarget {
    std::string path;
    std::string type;

    friend bool operator==(const AssetReferenceTarget&, const AssetReferenceTarget&) = default;
};

// One referencing asset reported by `FrostEditor assets.find-references`.
struct AssetReferenceEntry {
    std::string path;
    std::string type;
    std::int64_t count = 0;
    std::vector<std::string> targets;
    bool unsaved = false;

    friend bool operator==(const AssetReferenceEntry&, const AssetReferenceEntry&) = default;
};

// Structured output of `FrostEditor assets.find-references --project <project> --asset <asset>`.
struct AssetReferenceReport {
    std::string asset;
    std::vector<AssetReferenceTarget> targets;
    std::vector<AssetReferenceEntry> references;
    std::int64_t count = 0;
    std::vector<std::string> unreadable;

    [[nodiscard]] bool hasReferences() const noexcept { return !references.empty(); }
    [[nodiscard]] bool hasWarnings() const noexcept { return !references.empty() || !unreadable.empty(); }

    // Structured one-line warning entries for each referencing asset and any unreadable asset files.
    [[nodiscard]] std::vector<std::string> warningLines() const;

    // Human-readable warning summary suitable for notifications/dialogs; "" when !hasWarnings().
    [[nodiscard]] std::string formatWarningSummary(std::string_view operation = {}) const;

    friend bool operator==(const AssetReferenceReport&, const AssetReferenceReport&) = default;
};

/*
 * Parses the JSON output of `FrostEditor assets.find-references`. Accepts both the CLI envelope
 * (`{"ok":true,"command":"assets.find-references","result":{...}}` or `{"ok":false,"error":{...}}`)
 * and the bare result object (`{"asset":...,"targets":[...],"references":[...],"count":...,"unreadable":[...]}`).
 */
[[nodiscard]] Result<AssetReferenceReport> parseAssetReferenceReport(std::string_view jsonText);

struct AssetFindReferencesRequest {
    std::string executablePath; // Absolute path to FrostEditor(.exe).
    std::string projectPath;    // Absolute path to the FrostEngine project directory or .frostproj file.
    std::string assetPath;      // Asset path below Assets/ (e.g. "Scripts/Hud.lua").
    bool trusted = false;       // Workspace trust gate: must be true before spawning FrostEditor.
    IClock::Duration timeout = std::chrono::seconds(30);
};

/*
 * Runs `FrostEditor assets.find-references --project <project> --asset <asset>` via `launcher`.
 * Refuses immediately with ErrorCode::Untrusted when `!request.trusted` without invoking `launcher`.
 */
[[nodiscard]] Result<AssetReferenceReport> runAssetFindReferences(io::IProcessLauncher& launcher,
                                                                  const AssetFindReferencesRequest& request);

} // namespace frost::lang
