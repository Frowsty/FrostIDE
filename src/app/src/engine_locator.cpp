// SPDX-License-Identifier: Apache-2.0
// Real filesystem discovery for FrostEngine installations and live project instance records.
#include "engine_locator.hpp"

#include "frost/engine/engine_cli.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <set>

namespace frost::app {
namespace {

std::optional<std::string> canonicalFile(io::IFileSystem& fs, std::string_view path) {
    auto canonical = fs.canonicalPath(path);
    if (!canonical)
        return std::nullopt;
    auto info = fs.status(*canonical);
    if (!info || info->type != io::FileType::Regular)
        return std::nullopt;
    return std::move(*canonical);
}

std::optional<EngineInstallation> installation(io::IFileSystem& fs, std::string path, std::string root,
                                               std::string preset) {
    auto cli = canonicalFile(fs, path);
    if (!cli)
        return std::nullopt;
    std::string gui = *cli;
#ifdef _WIN32
    auto exe = io::joinPath(io::parentPath(*cli), "FrostEditor.exe");
    if (exe)
        if (auto resolved = canonicalFile(fs, *exe))
            gui = std::move(*resolved);
#endif
#ifdef _WIN32
    constexpr std::string_view playerName = "FrostRuntime.exe";
#else
    constexpr std::string_view playerName = "FrostRuntime";
#endif
    std::string player;
    if (auto playerPath = io::joinPath(io::parentPath(gui), playerName))
        if (auto existing = canonicalFile(fs, *playerPath))
            player = std::move(*existing);
    return EngineInstallation{std::move(gui), std::move(*cli), std::move(root), std::move(preset), std::move(player)};
}

} // namespace

Result<std::vector<EngineInstallation>> locateEngineInstallations(io::IFileSystem& fs, std::string_view projectRoot,
                                                                  std::string_view configuredProgram,
                                                                  std::stop_token stop) {
    if (!io::isAbsolutePath(projectRoot) || (!configuredProgram.empty() && !io::isAbsolutePath(configuredProgram)))
        return fail(ErrorCode::InvalidArgument, "FrostEngine discovery requires absolute project and editor paths");
    std::vector<EngineInstallation> out;
    std::set<std::string> seen;
    const auto append = [&](EngineInstallation item) {
        if (seen.insert(lang::documentKey(item.program)).second)
            out.push_back(std::move(item));
    };
    if (!configuredProgram.empty()) {
        auto explicitEditor = installation(fs, std::string(configuredProgram), {}, {});
        if (!explicitEditor)
            return fail(ErrorCode::NotFound, "The configured FrostEditor program is unavailable");
        append(std::move(*explicitEditor));
    }
    auto canonical = fs.canonicalPath(projectRoot);
    if (!canonical)
        return std::unexpected(canonical.error());
    std::string current = std::move(*canonical);
    for (unsigned depth = 0; depth <= 6 && !current.empty() && out.size() < 32; ++depth) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "FrostEngine discovery was cancelled");
        auto build = io::joinPath(current, "build");
        auto entries =
            build ? fs.listDirectory(*build) : Result<std::vector<io::DirEntry>>(fail(ErrorCode::InvalidArgument));
        if (entries && entries->size() <= 256) {
            std::ranges::sort(*entries, {}, &io::DirEntry::name);
            for (const auto& entry : *entries) {
                if (stop.stop_requested())
                    return fail(ErrorCode::Cancelled, "FrostEngine discovery was cancelled");
                if (entry.type != io::FileType::Directory)
                    continue;
                auto found = engine::findEngineEditorProgram(fs, current, entry.name);
                if (found)
                    if (auto item = installation(fs, *found, current, entry.name))
                        append(std::move(*item));
                if (out.size() >= 32)
                    break;
            }
        }
        const std::string parent(io::parentPath(current));
        if (parent.empty() || parent == current)
            break;
        current = parent;
    }
    return out;
}

