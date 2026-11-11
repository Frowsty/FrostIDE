// SPDX-License-Identifier: Apache-2.0
//
// FEC-3: read-only FrostEngine project settings and content reported by the engine CLI.
#pragma once

#include "frost/engine/engine_cli.hpp"
#include "frost/engine/handle.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::engine {

// Only settings explicitly present in the .frostproj document; defaults belong to the engine.
struct ProjectDocument {
    std::uint32_t formatVersion = 0;
    std::optional<std::string> name;
    std::optional<EngineHandle> startScene;
    friend bool operator==(const ProjectDocument&, const ProjectDocument&) = default;
};

// Pure, thread-safe, O(document size). Supports format Version 1; refuses malformed settings
// (Corrupt) and other versions (Unsupported). Handles never pass through floating point.
[[nodiscard]] Result<ProjectDocument> parseProjectDocument(const rpc::Json& document);
// Reads an absolute .frostproj file through fs (at most 4 MiB). No writes or process launches.
// Errors: InvalidArgument for a relative/non-project path, OutOfRange for oversized files,
// the filesystem's errors, and parseProjectDocument's errors. Call from a worker thread.
[[nodiscard]] Result<ProjectDocument> readProjectDocument(io::IFileSystem& fs, std::string_view file);

struct ProjectAsset {
    EngineHandle handle;
    std::string path; // Engine-reported project-relative Assets/... path, never an inferred file.
    std::string type;
    friend bool operator==(const ProjectAsset&, const ProjectAsset&) = default;
};

// A coherent value snapshot. Assets and their order come from assets.list, not a filesystem scan.
// Format 1 does not declare project modules; no modules are inferred from Lua API namespaces.
struct ProjectContent {
    ProjectDocument document;
    std::string name;
    std::string version;
    std::string directory;
    std::string projectFile;
    std::string assetsDirectory;
    EngineHandle startScene;
    std::string startScenePath;
    std::vector<ProjectAsset> assets;

    [[nodiscard]] std::size_t sceneCount() const noexcept;
    friend bool operator==(const ProjectContent&, const ProjectContent&) = default;
};

// Validates result objects of project.info/assets.list against a read .frostproj
// document. Refuses count mismatches, duplicate handles/paths, unsafe asset paths, invalid
// identities or inconsistent start scenes (Corrupt). Pure, thread-safe, O(n log n).
[[nodiscard]] Result<ProjectContent> parseProjectContent(const ProjectDocument& document, const rpc::Json& projectInfo,
                                                         const rpc::Json& assetListing);

struct ProjectContentRequest {
    std::string program;                                          // Absolute configured FrostEditor CLI program.
    std::string project;                                          // Absolute project directory or .frostproj file.
    std::chrono::milliseconds timeout = std::chrono::seconds(30); // Per command.
    std::stop_token stopToken{};
};

// Calls project.info and assets.list through IProcessLauncher on a worker. Trust
// is checked before each launch and before publication. Physically verifies the reported project
// identity against the requested root/file. Reads settings but never writes them. No fallback
// counts or path guessing. Errors propagate, including Untrusted, Corrupt and tool unavailable.
// Cancellation terminates the active contained CLI process and prevents further launches or
// publication. fs/launcher/trust callback must outlive the call; the callback must be safe on the worker.
[[nodiscard]] Result<ProjectContent> loadProjectContent(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                        const ProjectContentRequest& request,
                                                        const std::function<bool()>& stillTrusted);

// Revalidates a listed asset before navigation, including physical containment beneath the
// engine-reported Assets directory. Refuses unlisted paths and symlink escapes. Does not open
// or modify anything. O(assets + path length), worker thread; propagates filesystem errors.
[[nodiscard]] Result<std::string> resolveProjectAsset(io::IFileSystem& fs, const ProjectContent& content,
                                                      std::string_view relativePath);

} // namespace frost::engine
