// SPDX-License-Identifier: Apache-2.0
//
// FrostEngine .fmeta-safe file operations and `assets.find-references` CLI runner (FEC-9a).

#include "frost/lang/frost_engine_assets.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_service.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace frost::lang {
namespace {

bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    return std::ranges::equal(a, b, [](char lhs, char rhs) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(lhs) == lower(rhs);
    });
}

bool regularFile(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}

std::string_view lastComponent(std::string_view path) noexcept {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.remove_suffix(1);
    const std::size_t sep = path.find_last_of("/\\");
    return sep == std::string_view::npos ? path : path.substr(sep + 1);
}

std::string_view trimWhitespace(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

Result<Json> parseCliJsonPayload(std::string_view rawText) {
    const std::string_view trimmed = trimWhitespace(rawText);
    if (trimmed.empty())
        return fail(ErrorCode::Corrupt, "empty FrostEditor assets.find-references output");
    if (auto parsed = parseJson(trimmed))
        return parsed;
    // FrostEditor may emit diagnostic lines before the final JSON line if stdout/stderr were merged.
    std::string_view rest = trimmed;
    while (!rest.empty()) {
        const std::size_t nl = rest.find_last_of('\n');
        const std::string_view line = trimWhitespace(nl == std::string_view::npos ? rest : rest.substr(nl + 1));
        if (line.starts_with('{')) {
            if (auto parsed = parseJson(line))
                return parsed;
        }
        if (nl == std::string_view::npos)
            break;
        rest = rest.substr(0, nl);
    }
    return fail(ErrorCode::Corrupt, "invalid JSON in FrostEditor assets.find-references output");
}

ErrorCode mapCliErrorCode(std::string_view code) noexcept {
    if (code == "not_found")
        return ErrorCode::NotFound;
    if (code == "invalid_argument" || code == "unknown_command")
        return ErrorCode::InvalidArgument;
    if (code == "already_exists")
        return ErrorCode::AlreadyExists;
    if (code == "permission_denied")
        return ErrorCode::PermissionDenied;
    return ErrorCode::IoError;
}

} // namespace

bool isFmetaPath(std::string_view path) noexcept {
    const std::string_view name = lastComponent(path);
    if (name.size() <= kFmetaExtension.size())
        return false;
    return equalsIgnoreAsciiCase(name.substr(name.size() - kFmetaExtension.size()), kFmetaExtension);
}

Result<std::string> fmetaSidecarPath(std::string_view assetPath) {
    auto normalized = io::normalizePath(assetPath);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));
    if (io::fileName(*normalized).empty())
        return fail(ErrorCode::InvalidArgument, "asset path must name a file");
    if (isFmetaPath(*normalized))
        return fail(ErrorCode::InvalidArgument,
                    "cannot compute a .fmeta sidecar for a .fmeta file; rename or move the asset itself");
    return *normalized + std::string(kFmetaExtension);
}

Result<FmetaMovePlan> planFmetaMove(io::IFileSystem& fs, std::string_view sourcePath, std::string_view targetPath) {
    if (isFmetaPath(sourcePath))
        return fail(ErrorCode::InvalidArgument,
                    "cannot rename or move a .fmeta file directly; rename or move the asset itself");
    if (isFmetaPath(targetPath))
        return fail(ErrorCode::InvalidArgument,
                    "cannot rename or move an asset to a .fmeta file; rename or move the asset itself");

    auto normalizedSource = io::normalizePath(sourcePath);
    if (!normalizedSource)
        return std::unexpected(std::move(normalizedSource.error()));
    auto normalizedTarget = io::normalizePath(targetPath);
    if (!normalizedTarget)
        return std::unexpected(std::move(normalizedTarget.error()));

    FmetaMovePlan plan;
    plan.sourcePath = std::move(*normalizedSource);
    plan.targetPath = std::move(*normalizedTarget);

    if (regularFile(fs, plan.sourcePath)) {
        const std::string sourceSidecar = plan.sourcePath + std::string(kFmetaExtension);
        if (regularFile(fs, sourceSidecar)) {
            const std::string targetSidecar = plan.targetPath + std::string(kFmetaExtension);
            if (sourceSidecar != targetSidecar) {
                auto targetSidecarExists = fs.exists(targetSidecar);
                if (!targetSidecarExists)
                    return std::unexpected(std::move(targetSidecarExists.error()));
                if (*targetSidecarExists)
                    return fail(ErrorCode::AlreadyExists,
                                std::format("cannot move {}: target sidecar {} already exists", plan.sourcePath,
                                            targetSidecar));
            }
            plan.sourceSidecarPath = sourceSidecar;
            plan.targetSidecarPath = targetSidecar;
        }
    }

    return plan;
}

