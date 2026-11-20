// SPDX-License-Identifier: Apache-2.0
#include "frost/io/process_sandbox.hpp"
#include "frost/io/utf.hpp"
namespace frost::io {
namespace {
std::string key(std::string_view path, PathStyle style) {
    auto normalized = normalizePath(path, style);
    if (!normalized) return {};
    if (style == PathStyle::Windows) for (auto& c : *normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return *normalized;
}
}
bool sandboxAllowsPath(const ProcessSandboxProfile& profile, std::string_view path, SandboxAccess access, PathStyle style) {
    const auto target = key(path, style); if (target.empty()) return false;
    if (target == ".." || target.find("/../") != target.npos || target.ends_with("/..") ||
        target.find("\\..\\") != target.npos || target.ends_with("\\..")) return false;
    for (const auto& grant : profile.paths) {
        const auto root = key(grant.path, style);
        if (!hasSandboxAccess(grant.access, access) || root.empty()) continue;
        if (target == root || (grant.recursive && target.size() > root.size() && target.starts_with(root) &&
            (root.back() == '/' || root.back() == '\\' || target[root.size()] == '/' || target[root.size()] == '\\'))) return true;
    }
    return false;
}
Result<void> validateProcessSandbox(const ProcessSpec& spec, bool windowsRules, bool prepared) {
    if (!spec.sandbox) return {};
    const auto& profile = *spec.sandbox;
    if (!spec.killOnClose || !spec.cleanEnvironment || spec.pseudoTerminal || spec.newConsole || spec.rawArguments ||
        !spec.desktop.empty() || !spec.posixInheritedFds.empty() || spec.stdinMode == Stdio::Inherit ||
        spec.stdoutMode == Stdio::Inherit || spec.stderrMode == Stdio::Inherit)
        return fail(ErrorCode::InvalidArgument, "Sandbox requires a clean environment, stdio pipes/null, tree containment and no extra handles/console/raw arguments");
    if (profile.paths.empty() || profile.paths.size() > 256 ||
        (profile.network != SandboxNetwork::Deny && profile.network != SandboxNetwork::InternetClient))
        return fail(ErrorCode::InvalidArgument, "Sandbox profile has invalid grant count or network policy");
    const auto style = windowsRules ? PathStyle::Windows : PathStyle::Posix;
    for (const auto& grant : profile.paths) {
        if (grant.path.size() > 4096 || grant.path.find('\0') != grant.path.npos || !isValidUtf8(grant.path) ||
            !isAbsolutePath(grant.path, style) || !normalizePath(grant.path, style) ||
            grant.path.find("/../") != grant.path.npos || grant.path.ends_with("/..") || grant.path.find("\\..\\") != grant.path.npos || grant.path.ends_with("\\..") ||
            static_cast<unsigned>(grant.access) == 0 || static_cast<unsigned>(grant.access) > 7)
            return fail(ErrorCode::InvalidArgument, "Sandbox grants require bounded absolute UTF-8 paths and explicit known rights");
        if (windowsRules && prepared) {
            ProcessSandboxProfile stage; stage.paths.push_back({profile.windowsStagingRoot, grant.access, true});
            if (profile.windowsStagingRoot.empty() || !sandboxAllowsPath(stage, grant.path, grant.access, style))
                return fail(ErrorCode::InvalidArgument, "Windows sandbox grants must name private staged content, never original workspace/tool ACLs");
        }
    }
    if (!sandboxAllowsPath(profile, spec.executable, SandboxAccess::Read | SandboxAccess::Execute, style))
        return fail(ErrorCode::PermissionDenied, "Sandbox executable needs an explicit read/execute grant");
    if (windowsRules && prepared && (profile.windowsAppContainerName.empty() || profile.windowsAppContainerName.size() > 128 ||
        profile.windowsAppContainerName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos))
        return fail(ErrorCode::InvalidArgument, "Windows sandbox needs an owned staged AppContainer identity");
    return {};
}
} // namespace frost::io
