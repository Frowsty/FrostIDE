// SPDX-License-Identifier: Apache-2.0
//
// Finding git (frost/vcs/git_locator.hpp): the setting, the install directories and the PATH walk
// (done here, never by the OS: CLAUDE.md "absolute paths").

#include "frost/vcs/git_locator.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/vcs/vcs_status.hpp"

#include "environment.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace frost::vcs {
namespace {

#ifdef _WIN32
constexpr std::string_view kGitName = "git.exe";
constexpr char kPathListSeparator = ';';
#else
constexpr std::string_view kGitName = "git";
constexpr char kPathListSeparator = ':';
#endif

[[nodiscard]] bool isRegularFile(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}

// <directory>/<git name>, normalised; nullopt for a relative or malformed directory.
[[nodiscard]] std::optional<std::string> candidateIn(std::string_view directory) {
    std::string dir(directory);
    if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') {
        dir = dir.substr(1, dir.size() - 2);
    }
    if (dir.empty() || !io::isAbsolutePath(dir)) {
        return std::nullopt;
    }
    auto normalized = io::normalizePath(dir);
    if (!normalized) {
        return std::nullopt;
    }
    auto joined = io::joinPath(*normalized, kGitName);
    return joined ? std::optional<std::string>(std::move(*joined)) : std::nullopt;
}

// Compare canonical paths to reject PATH entries planted in a project, including symlinks.
[[nodiscard]] bool excluded(io::IFileSystem& fs, std::string_view candidate, const GitSearch& search) {
    const std::string path = vcsPathKey(fs.canonicalPath(candidate).value_or(std::string(candidate)));
    for (const std::string& directory : search.excludedDirectories) {
        std::string root = vcsPathKey(fs.canonicalPath(directory).value_or(directory));
        if (path == root)
            return true;
        if (!root.empty() && root.back() != '/' && root.back() != '\\') {
#ifdef _WIN32
            root += '\\';
#else
            root += '/';
#endif
        }
        if (!root.empty() && path.starts_with(root))
            return true;
    }
    return false;
}

} // namespace

std::string_view toString(GitOrigin origin) noexcept {
    switch (origin) {
    case GitOrigin::Setting: return "setting";
    case GitOrigin::InstallDirectory: return "install directory";
    case GitOrigin::Path: return "PATH";
    }
    return "PATH";
}

std::string configuredGitPath(const settings::SettingsSnapshot& settings) {
    return settings.get(settings::key::toolsGitPath);
}

GitSearch defaultGitSearch(std::string configuredPath) {
    GitSearch search;
    search.configuredPath = std::move(configuredPath);
#ifdef _WIN32
    const auto addUnder = [&](const char* variable, std::string_view suffix) {
        const std::string root = detail::environmentVariable(variable);
        if (root.empty()) {
            return;
        }
        if (auto dir = io::joinPath(root, suffix)) {
            if (std::ranges::find(search.installDirectories, *dir) == search.installDirectories.end()) {
                search.installDirectories.push_back(std::move(*dir));
            }
        }
    };
    addUnder("ProgramW6432", "Git\\cmd");
    addUnder("ProgramFiles", "Git\\cmd");
    addUnder("ProgramFiles(x86)", "Git\\cmd");
    addUnder("LOCALAPPDATA", "Programs\\Git\\cmd");
#else
    search.installDirectories = {"/usr/bin", "/usr/local/bin"};
#endif
    search.pathVariable = detail::environmentVariable("PATH");
    return search;
}

Result<GitLocation> findGit(io::IFileSystem& fs, const GitSearch& search) {
    if (!search.configuredPath.empty()) {
        if (!io::isAbsolutePath(search.configuredPath)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tools.git.path '{}' is not an absolute path", search.configuredPath));
        }
        auto normalized = io::normalizePath(search.configuredPath);
        if (!normalized || !isRegularFile(fs, *normalized)) {
            return fail(ErrorCode::NotFound, std::format("tools.git.path '{}' does not exist", search.configuredPath));
        }
        if (excluded(fs, *normalized, search))
            return fail(ErrorCode::PermissionDenied,
                        "The Git executable cannot be inside the open project or repository");
        return GitLocation{.path = std::move(*normalized), .origin = GitOrigin::Setting};
    }
    std::vector<std::string> tried;
    for (const std::string& directory : search.installDirectories) {
        if (auto candidate = candidateIn(directory)) {
            if (isRegularFile(fs, *candidate) && !excluded(fs, *candidate, search)) {
                return GitLocation{.path = std::move(*candidate), .origin = GitOrigin::InstallDirectory};
            }
            tried.push_back(std::move(*candidate));
        }
    }
    std::string_view rest = search.pathVariable;
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find(kPathListSeparator), rest.size());
        const std::string_view entry = rest.substr(0, end);
        rest = end < rest.size() ? rest.substr(end + 1) : std::string_view();
        if (auto candidate = candidateIn(entry)) {
            if (isRegularFile(fs, *candidate) && !excluded(fs, *candidate, search)) {
                return GitLocation{.path = std::move(*candidate), .origin = GitOrigin::Path};
            }
        }
    }
    std::string message = "git not found (install Git or set tools.git.path); tried";
    for (const std::string& path : tried) {
        message += ' ';
        message += path;
    }
    message += " and PATH";
    return fail(ErrorCode::NotFound, std::move(message));
}

} // namespace frost::vcs