Result<FmetaMovePlan> executeFmetaMove(io::IFileSystem& fs, const FmetaMovePlan& plan) {
    if (isFmetaPath(plan.sourcePath))
        return fail(ErrorCode::InvalidArgument,
                    "cannot rename or move a .fmeta file directly; rename or move the asset itself");
    if (isFmetaPath(plan.targetPath))
        return fail(ErrorCode::InvalidArgument,
                    "cannot rename or move an asset to a .fmeta file; rename or move the asset itself");
    if (plan.sourceSidecarPath.has_value() != plan.targetSidecarPath.has_value())
        return fail(ErrorCode::InvalidArgument, "incomplete .fmeta move plan");

    if (plan.sourcePath == plan.targetPath)
        return plan;

    if (plan.hasSidecar() && *plan.sourceSidecarPath != *plan.targetSidecarPath) {
        auto targetSidecarExists = fs.exists(*plan.targetSidecarPath);
        if (!targetSidecarExists)
            return std::unexpected(std::move(targetSidecarExists.error()));
        if (*targetSidecarExists)
            return fail(ErrorCode::AlreadyExists, std::format("cannot move {}: target sidecar {} already exists",
                                                              plan.sourcePath, *plan.targetSidecarPath));
    }

    auto moved = fs.rename(plan.sourcePath, plan.targetPath);
    if (!moved)
        return std::unexpected(std::move(moved.error()));

    if (plan.hasSidecar() && *plan.sourceSidecarPath != *plan.targetSidecarPath) {
        auto sidecarMoved = fs.rename(*plan.sourceSidecarPath, *plan.targetSidecarPath);
        if (!sidecarMoved) {
            auto rollback = fs.rename(plan.targetPath, plan.sourcePath);
            if (!rollback)
                return std::unexpected(sidecarMoved.error().withContext(
                    std::format("failed to roll back {} -> {} after .fmeta sidecar move failed: {}", plan.targetPath,
                                plan.sourcePath, rollback.error().describe())));
            return std::unexpected(
                sidecarMoved.error().withContext("rolled back asset move after .fmeta sidecar move failed"));
        }
    }

    return plan;
}

Result<FmetaMovePlan> moveWithFmeta(io::IFileSystem& fs, std::string_view sourcePath, std::string_view targetPath) {
    auto plan = planFmetaMove(fs, sourcePath, targetPath);
    if (!plan)
        return std::unexpected(std::move(plan.error()));
    return executeFmetaMove(fs, *plan);
}

Result<FmetaDeletePlan> planFmetaDelete(io::IFileSystem& fs, std::string_view targetPath) {
    auto normalized = io::normalizePath(targetPath);
    if (!normalized)
        return std::unexpected(std::move(normalized.error()));

    FmetaDeletePlan plan;
    plan.targetPath = std::move(*normalized);
    if (!isFmetaPath(plan.targetPath) && regularFile(fs, plan.targetPath)) {
        const std::string sidecar = plan.targetPath + std::string(kFmetaExtension);
        if (regularFile(fs, sidecar))
            plan.sidecarPath = sidecar;
    }
    return plan;
}

