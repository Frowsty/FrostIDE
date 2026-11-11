// SPDX-License-Identifier: Apache-2.0
//
// Validates FEC-3 project snapshots from real CLI output and confines asset navigation.
#include "frost/engine/project_content.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace frost::engine {
namespace {

constexpr std::uint64_t kProjectLimit = 4 * 1024 * 1024;

bool projectFileName(std::string_view file) {
    const auto name = io::fileName(file);
    constexpr std::string_view suffix = ".frostproj";
    if (name.size() <= suffix.size())
        return false;
    const auto tail = name.substr(name.size() - suffix.size());
    return std::ranges::equal(tail, suffix, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a) == b;
    });
}

bool samePath(std::string_view a, std::string_view b) { return lang::documentKey(a) == lang::documentKey(b); }

bool beneath(std::string_view path, std::string_view root) {
    std::string prefix = lang::documentKey(root);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    return lang::documentKey(path).starts_with(prefix);
}

bool assetPath(std::string_view path) {
    if (!path.starts_with("Assets/") || path.size() <= 7 || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos)
        return false;
    for (const char c : path)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return false;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (end == std::string_view::npos)
            return true;
        begin = end + 1;
    }
    return false;
}

Result<std::string> pathMember(const rpc::Json& object, std::string_view key) {
    const auto* value = object.find(key);
    if (!value || !value->isString())
        return fail(ErrorCode::Corrupt, std::format("project.info.{}: expected an absolute path", key));
    auto normalized = io::normalizePath(*value->asString());
    if (!normalized)
        return fail(ErrorCode::Corrupt, std::format("project.info.{}: invalid absolute path", key));
    return normalized;
}

std::optional<std::uint64_t> exactCount(const rpc::Json& object, std::string_view key) {
    const auto* value = object.find(key);
    return value && value->kind() == rpc::Json::Kind::Integer ? value->asUnsigned() : std::nullopt;
}

} // namespace

Result<ProjectDocument> parseProjectDocument(const rpc::Json& document) {
    if (!document.isObject())
        return fail(ErrorCode::Corrupt, ".frostproj: expected an object");
    const auto version = exactCount(document, "Version");
    if (!version)
        return fail(ErrorCode::Corrupt, ".frostproj.Version: expected an integer");
    if (*version != 1)
        return fail(ErrorCode::Unsupported, "unsupported .frostproj Version (expected 1)");
    ProjectDocument out;
    out.formatVersion = 1;
    if (const auto* general = document.find("General")) {
        if (!general->isObject())
            return fail(ErrorCode::Corrupt, ".frostproj.General: expected an object");
        if (const auto* name = general->find("Name")) {
            if (!name->isString())
                return fail(ErrorCode::Corrupt, ".frostproj.General.Name: expected a string");
            out.name = *name->asString();
        }
        if (const auto* start = general->find("StartScene")) {
            out.startScene = engineHandleFrom(*start);
            if (!out.startScene)
                return fail(ErrorCode::Corrupt, ".frostproj.General.StartScene: expected an exact handle");
        }
    }
    return out;
}

Result<ProjectDocument> readProjectDocument(io::IFileSystem& fs, std::string_view file) {
    auto normalized = io::normalizePath(file);
    if (!normalized || !projectFileName(file))
        return fail(ErrorCode::InvalidArgument, "expected an absolute .frostproj file");
    const auto status = fs.status(*normalized);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, ".frostproj is not a regular file");
    if (status->size > kProjectLimit)
        return fail(ErrorCode::OutOfRange, ".frostproj exceeds the 4 MiB read limit");
    auto bytes = fs.readFile(*normalized);
    if (!bytes)
        return std::unexpected(std::move(bytes.error()));
    if (bytes->size() > kProjectLimit)
        return fail(ErrorCode::OutOfRange, ".frostproj exceeds the 4 MiB read limit");
    auto parsed = rpc::parseJson(*bytes);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    return parseProjectDocument(*parsed);
}

std::size_t ProjectContent::sceneCount() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(assets, std::string_view("Scene"), &ProjectAsset::type));
}

