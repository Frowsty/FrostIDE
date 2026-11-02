// SPDX-License-Identifier: Apache-2.0
//
// Shell detection, the terminal's trust rule and its settings (frost/term/shell.hpp).

#include "frost/term/shell.hpp"

#include "frost/core/build_info.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"
#include "frost/term/pty.hpp"

#include <algorithm>
#include <utility>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>

#include <cstdlib>

#include <unistd.h>
#endif

namespace frost::term {

namespace {

#ifdef _WIN32
constexpr char kPathListSeparator = ';';

char foldAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameText(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return foldAscii(x) == foldAscii(y); });
}

bool isSeparator(char c) noexcept { return c == '\\' || c == '/'; }
#else
bool sameText(std::string_view a, std::string_view b) noexcept { return a == b; }

bool isSeparator(char c) noexcept { return c == '/'; }
#endif

#ifdef _WIN32
// `directory` joined with `name`, or "" when the result is not a valid path.
std::string join(std::string_view directory, std::string_view name) {
    auto joined = io::joinPath(directory, name);
    return joined ? std::move(*joined) : std::string();
}
#endif

std::optional<std::string> homeDirectory(const ShellProbe& probe) {
#ifdef _WIN32
    if (auto profile = probe.environment("USERPROFILE"); profile && io::isAbsolutePath(*profile)) {
        return profile;
    }
    auto drive = probe.environment("HOMEDRIVE");
    auto path = probe.environment("HOMEPATH");
    if (drive && path) {
        std::string combined = *drive + *path;
        if (io::isAbsolutePath(combined)) {
            return combined;
        }
    }
    return std::nullopt;
#else
    auto home = probe.environment("HOME");
    if (home && io::isAbsolutePath(*home)) {
        return home;
    }
    return std::nullopt;
#endif
}

} // namespace

bool isInsideRoot(std::string_view path, std::string_view root) {
    if (root.empty() || path.empty()) {
        return false;
    }
    auto normalizedPath = io::normalizePath(path);
    auto normalizedRoot = io::normalizePath(root);
    if (!normalizedPath || !normalizedRoot) {
        return false;
    }
    std::string_view p = *normalizedPath;
    std::string_view r = *normalizedRoot;
    while (r.size() > 1 && isSeparator(r.back())) {
        r.remove_suffix(1);
    }
    if (p.size() < r.size() || !sameText(p.substr(0, r.size()), r)) {
        return false;
    }
    return p.size() == r.size() || isSeparator(p[r.size()]) || isSeparator(r.back());
}