Result<FmetaDeletePlan> executeFmetaDelete(io::IFileSystem& fs, const FmetaDeletePlan& plan) {
    if (plan.targetPath.empty() || !io::isAbsolutePath(plan.targetPath))
        return fail(ErrorCode::InvalidArgument, "delete target path must be absolute");

    if (plan.hasSidecar()) {
        auto savedBytes = fs.readFile(plan.targetPath);
        if (!savedBytes)
            return std::unexpected(std::move(savedBytes.error()));

        auto removed = fs.removeAll(plan.targetPath);
        if (!removed)
            return std::unexpected(std::move(removed.error()));

        auto sidecarRemoved = fs.removeFile(*plan.sidecarPath);
        if (!sidecarRemoved && sidecarRemoved.error().code() != ErrorCode::NotFound) {
            auto restored = fs.writeFileAtomic(plan.targetPath, *savedBytes);
            if (!restored)
                return std::unexpected(sidecarRemoved.error().withContext(
                    std::format("failed to restore {} after .fmeta sidecar deletion failed: {}", plan.targetPath,
                                restored.error().describe())));
            return std::unexpected(
                sidecarRemoved.error().withContext("rolled back asset deletion after .fmeta sidecar deletion failed"));
        }
        return plan;
    }

    auto removed = fs.removeAll(plan.targetPath);
    if (!removed)
        return std::unexpected(std::move(removed.error()));
    return plan;
}

Result<FmetaDeletePlan> deleteWithFmeta(io::IFileSystem& fs, std::string_view targetPath) {
    auto plan = planFmetaDelete(fs, targetPath);
    if (!plan)
        return std::unexpected(std::move(plan.error()));
    return executeFmetaDelete(fs, *plan);
}

std::optional<FrostEngineAssetLocation> locateFrostEngineAsset(io::IFileSystem& fs, std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized || isFmetaPath(*normalized))
        return std::nullopt;

    std::vector<std::string> components;
    std::string current = *normalized;
    constexpr std::size_t kMaxAssetDepth = 16;
    for (std::size_t depth = 0; depth < kMaxAssetDepth; ++depth) {
        const std::string_view name = io::fileName(current);
        const std::string parent(io::parentPath(current));
        if (name.empty() || parent.empty() || parent == current)
            break;
        if (equalsIgnoreAsciiCase(name, "Assets")) {
            if (!components.empty() && isFrostEngineProjectRoot(fs, parent)) {
                std::string relativeBelowAssets;
                for (auto it = components.rbegin(); it != components.rend(); ++it) {
                    if (!relativeBelowAssets.empty())
                        relativeBelowAssets += '/';
                    relativeBelowAssets += *it;
                }
                std::string projectAssetPath = std::string(name) + "/" + relativeBelowAssets;
                return FrostEngineAssetLocation{
                    .projectRoot = parent,
                    .assetPath = std::move(relativeBelowAssets),
                    .projectAssetPath = std::move(projectAssetPath),
                };
            }
        }
        components.emplace_back(name);
        current = parent;
    }
    return std::nullopt;
}

Result<std::string> findFrostEditorExecutable(io::IFileSystem& fs, std::string_view projectRoot,
                                              std::string_view configuredPath) {
    if (!configuredPath.empty()) {
        if (!io::isAbsolutePath(configuredPath))
            return fail(ErrorCode::InvalidArgument, "FrostEditor path must be absolute");
        auto normalized = io::normalizePath(configuredPath);
        if (!normalized || !regularFile(fs, *normalized))
            return fail(ErrorCode::NotFound, "configured FrostEditor executable does not exist");
        return *normalized;
    }

    if (projectRoot.empty() || !io::isAbsolutePath(projectRoot))
        return fail(ErrorCode::InvalidArgument, "project root must be an absolute path");
    auto normalizedRoot = io::normalizePath(projectRoot);
    if (!normalizedRoot)
        return std::unexpected(std::move(normalizedRoot.error()));

#ifdef _WIN32
    static constexpr std::array<std::string_view, 3> kBuildCandidates = {
        "build/windows-debug/bin/FrostEditor.exe",
        "build/windows-release/bin/FrostEditor.exe",
        "build/windows-dist/bin/FrostEditor.exe",
    };
#else
    static constexpr std::array<std::string_view, 3> kBuildCandidates = {
        "build/linux-debug/bin/FrostEditor",
        "build/linux-release/bin/FrostEditor",
        "build/linux-dist/bin/FrostEditor",
    };
#endif

    std::string folder = *normalizedRoot;
    for (std::size_t depth = 0; depth <= kMaxReferenceDepth; ++depth) {
        auto reference = io::joinPath(folder, kFrostEngineScriptApiReference);
        if (reference && regularFile(fs, *reference)) {
            for (const std::string_view relCandidate : kBuildCandidates) {
                auto candidate = io::joinPath(folder, relCandidate);
                if (!candidate)
                    continue;
                auto normalizedCandidate = io::normalizePath(*candidate);
                if (normalizedCandidate && regularFile(fs, *normalizedCandidate))
                    return *normalizedCandidate;
            }
        }
        const std::string parent(io::parentPath(folder));
        if (parent.empty() || parent == folder)
            break;
        folder = parent;
    }

    return fail(ErrorCode::NotFound, "FrostEditor CLI executable was not found");
}