Result<ProjectContent> parseProjectContent(const ProjectDocument& document, const rpc::Json& projectInfo,
                                           const rpc::Json& assetListing) {
    if (document.formatVersion != 1 || !projectInfo.isObject() || !assetListing.isObject())
        return fail(ErrorCode::Corrupt, "project content requires project settings and CLI result objects");
    ProjectContent out;
    out.document = document;
    for (const auto key : {"name", "version"}) {
        const auto* value = projectInfo.find(key);
        if (!value || !value->isString())
            return fail(ErrorCode::Corrupt, std::format("project.info.{}: expected a string", key));
    }
    out.name = projectInfo.stringAt("name");
    out.version = projectInfo.stringAt("version");
    auto directory = pathMember(projectInfo, "directory");
    auto file = pathMember(projectInfo, "projectFile");
    auto assetsDirectory = pathMember(projectInfo, "assetsDirectory");
    if (!directory || !file || !assetsDirectory)
        return fail(ErrorCode::Corrupt, "project.info contains an invalid project path");
    if (!projectFileName(*file) || !samePath(io::parentPath(*file), *directory) ||
        !beneath(*assetsDirectory, *directory))
        return fail(ErrorCode::Corrupt, "project.info paths are outside the project directory");
    out.directory = std::move(*directory);
    out.projectFile = std::move(*file);
    out.assetsDirectory = std::move(*assetsDirectory);
    const auto* start = projectInfo.find("startScene");
    const auto startHandle = start ? engineHandleAt(*start, "handle") : std::nullopt;
    const auto* startPath = start ? start->find("path") : nullptr;
    if (!startHandle || !startPath || !startPath->isString())
        return fail(ErrorCode::Corrupt, "project.info.startScene: expected an exact handle and a path");
    out.startScene = *startHandle;
    out.startScenePath = *startPath->asString();
    if ((!out.startScenePath.empty() && !assetPath(out.startScenePath)) ||
        (document.startScene && *document.startScene != out.startScene) ||
        (document.name && *document.name != out.name))
        return fail(ErrorCode::Corrupt, "project.info disagrees with the .frostproj settings");

    const auto* assets = assetListing.find("assets");
    const auto count = exactCount(assetListing, "count");
    const auto total = exactCount(projectInfo, "assetCount");
    if (!assets || !assets->isArray() || !count || !total || *count != assets->size() || *total != *count)
        return fail(ErrorCode::Corrupt, "assets.list count disagrees with its entries or project.info.assetCount");
    std::set<std::uint64_t> handles;
    std::set<std::string> paths;
    out.assets.reserve(assets->size());
    for (const auto& asset : assets->items()) {
        const auto handle = engineHandleAt(asset, "handle");
        const auto path = asset.stringAt("path");
        const auto type = asset.stringAt("type");
        if (!handle || handle->value == 0 || !assetPath(path) || type.empty() || !io::joinPath(out.directory, path) ||
            !handles.insert(handle->value).second || !paths.insert(lang::documentKey(path)).second)
            return fail(ErrorCode::Corrupt, "assets.list contains an invalid or duplicate asset");
        out.assets.push_back(ProjectAsset{*handle, std::string(path), std::string(type)});
    }
    if (!out.startScenePath.empty() && !std::ranges::any_of(out.assets, [&](const ProjectAsset& asset) {
            return asset.handle == out.startScene && asset.path == out.startScenePath && asset.type == "Scene";
        }))
        return fail(ErrorCode::Corrupt, "project.info.startScene is not a listed Scene asset");
    return out;
}

