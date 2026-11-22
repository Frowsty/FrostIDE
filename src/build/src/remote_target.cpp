// SPDX-License-Identifier: Apache-2.0
// Explicit endpoint validation, exact mapping and static transport bootstrap argv for R19.
#include "frost/build/remote_target.hpp"

#include "frost/rpc/json.hpp"

#include <algorithm>
#include <cctype>
#include <set>
namespace frost::build {
namespace {
bool clean(std::string_view text) {
    return std::ranges::none_of(text, [](unsigned char c) { return c < 32 || c == 127; });
}
bool identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 && std::ranges::all_of(text, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}
bool hasDotComponent(std::string_view path, bool windows) {
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/' && !(windows && path[i] == '\\'))
            continue;
        const auto part = path.substr(begin, i - begin);
        if (part == "." || part == "..")
            return true;
        begin = i + 1;
    }
    return false;
}
std::string localKey(std::string_view path) {
    auto result = io::normalizePath(path).value_or(std::string(path));
    if (io::kNativePathStyle == io::PathStyle::Windows)
        for (auto& c : result) {
            if (c == '\\')
                c = '/';
            else
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    return result;
}
bool within(std::string_view path, std::string_view root) {
    return path == root || (path.starts_with(root) && !root.empty() &&
                            (root.back() == '/' || (path.size() > root.size() && path[root.size()] == '/')));
}
std::string posixQuote(std::string_view text) {
    std::string result = "'";
    for (char c : text) {
        if (c == '\'')
            result += "'\\''";
        else
            result += c;
    }
    return result + "'";
}
} // namespace
Result<void> validateRemoteTarget(const RemoteTarget& target) {
    if (!identifier(target.id) || target.name.empty() || !clean(target.name) || target.name.size() > 256)
        return fail(ErrorCode::InvalidArgument, "Remote target needs a stable id and nonempty display name");
    if (!io::isAbsolutePath(target.transportExecutable) || !clean(target.transportExecutable))
        return fail(ErrorCode::InvalidArgument, "Remote transport must be an explicit absolute local executable");
    if (!io::isAbsolutePath(target.remotePython, io::PathStyle::Posix) || !clean(target.remotePython))
        return fail(ErrorCode::InvalidArgument, "Remote Linux Python path must be absolute");
    if (target.transport == RemoteTransport::Wsl) {
        if (target.distribution.empty() || target.distribution.starts_with('-') || !clean(target.distribution) ||
            target.distribution.size() > 256 || !target.host.empty() || !target.user.empty() ||
            !target.identityFile.empty())
            return fail(ErrorCode::InvalidArgument, "WSL target needs one explicit distribution and no SSH options");
    } else {
        if (target.host.empty() || target.host.starts_with('-') || target.host.size() > 256 ||
            !std::ranges::all_of(target.host,
                                 [](unsigned char c) {
                                     return std::isalnum(c) || std::string_view(".-:[]").find(static_cast<char>(c)) !=
                                                                   std::string_view::npos;
                                 }) ||
            !identifier(target.user) || target.user.starts_with('-') || target.port == 0 || target.port > 65535 ||
            !target.distribution.empty())
            return fail(ErrorCode::InvalidArgument,
                        "SSH target needs an explicit Linux host/user/port and no WSL distribution");
        if (!target.identityFile.empty() && (!io::isAbsolutePath(target.identityFile) || !clean(target.identityFile)))
            return fail(ErrorCode::InvalidArgument, "SSH identity path must be an explicit absolute local file");
    }
    if (target.mappings.size() > 64)
        return fail(ErrorCode::OutOfRange, "Remote target has too many path mappings");
    for (std::size_t i = 0; i < target.mappings.size(); ++i) {
        const auto& mapping = target.mappings[i];
        if (!io::isAbsolutePath(mapping.localRoot) || !io::isAbsolutePath(mapping.remoteRoot, io::PathStyle::Posix) ||
            !clean(mapping.localRoot) || !clean(mapping.remoteRoot) ||
            hasDotComponent(mapping.localRoot, io::kNativePathStyle == io::PathStyle::Windows) ||
            hasDotComponent(mapping.remoteRoot, false))
            return fail(ErrorCode::InvalidArgument,
                        "Local/remote mapping roots must be absolute and free of control characters");
        const auto local = localKey(mapping.localRoot);
        auto remote = io::normalizePath(mapping.remoteRoot, io::PathStyle::Posix);
        if (!remote)
            return std::unexpected(remote.error());
        for (std::size_t j = 0; j < i; ++j) {
            const auto otherLocal = localKey(target.mappings[j].localRoot);
            const auto otherRemote = io::normalizePath(target.mappings[j].remoteRoot, io::PathStyle::Posix).value();
            if (within(local, otherLocal) || within(otherLocal, local) || within(*remote, otherRemote) ||
                within(otherRemote, *remote))
                return fail(ErrorCode::InvalidArgument, "Remote mappings overlap or have an ambiguous inverse");
        }
    }
    return {};
}
Result<std::string> mapRemoteToLocal(const RemoteTarget& target, std::string_view value) {
    if (auto valid = validateRemoteTarget(target); !valid)
        return std::unexpected(valid.error());
    if (hasDotComponent(value, false))
        return fail(ErrorCode::InvalidArgument, "Remote source mapping refuses dot traversal components");
    auto path = io::normalizePath(value, io::PathStyle::Posix);
    if (!path)
        return std::unexpected(path.error());
    for (const auto& mapping : target.mappings) {
        const auto root = io::normalizePath(mapping.remoteRoot, io::PathStyle::Posix).value();
        if (!within(*path, root))
            continue;
        auto suffix = path->substr(root.size());
        while (suffix.starts_with('/'))
            suffix.erase(0, 1);
        if (io::kNativePathStyle == io::PathStyle::Windows && suffix.find_first_of("\\:") != suffix.npos)
            return fail(ErrorCode::Unsupported,
                        "Remote filename cannot be represented in the Windows source namespace");
        auto mapped = suffix.empty() ? io::normalizePath(mapping.localRoot) : io::joinPath(mapping.localRoot, suffix);
        if (!mapped)
            return std::unexpected(mapped.error());
        if (!within(localKey(*mapped), localKey(mapping.localRoot)))
            return fail(ErrorCode::InvalidArgument, "Remote source mapping escaped its explicit local root");
        auto reversed = mapLocalToRemote(target, *mapped);
        if (!reversed || *reversed != *path)
            return fail(ErrorCode::Unsupported, "Remote source path has no exact reversible local representation");
        return mapped;
    }
    return fail(ErrorCode::NotFound, "Remote path has no explicit local source mapping: " + std::string(value));
}
Result<std::string> mapLocalToRemote(const RemoteTarget& target, std::string_view value) {
    if (auto valid = validateRemoteTarget(target); !valid)
        return std::unexpected(valid.error());
    if (hasDotComponent(value, io::kNativePathStyle == io::PathStyle::Windows))
        return fail(ErrorCode::InvalidArgument, "Local source mapping refuses dot traversal components");
    auto normalized = io::normalizePath(value);
    if (!normalized)
        return std::unexpected(normalized.error());
    const auto path = localKey(*normalized);
    for (const auto& mapping : target.mappings) {
        const auto root = localKey(mapping.localRoot);
        if (!within(path, root))
            continue;
        // Preserve the original path's character case; only comparison uses a Windows key.
        auto original = *normalized;
        for (auto& c : original)
            if (c == '\\')
                c = '/';
        auto suffix = original.substr(root.size());
        while (suffix.starts_with('/'))
            suffix.erase(0, 1);
        return suffix.empty() ? io::normalizePath(mapping.remoteRoot, io::PathStyle::Posix)
                              : io::joinPath(mapping.remoteRoot, suffix, io::PathStyle::Posix);
    }
    return fail(ErrorCode::NotFound, "Local path has no explicit remote mapping: " + std::string(value));
}
Result<std::vector<RemoteTarget>> parseRemoteTargets(std::string_view bytes) {
    if (bytes.size() > 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Remote target registry exceeds 1 MiB");
    auto parsed = rpc::parseJson(bytes);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* targets = parsed->find("targets");
    if (parsed->integerAt("version") != 1 || !targets || !targets->isArray() || targets->size() > 128)
        return fail(ErrorCode::Corrupt, "Unsupported or malformed application remote target registry");
    std::vector<RemoteTarget> result;
    std::set<std::string> ids;
    for (const auto& row : targets->items()) {
        RemoteTarget target;
        const auto kind = row.stringAt("transport");
        if (kind != "wsl" && kind != "ssh")
            return fail(ErrorCode::Corrupt, "Unknown remote transport");
        target.transport = kind == "wsl" ? RemoteTransport::Wsl : RemoteTransport::Ssh;
        for (auto [name, field] : {std::pair{"id", &target.id},
                                   {"name", &target.name},
                                   {"executable", &target.transportExecutable},
                                   {"distribution", &target.distribution},
                                   {"host", &target.host},
                                   {"user", &target.user},
                                   {"identityFile", &target.identityFile},
                                   {"python", &target.remotePython}}) {
            const auto* member = row.find(name);
            if (!member || !member->asString())
                return fail(ErrorCode::Corrupt, "Missing/wrong-typed remote target field: " + std::string(name));
            *field = *member->asString();
        }
        const auto* approved = row.find("executionApproved");
        auto port = row.integerAt("port");
        const auto* mappings = row.find("mappings");
        if (!approved || !approved->asBool() || !port || *port <= 0 || *port > 65535 || !mappings ||
            !mappings->isArray())
            return fail(ErrorCode::Corrupt, "Malformed remote endpoint approval, port or path mappings");
        target.executionApproved = *approved->asBool();
        target.port = static_cast<unsigned>(*port);
        for (const auto& mapping : mappings->items()) {
            const auto* local = mapping.find("local");
            const auto* remote = mapping.find("remote");
            if (!local || !local->asString() || !remote || !remote->asString())
                return fail(ErrorCode::Corrupt, "Malformed remote path mapping");
            target.mappings.push_back({*local->asString(), *remote->asString()});
        }
        if (auto valid = validateRemoteTarget(target); !valid)
            return std::unexpected(valid.error());
        if (!ids.insert(target.id).second)
            return fail(ErrorCode::Corrupt, "Duplicate remote target id");
        result.push_back(std::move(target));
    }
    return result;
}
std::string remoteTargetsJson(const std::vector<RemoteTarget>& targets) {
    auto rows = rpc::Json::array();
    for (const auto& target : targets) {
        auto mappings = rpc::Json::array();
        for (const auto& mapping : target.mappings)
            mappings.push(rpc::Json::object().set("local", mapping.localRoot).set("remote", mapping.remoteRoot));
        rows.push(rpc::Json::object()
                      .set("id", target.id)
                      .set("name", target.name)
                      .set("transport", target.transport == RemoteTransport::Wsl ? "wsl" : "ssh")
                      .set("executable", target.transportExecutable)
                      .set("distribution", target.distribution)
                      .set("host", target.host)
                      .set("user", target.user)
                      .set("port", target.port)
                      .set("identityFile", target.identityFile)
                      .set("python", target.remotePython)
                      .set("executionApproved", target.executionApproved)
                      .set("mappings", std::move(mappings)));
    }
    return rpc::Json::object().set("version", 1).set("targets", std::move(rows)).dump();
}
Result<io::ProcessSpec> remoteTransportSpec(const RemoteTarget& target, std::string_view guardScript) {
    if (auto valid = validateRemoteTarget(target); !valid)
        return std::unexpected(valid.error());
    if (!target.executionApproved)
        return fail(ErrorCode::Untrusted, "Named remote target has no explicit application approval");
    if (guardScript.empty() || guardScript.size() > 64U * 1024U)
        return fail(ErrorCode::InvalidArgument, "Remote guard bootstrap is missing or oversized");
    io::ProcessSpec spec;
    spec.executable = target.transportExecutable;
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    if (target.transport == RemoteTransport::Wsl) {
        spec.arguments = {"--distribution",        target.distribution, "--exec", target.remotePython, "-I", "-u", "-c",
                          std::string(guardScript)};
    } else {
        spec.arguments = {"-T",
                          "-F",
                          "none",
                          "-o",
                          "BatchMode=yes",
                          "-o",
                          "StrictHostKeyChecking=yes",
                          "-o",
                          "PermitLocalCommand=no",
                          "-o",
                          "ClearAllForwardings=yes",
                          "-o",
                          "ControlMaster=no",
                          "-o",
                          "ControlPath=none",
                          "-o",
                          "ConnectTimeout=10",
                          "-o",
                          "ServerAliveInterval=3",
                          "-o",
                          "ServerAliveCountMax=2",
                          "-p",
                          std::to_string(target.port),
                          "-l",
                          target.user};
        if (!target.identityFile.empty()) {
            spec.arguments.emplace_back("-i");
            spec.arguments.push_back(target.identityFile);
        }
        spec.arguments.push_back(target.host);
        spec.arguments.push_back("exec " + posixQuote(target.remotePython) + " -I -u -c " + posixQuote(guardScript));
    }
    return spec;
}
Result<void> validateRemoteWorkload(const io::ProcessSpec& workload) {
    if (!io::isAbsolutePath(workload.executable, io::PathStyle::Posix) || !clean(workload.executable) ||
        !io::isAbsolutePath(workload.workingDirectory, io::PathStyle::Posix) || !clean(workload.workingDirectory) ||
        !workload.killOnClose || workload.rawArguments)
        return fail(ErrorCode::InvalidArgument,
                    "Remote workload needs absolute Linux program/cwd, structured argv and owned-tree lifetime");
    if (workload.residentMemoryLimitBytes || workload.pseudoTerminal || workload.newConsole ||
        !workload.desktop.empty() || !workload.posixInheritedFds.empty() ||
        workload.imagePathForm != io::ImagePathForm::Auto)
        return fail(ErrorCode::Unsupported,
                    "Remote execution cannot silently drop native memory, terminal, desktop or descriptor policies");
    if (workload.arguments.size() > 4096)
        return fail(ErrorCode::OutOfRange, "Remote workload argv exceeds budget");
    for (const auto& arg : workload.arguments)
        if (arg.find('\0') != arg.npos)
            return fail(ErrorCode::InvalidArgument, "Remote argument holds NUL");
    for (const auto& [name, value] : workload.environment)
        if (name.empty() || name.find_first_of("=\0", 0, 2) != name.npos || (value && value->find('\0') != value->npos))
            return fail(ErrorCode::InvalidArgument, "Remote environment entry is invalid");
    return {};
}
} // namespace frost::build