std::vector<std::string> AssetReferenceReport::warningLines() const {
    std::vector<std::string> lines;
    lines.reserve(references.size() + unreadable.size());
    for (const AssetReferenceEntry& ref : references) {
        std::string details;
        if (!ref.type.empty())
            details += ref.type;
        if (ref.count > 0) {
            if (!details.empty())
                details += ", ";
            details += std::format("{} {}", ref.count, ref.count == 1 ? "reference" : "references");
        }
        if (ref.unsaved) {
            if (!details.empty())
                details += ", ";
            details += "unsaved";
        }
        if (details.empty())
            lines.push_back(ref.path);
        else
            lines.push_back(std::format("{} ({})", ref.path, details));
    }
    for (const std::string& path : unreadable)
        lines.push_back(std::format("Unreadable asset while checking references: {}", path));
    return lines;
}

std::string AssetReferenceReport::formatWarningSummary(std::string_view operation) const {
    const std::vector<std::string> lines = warningLines();
    if (lines.empty())
        return {};
    const std::string_view assetName = asset.empty() ? std::string_view("asset") : std::string_view(asset);
    std::string summary;
    if (!references.empty()) {
        if (!operation.empty()) {
            summary = std::format("{} {} is referenced by {} {}:", operation, assetName, references.size(),
                                  references.size() == 1 ? "asset" : "assets");
        } else {
            summary = std::format("{} is referenced by {} {}:", assetName, references.size(),
                                  references.size() == 1 ? "asset" : "assets");
        }
    } else {
        summary = std::format("Reference check for {} reported unreadable assets:", assetName);
    }
    for (const std::string& line : lines) {
        summary += "\n  - ";
        summary += line;
    }
    return summary;
}