ShellProbe systemShellProbe() {
    ShellProbe probe;
#ifdef _WIN32
    probe.environment = [](std::string_view name) -> std::optional<std::string> {
        auto wideName = io::toWide(name);
        if (!wideName) {
            return std::nullopt;
        }
        const DWORD needed = GetEnvironmentVariableW(wideName->c_str(), nullptr, 0);
        if (needed == 0) {
            return std::nullopt;
        }
        std::wstring value(needed, L'\0');
        const DWORD length = GetEnvironmentVariableW(wideName->c_str(), value.data(), needed);
        if (length == 0 || length >= needed) {
            return std::nullopt;
        }
        value.resize(length);
        auto utf8 = io::fromWide(value);
        return utf8 ? std::optional<std::string>(std::move(*utf8)) : std::nullopt;
    };
    probe.isExecutable = [](const std::string& path) {
        auto wide = io::toWin32Path(path);
        if (!wide) {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(wide->c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    };
    std::wstring systemDir(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(systemDir.data(), static_cast<UINT>(systemDir.size()));
    if (length > 0 && length < systemDir.size()) {
        systemDir.resize(length);
        if (auto utf8 = io::fromWide(systemDir)) {
            probe.systemDirectory = std::move(*utf8);
        }
    }
#else
    probe.environment = [](std::string_view name) -> std::optional<std::string> {
        const std::string key(name);
        const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): FrostIDE never calls setenv
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
    };
    probe.isExecutable = [](const std::string& path) {
        struct stat info{};
        return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
    };
#endif
    return probe;
}

Result<ShellCommand> defaultShell(const ShellProbe& probe, std::string_view excludeRoot) {
    const auto usable = [&](const std::string& path) {
        return !path.empty() && io::isAbsolutePath(path) && !isInsideRoot(path, excludeRoot) && probe.isExecutable &&
               probe.isExecutable(path);
    };
#ifdef _WIN32
    // PowerShell 7: its installer's directory first, then PATH (winget, scoop and dotnet-tool installs).
    if (auto programFiles = probe.environment("ProgramFiles")) {
        if (const std::string pwsh = join(join(join(*programFiles, "PowerShell"), "7"), "pwsh.exe"); usable(pwsh)) {
            return ShellCommand{pwsh, {}};
        }
    }
    if (auto path = probe.environment("PATH")) {
        std::string_view rest = *path;
        while (!rest.empty()) {
            const std::size_t end = rest.find(kPathListSeparator);
            std::string_view entry = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            // Quoted entries are legal in PATH; relative ones ("." included) are never searched.
            if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
                entry = entry.substr(1, entry.size() - 2);
            }
            if (entry.empty() || !io::isAbsolutePath(entry)) {
                continue;
            }
            if (const std::string pwsh = join(entry, "pwsh.exe"); usable(pwsh)) {
                return ShellCommand{pwsh, {}};
            }
        }
    }
    if (!probe.systemDirectory.empty()) {
        const std::string windowsPowerShell =
            join(join(join(probe.systemDirectory, "WindowsPowerShell"), "v1.0"), "powershell.exe");
        if (usable(windowsPowerShell)) {
            return ShellCommand{windowsPowerShell, {}};
        }
        if (const std::string cmd = join(probe.systemDirectory, "cmd.exe"); usable(cmd)) {
            return ShellCommand{cmd, {}};
        }
    }
#else
    if (auto shell = probe.environment("SHELL"); shell && usable(*shell)) {
        return ShellCommand{*shell, {}};
    }
    for (const char* candidate : {"/bin/bash", "/bin/sh"}) {
        if (usable(candidate)) {
            return ShellCommand{candidate, {}};
        }
    }
#endif
    return fail(ErrorCode::NotFound, "no shell found for the terminal (set terminal.shell to its absolute path)");
}

TerminalSettings terminalSettings(const settings::SettingsSnapshot& snapshot) {
    TerminalSettings out;
    out.shell = snapshot.get(settings::key::terminalShell);
    out.scrollbackLines =
        static_cast<std::size_t>(std::max<std::int64_t>(snapshot.get(settings::key::terminalScrollbackLines), 0));
    return out;
}

Result<TerminalLaunch> planTerminalLaunch(const TerminalLaunchRequest& request, const ShellProbe& probe) {
    const bool restricted = !request.projectRoot.empty() && !request.trusted;
    ShellCommand shell;
    if (!request.configuredShell.empty()) {
        if (!io::isAbsolutePath(request.configuredShell)) {
            return fail(ErrorCode::InvalidArgument,
                        "terminal.shell must be an absolute path, got '" + request.configuredShell + "'");
        }
        shell.executable = request.configuredShell;
    } else {
        auto detected = defaultShell(probe, request.projectRoot);
        if (!detected) {
            return std::unexpected(std::move(detected.error()));
        }
        shell = std::move(*detected);
    }
    if (!request.workingDirectory.empty() && !io::isAbsolutePath(request.workingDirectory)) {
        return fail(ErrorCode::InvalidArgument,
                    "the terminal's folder must be an absolute path, got '" + request.workingDirectory + "'");
    }

    const std::optional<std::string> home = homeDirectory(probe);
    std::string directory;
    if (restricted) {
        if (!home) {
            return fail(ErrorCode::NotFound, "no home directory for a terminal in an untrusted project");
        }
        directory = *home;
    } else if (!request.workingDirectory.empty()) {
        directory = request.workingDirectory;
    } else if (!request.projectRoot.empty()) {
        directory = request.projectRoot;
    } else if (home) {
        directory = *home;
    }

    TerminalLaunch launch;
    launch.restricted = restricted;
    launch.spec = ptyProcessSpec(std::move(shell.executable), std::move(shell.arguments));
    launch.spec.workingDirectory = std::move(directory);
    auto& env = launch.spec.environment;
    env.emplace_back("TERM", "xterm-256color");
    env.emplace_back("COLORTERM", "truecolor");
    env.emplace_back("TERM_PROGRAM", "FrostIDE");
    env.emplace_back("TERM_PROGRAM_VERSION", std::string(build_info::kVersion));
#ifdef _WIN32
    // ADR 0011 E2: restricted children keep the hardened search, trusted user shells get cmd.exe's usual one.
    if (restricted) {
        env.emplace_back("NoDefaultCurrentDirectoryInExePath", "1");
    } else {
        env.emplace_back("NoDefaultCurrentDirectoryInExePath", std::nullopt);
    }
#endif
    return launch;
}

} // namespace frost::term