Result<ProjectContent> loadProjectContent(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                          const ProjectContentRequest& request,
                                          const std::function<bool()>& stillTrusted) {
    if (!stillTrusted || !stillTrusted())
        return fail(ErrorCode::Untrusted, "trust the project before loading engine content");
    if (request.stopToken.stop_requested())
        return fail(ErrorCode::Cancelled, "engine content loading was cancelled");
    auto program = io::normalizePath(request.program);
    auto project = io::normalizePath(request.project);
    if (!program || !project || request.timeout <= std::chrono::milliseconds::zero())
        return fail(ErrorCode::InvalidArgument,
                    "engine content needs absolute program/project paths and a positive timeout");
    auto programStatus = fs.status(*program);
    if (!programStatus)
        return std::unexpected(std::move(programStatus.error()));
    if (programStatus->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "FrostEditor tool is not a regular file");
    auto inputStatus = fs.status(*project);
    if (!inputStatus)
        return std::unexpected(std::move(inputStatus.error()));
    const bool inputFile = inputStatus->type == io::FileType::Regular && projectFileName(*project);
    if (!inputFile && inputStatus->type != io::FileType::Directory)
        return fail(ErrorCode::InvalidArgument, "engine content needs a project directory or .frostproj");
    auto expectedRoot = fs.canonicalPath(inputFile ? io::parentPath(*project) : *project);
    if (!expectedRoot)
        return std::unexpected(std::move(expectedRoot.error()));
    auto invoke = [&](std::string command) -> Result<rpc::Json> {
        if (!stillTrusted())
            return fail(ErrorCode::Untrusted, "project trust was revoked while loading engine content");
        if (request.stopToken.stop_requested())
            return fail(ErrorCode::Cancelled, "engine content loading was cancelled");
        EngineCliRequest cli;
        cli.command = std::move(command);
        cli.project = *project;
        cli.workingDirectory = *expectedRoot;
        cli.timeout = request.timeout;
        cli.stopToken = request.stopToken;
        return runEngineCliResult(launcher, *program, cli);
    };
    auto info = invoke("project.info");
    if (!info)
        return std::unexpected(std::move(info.error()));
    auto root = pathMember(*info, "directory");
    auto file = pathMember(*info, "projectFile");
    if (!root || !file)
        return fail(ErrorCode::Corrupt, "project.info did not identify a project");
    auto physicalRoot = fs.canonicalPath(*root);
    auto physicalFile = fs.canonicalPath(*file);
    if (!physicalRoot || !physicalFile || !samePath(*physicalRoot, *expectedRoot) ||
        !samePath(io::parentPath(*physicalFile), *expectedRoot))
        return fail(ErrorCode::Corrupt, "project.info belongs to a different project");
    if (inputFile) {
        auto expectedFile = fs.canonicalPath(*project);
        if (!expectedFile || !samePath(*expectedFile, *physicalFile))
            return fail(ErrorCode::Corrupt, "project.info identified a different .frostproj");
    }
    auto document = readProjectDocument(fs, *physicalFile);
    if (!document)
        return std::unexpected(std::move(document.error()));
    auto assets = invoke("assets.list");
    if (!assets)
        return std::unexpected(std::move(assets.error()));
    auto content = parseProjectContent(*document, *info, *assets);
    if (!content)
        return content;
    auto physicalAssets = fs.canonicalPath(content->assetsDirectory);
    if (!physicalAssets || !beneath(*physicalAssets, *expectedRoot))
        return fail(ErrorCode::Corrupt, "the engine's assets directory escapes the project");
    if (!stillTrusted())
        return fail(ErrorCode::Untrusted, "project trust was revoked while loading engine content");
    if (request.stopToken.stop_requested())
        return fail(ErrorCode::Cancelled, "engine content loading was cancelled");
    return content;
}

Result<std::string> resolveProjectAsset(io::IFileSystem& fs, const ProjectContent& content,
                                        std::string_view relativePath) {
    if (!assetPath(relativePath) ||
        !std::ranges::any_of(content.assets, [&](const ProjectAsset& asset) { return asset.path == relativePath; }))
        return fail(ErrorCode::InvalidArgument, "the asset is not listed in this engine project");
    auto path = io::joinPath(content.directory, relativePath);
    if (!path)
        return std::unexpected(std::move(path.error()));
    auto physical = fs.canonicalPath(*path);
    auto root = fs.canonicalPath(content.assetsDirectory);
    auto projectRoot = fs.canonicalPath(content.directory);
    if (!physical)
        return std::unexpected(std::move(physical.error()));
    if (!root)
        return std::unexpected(std::move(root.error()));
    if (!projectRoot)
        return std::unexpected(std::move(projectRoot.error()));
    if (!beneath(*root, *projectRoot) || !beneath(*physical, *root))
        return fail(ErrorCode::PermissionDenied, "the asset resolves outside the project's assets directory");
    auto status = fs.status(*physical);
    if (!status)
        return std::unexpected(std::move(status.error()));
    if (status->type != io::FileType::Regular)
        return fail(ErrorCode::InvalidArgument, "the engine asset is not a regular file");
    return physical;
}

} // namespace frost::engine