Result<std::vector<EngineInstanceRecord>> listEngineInstances(io::IFileSystem& fs, std::string_view automationDirectory,
                                                              std::string_view projectRoot, std::stop_token stop) {
    if (!io::isAbsolutePath(automationDirectory) || !io::isAbsolutePath(projectRoot))
        return fail(ErrorCode::InvalidArgument, "FrostEngine instance discovery requires absolute paths");
    auto physicalRoot = fs.physicalPath(projectRoot);
    if (!physicalRoot)
        return std::unexpected(physicalRoot.error());
    const std::string rootKey = lang::documentKey(*physicalRoot);
    std::vector<std::string> records;
    for (const auto subdirectory : {std::string_view("Instances"), std::string_view("Projects")}) {
        auto directory = io::joinPath(automationDirectory, subdirectory);
        if (!directory)
            continue;
        auto entries = fs.listDirectory(*directory);
        if (!entries) {
            if (entries.error().code() != ErrorCode::NotFound)
                return fail(ErrorCode::IoError, "Cannot read FrostEngine instance records");
            continue;
        }
        if (entries->size() > 512)
            return fail(ErrorCode::OutOfRange, "Too many FrostEngine instance records");
        for (const auto& entry : *entries) {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "FrostEngine instance discovery was cancelled");
            auto path = io::joinPath(*directory, entry.name);
            if (!path)
                continue;
            if (entry.type == io::FileType::Regular && entry.name.ends_with(".json"))
                records.push_back(std::move(*path));
            else if (subdirectory == "Projects" && entry.type == io::FileType::Directory) {
                auto nested = fs.listDirectory(*path);
                if (!nested || nested->size() > 512)
                    continue;
                for (const auto& child : *nested)
                    if (child.type == io::FileType::Regular && child.name.ends_with(".json"))
                        if (auto childPath = io::joinPath(*path, child.name))
                            records.push_back(std::move(*childPath));
            }
            if (records.size() > 1024)
                return fail(ErrorCode::OutOfRange, "Too many FrostEngine instance records");
        }
    }
    if (auto fallback = io::joinPath(automationDirectory, "Automation.json"))
        records.push_back(std::move(*fallback));
    std::vector<EngineInstanceRecord> out;
    std::set<std::string> seen;
    for (const auto& path : records) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "FrostEngine instance discovery was cancelled");
        auto info = fs.status(path);
        if (!info || info->type != io::FileType::Regular || info->size > 65536)
            continue;
        auto bytes = fs.readFile(path);
        if (!bytes || bytes->size() > 65536)
            continue;
        auto document = lang::parseJson(*bytes);
        if (!document || !document->isObject() || document->stringAt("host") != "127.0.0.1")
            continue;
        const auto port = document->integerAt("port");
        if (!port || *port < 1 || *port > 65535)
            continue;
        if (!io::isAbsolutePath(document->stringAt("projectPath")))
            continue;
        auto project = fs.physicalPath(document->stringAt("projectPath"));
        if (!project || lang::documentKey(*project) != rootKey)
            continue;
        const std::string instance(document->stringAt("instanceId"));
        const std::string key = instance.empty() ? std::to_string(*port) : instance;
        if (!seen.insert(key).second)
            continue;
        out.push_back(EngineInstanceRecord{instance, std::move(*project), static_cast<std::uint16_t>(*port),
                                           std::move(*document)});
    }
    std::ranges::sort(out, {}, &EngineInstanceRecord::instanceId);
    return out;
}

bool retainEngineProjectIdentity(io::IFileSystem& fs, std::string_view lexicalRoot, std::string_view originalKey,
                                 const std::shared_ptr<std::atomic<bool>>& admission) {
    if (!admission->load())
        return false;
    auto current = fs.physicalPath(lexicalRoot);
    if (!current || lang::documentKey(*current) != originalKey) {
        admission->store(false);
        return false;
    }
    return admission->load();
}

} // namespace frost::app