Result<AssetReferenceReport> parseAssetReferenceReport(std::string_view jsonText) {
    auto root = parseCliJsonPayload(jsonText);
    if (!root)
        return std::unexpected(std::move(root.error()));
    if (!root->isObject())
        return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references output must be a JSON object");

    const Json* payload = &(*root);
    if (const auto ok = root->boolAt("ok")) {
        if (!*ok) {
            if (const Json* errorObj = root->find("error"); errorObj && errorObj->isObject()) {
                const std::string_view code = errorObj->stringAt("code");
                const std::string_view message = errorObj->stringAt("message");
                return fail(mapCliErrorCode(code), message.empty()
                                                       ? std::string("FrostEditor assets.find-references failed")
                                                       : std::string(message));
            }
            return fail(ErrorCode::IoError, "FrostEditor assets.find-references reported ok=false");
        }
        const Json* resultObj = root->find("result");
        if (!resultObj || !resultObj->isObject())
            return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references envelope is missing result object");
        payload = resultObj;
    }

    const Json* referencesJson = payload->find("references");
    if (!referencesJson || !referencesJson->isArray())
        return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references output is missing references array");

    AssetReferenceReport report;
    report.asset = std::string(payload->stringAt("asset"));

    if (const Json* targetsJson = payload->find("targets")) {
        if (!targetsJson->isArray())
            return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references targets field must be an array");
        for (const Json& targetItem : targetsJson->items()) {
            if (!targetItem.isObject())
                return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references target entry must be an object");
            AssetReferenceTarget target{
                .path = std::string(targetItem.stringAt("path")),
                .type = std::string(targetItem.stringAt("type")),
            };
            if (target.path.empty())
                return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references target entry is missing path");
            report.targets.push_back(std::move(target));
        }
    }

    for (const Json& refItem : referencesJson->items()) {
        if (!refItem.isObject())
            return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references reference entry must be an object");
        AssetReferenceEntry entry;
        entry.path = std::string(refItem.stringAt("path"));
        if (entry.path.empty())
            return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references reference entry is missing path");
        entry.type = std::string(refItem.stringAt("type"));
        entry.count = refItem.integerAt("count").value_or(1);
        entry.unsaved = refItem.boolAt("unsaved").value_or(false);
        if (const Json* refTargets = refItem.find("targets")) {
            if (!refTargets->isArray())
                return fail(ErrorCode::Corrupt,
                            "FrostEditor assets.find-references reference targets field must be an array");
            for (const Json& targetPath : refTargets->items()) {
                if (const std::string* s = targetPath.asString())
                    entry.targets.push_back(*s);
            }
        }
        report.references.push_back(std::move(entry));
    }

    report.count = payload->integerAt("count").value_or(static_cast<std::int64_t>(report.references.size()));

    if (const Json* unreadableJson = payload->find("unreadable")) {
        if (!unreadableJson->isArray())
            return fail(ErrorCode::Corrupt, "FrostEditor assets.find-references unreadable field must be an array");
        for (const Json& item : unreadableJson->items()) {
            if (const std::string* s = item.asString())
                report.unreadable.push_back(*s);
        }
    }

    return report;
}

Result<AssetReferenceReport> runAssetFindReferences(io::IProcessLauncher& launcher,
                                                    const AssetFindReferencesRequest& request) {
    if (!request.trusted)
        return fail(ErrorCode::Untrusted, "refusing to run FrostEditor in an untrusted workspace");
    if (request.executablePath.empty() || !io::isAbsolutePath(request.executablePath))
        return fail(ErrorCode::InvalidArgument, "FrostEditor executable path must be absolute");
    if (request.projectPath.empty() || !io::isAbsolutePath(request.projectPath))
        return fail(ErrorCode::InvalidArgument, "FrostEngine project path must be absolute");
    if (request.assetPath.empty())
        return fail(ErrorCode::InvalidArgument, "asset path must not be empty");

    auto normalizedExe = io::normalizePath(request.executablePath);
    if (!normalizedExe)
        return std::unexpected(std::move(normalizedExe.error()));
    auto normalizedProject = io::normalizePath(request.projectPath);
    if (!normalizedProject)
        return std::unexpected(std::move(normalizedProject.error()));

    std::string workingDir = *normalizedProject;
    if (isFrostEngineProjectFileName(io::fileName(*normalizedProject))) {
        const std::string_view parent = io::parentPath(*normalizedProject);
        if (!parent.empty())
            workingDir = std::string(parent);
    }

    io::ProcessSpec spec;
    spec.executable = std::move(*normalizedExe);
    spec.arguments = {
        "assets.find-references", "--project", *normalizedProject, "--asset", request.assetPath,
    };
    spec.workingDirectory = std::move(workingDir);
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;

    auto processResult = io::runToCompletion(launcher, spec, {}, request.timeout);
    if (!processResult)
        return std::unexpected(std::move(processResult.error()));

    if (!trimWhitespace(processResult->stdoutBytes).empty()) {
        auto parsed = parseAssetReferenceReport(processResult->stdoutBytes);
        if (parsed || processResult->exitCode != 0)
            return parsed;
    }

    if (processResult->exitCode != 0) {
        const std::string_view errText = trimWhitespace(processResult->stderrBytes);
        return fail(ErrorCode::IoError,
                    errText.empty()
                        ? std::format("FrostEditor assets.find-references exited with code {}", processResult->exitCode)
                        : std::format("FrostEditor assets.find-references exited with code {}: {}",
                                      processResult->exitCode, errText));
    }

    return parseAssetReferenceReport(processResult->stdoutBytes);
}

} // namespace frost::lang
