// SPDX-License-Identifier: Apache-2.0
// Bounded wire validation and asynchronous read-only project rename service.
#include "frost/lang/cpp_rename_plan.hpp"

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/spawn_log.hpp"

#include <algorithm>
#include <map>
#include <set>
namespace frost::lang {
Json cppRenameRequestJson(const CppRenameRequest& request) {
    std::vector<Json> overlays;
    for (const auto& item : request.overlays)
        overlays.push_back(Json::object().set("path", item.path).set("contents", item.contents));
    return Json::object()
        .set("projectRoot", request.projectRoot)
        .set("seedTranslationUnit", request.seedTranslationUnit)
        .set("path", request.path)
        .set("offset", request.offset)
        .set("newName", request.newName)
        .set("compilationDatabase", request.compilationDatabase)
        .set("overlays", Json::array(std::move(overlays)));
}
Result<CppRenameProposal> parseCppRenameProposal(std::string_view bytes) {
    if (bytes.size() > 64 * 1024 * 1024)
        return fail(ErrorCode::InvalidArgument, "Rename proposal exceeds64MiB");
    auto parsed = parseJson(bytes);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    if (parsed->stringAt("kind") != "cpp-rename-plan")
        return fail(ErrorCode::InvalidArgument, "Unexpected rename proposal kind");
    CppRenameProposal result;
    result.status = std::string(parsed->stringAt("status"));
    if (result.status != "ready" && result.status != "conflict" && result.status != "unsupported" &&
        result.status != "parse-error" && result.status != "invalid-input")
        return fail(ErrorCode::InvalidArgument, "Unknown rename result status");
    if (const auto* diagnostics = parsed->find("diagnostics"))
        for (const auto& item : diagnostics->items()) {
            if (!item.asString())
                return fail(ErrorCode::InvalidArgument, "Invalid rename diagnostic");
            result.diagnostics.push_back(*item.asString());
        }
    const auto* files = parsed->find("files");
    if (!files || files->kind() != Json::Kind::Array)
        return fail(ErrorCode::InvalidArgument, "Rename files missing");
    if (!result.ready()) {
        if (!files->items().empty())
            return fail(ErrorCode::InvalidArgument, "Refused proposal contains edits");
        return result;
    }
    const auto* count = parsed->find("translationUnitConfigurations");
    if (!count || !count->asUnsigned() || *count->asUnsigned() == 0)
        return fail(ErrorCode::InvalidArgument, "No checked compilation configurations");
    const auto* schema = parsed->find("schemaVersion");
    if (!schema || schema->asUnsigned() != 1)
        return fail(ErrorCode::Unsupported, "Unsupported C++ rename schema");
    result.compilationDatabaseSha256 = std::string(parsed->stringAt("compilationDatabaseSha256"));
    if (result.compilationDatabaseSha256.size() != 64)
        return fail(ErrorCode::InvalidArgument, "Missing compile-command receipt");
    result.translationUnitConfigurations = *count->asUnsigned();
    if (parsed->boolAt("atomic") != true)
        return fail(ErrorCode::InvalidArgument, "Rename proposal must remain atomic");
    std::set<std::string> paths;
    for (const auto& item : files->items()) {
        for (const auto key : {"path", "before", "after", "beforeSha256", "afterSha256"})
            if (!item.find(key) || !item.find(key)->asString())
                return fail(ErrorCode::InvalidArgument, "Rename image field missing");
        if (!item.boolAt("overlay").has_value())
            return fail(ErrorCode::InvalidArgument, "Rename image overlay flag missing");
        CppRenameFile file{std::string(item.stringAt("path")),        std::string(item.stringAt("before")),
                           std::string(item.stringAt("after")),       std::string(item.stringAt("beforeSha256")),
                           std::string(item.stringAt("afterSha256")), item.boolAt("overlay").value_or(false)};
        if (!io::isAbsolutePath(file.path) || !paths.insert(file.path).second || file.before == file.after ||
            io::sha256Hex(file.before) != file.beforeSha256 || io::sha256Hex(file.after) != file.afterSha256)
            return fail(ErrorCode::InvalidArgument, "Invalid or inconsistent rename image");
        result.files.push_back(std::move(file));
    }
    const auto* dependencies = parsed->find("dependencies");
    const auto* missing = parsed->find("missingDependencies");
    if (result.files.empty() || !dependencies || dependencies->kind() != Json::Kind::Array || !missing ||
        missing->kind() != Json::Kind::Array)
        return fail(ErrorCode::InvalidArgument, "Rename dependency receipts missing");
    std::set<std::string> dependencyPaths;
    for (const auto& item : dependencies->items()) {
        const auto* size = item.find("bytes");
        auto digest = std::string(item.stringAt("sha256"));
        auto path = std::string(item.stringAt("path"));
        if (!io::isAbsolutePath(path) || !dependencyPaths.insert(path).second || !size || !size->asUnsigned() ||
            digest.size() != 64 || std::ranges::any_of(digest, [](char ch) {
                return !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
            }))
            return fail(ErrorCode::InvalidArgument, "Malformed rename dependency");
        result.dependencies.push_back(
            {std::move(path), std::move(digest), *size->asUnsigned(), item.boolAt("overlay").value_or(false)});
    }
    for (const auto& file : result.files) {
        const auto dependency =
            std::ranges::find_if(result.dependencies, [&](const auto& item) { return item.path == file.path; });
        if (dependency == result.dependencies.end() || dependency->sha256 != file.beforeSha256 ||
            dependency->bytes != file.before.size() || dependency->overlay != file.overlay)
            return fail(ErrorCode::InvalidArgument, "Edited file lacks matching original receipt");
    }
    for (const auto& item : missing->items()) {
        if (!item.asString() || !io::isAbsolutePath(*item.asString()) ||
            !dependencyPaths.insert(*item.asString()).second)
            return fail(ErrorCode::InvalidArgument, "Conflicting missing-path dependency");
        result.missingDependencies.push_back(*item.asString());
    }
    const auto* directories = parsed->find("directoryDependencies");
    if (!directories || directories->kind() != Json::Kind::Array)
        return fail(ErrorCode::InvalidArgument, "Directory read dependencies missing");
    for (const auto& item : directories->items()) {
        if (!item.asString() || !io::isAbsolutePath(*item.asString()) ||
            !dependencyPaths.insert(*item.asString()).second)
            return fail(ErrorCode::InvalidArgument, "Conflicting directory read dependency");
        result.directoryDependencies.push_back(*item.asString());
    }
    if (dependencyPaths.size() > 32768)
        return fail(ErrorCode::InvalidArgument, "Rename read dependency budget exceeded");
    return result;
}
sched::Task<Result<CppRenameProposal>> requestCppRenamePlan(io::IProcessLauncher& launcher, io::AsyncContext context,
                                                            std::string helper, CppRenameRequest request,
                                                            IClock::Duration timeout, std::stop_token stop) {
    if (!io::isAbsolutePath(helper) || !io::isAbsolutePath(request.projectRoot) || !io::isAbsolutePath(request.path) ||
        (!request.seedTranslationUnit.empty() && !io::isAbsolutePath(request.seedTranslationUnit)))
        co_return fail(ErrorCode::InvalidArgument, "Rename requires absolute helper/project/source paths");
    if (!request.llvmRoot.empty() && !io::isAbsolutePath(request.llvmRoot))
        co_return fail(ErrorCode::InvalidArgument, "Pinned LLVM root must be absolute");
    std::map<std::string, std::string> overlays;
    for (const auto& item : request.overlays) {
        auto path = io::normalizePath(item.path);
        if (!path || !overlays.emplace(*path, item.contents).second)
            co_return fail(ErrorCode::InvalidArgument, "Invalid/duplicate dirty overlay path");
    }
    auto payload = cppRenameRequestJson(request).dump();
    if (payload.size() > 32 * 1024 * 1024)
        co_return fail(ErrorCode::InvalidArgument, "Rename request exceeds32MiB");
    io::ProcessSpec spec;
    spec.executable = std::move(helper);
    spec.arguments = {"--plan-project-rename"};
    spec.workingDirectory = request.projectRoot;
    spec.stdinMode = io::Stdio::Pipe;
    spec.belowNormalPriority = true;
    if (!request.llvmRoot.empty())
        spec.environment.emplace_back("FROST_LLVM_ROOT", request.llvmRoot);
    auto child = launcher.launchAsync(spec, context);
    if (!child)
        co_return std::unexpected(std::move(child.error()));
    auto completed = co_await (*child)->communicate(std::move(payload), timeout, stop);
    if (!completed)
        co_return std::unexpected(std::move(completed.error()));
    auto proposal = parseCppRenameProposal(completed->stdoutBytes);
    if (!proposal)
        co_return std::unexpected(std::move(proposal.error()));
    if (proposal->ready())
        for (const auto& dependency : proposal->dependencies) {
            auto path = io::normalizePath(dependency.path);
            if (!path)
                co_return std::unexpected(std::move(path.error()));
            auto captured = overlays.find(*path);
            if (dependency.overlay != (captured != overlays.end()) ||
                (captured != overlays.end() &&
                 (io::sha256Hex(captured->second) != dependency.sha256 || captured->second.size() != dependency.bytes)))
                co_return fail(ErrorCode::InvalidArgument, "Dirty overlay receipt differs from captured request");
        }
    if (proposal->ready() && proposal->compilationDatabaseSha256 != io::sha256Hex(request.compilationDatabase))
        co_return fail(ErrorCode::InvalidArgument, "Rename response belongs to other compile commands");
    if (completed->signal != 0)
        co_return fail(ErrorCode::IoError, "Rename helper terminated by signal");
    if ((proposal->ready() && completed->exitCode != 0) || (!proposal->ready() && completed->exitCode != 1))
        co_return fail(ErrorCode::IoError, "Rename helper status and native exit disagree");
    co_return std::move(*proposal);
}
} // namespace frost::lang
