// SPDX-License-Identifier: Apache-2.0
//
// Finding clangd and building its command line (clangd.hpp): the search order, the PATH walk
// (done here, never by the OS: CLAUDE.md "absolute paths"), the arguments and the restart backoff.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/store.hpp"

#include <algorithm>
#include <format>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>

#include <unistd.h>
#endif

#ifndef FROST_PINNED_LLVM_VERSION
#error "FROST_PINNED_LLVM_VERSION comes from src/lang/CMakeLists.txt (sdk/sdk.lock.json)"
#endif

namespace frost::lang {
namespace {

#ifdef _WIN32
constexpr std::string_view kClangdName = "clangd.exe";
constexpr char kPathListSeparator = ';';
#else
constexpr std::string_view kClangdName = "clangd";
constexpr char kPathListSeparator = ':';
#endif

// The value of an environment variable (UTF-8), or "" when unset.
[[nodiscard]] std::string environment(const char* name) {
#ifdef _WIN32
    auto wide = io::toWide(name);
    if (!wide) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableW(wide->c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide->c_str(), value.data(), needed);
    if (length == 0 || length >= needed) {
        return {};
    }
    value.resize(length);
    auto utf8 = io::fromWide(value);
    return utf8 ? std::move(*utf8) : std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

[[nodiscard]] bool isRegularFile(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    return status && status->type == io::FileType::Regular;
}

// <directory>/<clangd name>, normalised; nullopt for a relative or malformed directory.
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
    auto joined = io::joinPath(*normalized, kClangdName);
    return joined ? std::optional<std::string>(std::move(*joined)) : std::nullopt;
}

[[nodiscard]] std::string llvmBin(std::string_view sdkRoot) {
    auto joined = io::joinPath(sdkRoot, std::format("llvm-{}", pinnedLlvmVersion()));
    if (!joined) {
        return {};
    }
    auto bin = io::joinPath(*joined, "bin");
    return bin ? std::move(*bin) : std::string();
}

[[nodiscard]] std::string executableDirectory() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    auto utf8 = io::fromWide(buffer);
    return utf8 ? std::string(io::parentPath(*utf8)) : std::string();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(length));
    return std::string(io::parentPath(buffer));
#else
    return {};
#endif
}

} // namespace

std::string_view toString(ClangdOrigin origin) noexcept {
    switch (origin) {
    case ClangdOrigin::Setting: return "setting";
    case ClangdOrigin::PinnedSdk: return "pinned SDK";
    case ClangdOrigin::Path: return "PATH";
    case ClangdOrigin::BundledFrost: return "bundled frost-clang";
    }
    return "PATH";
}

std::string_view pinnedLlvmVersion() noexcept { return FROST_PINNED_LLVM_VERSION; }

std::string configuredClangdPath(const settings::SettingsSnapshot& settings) {
    return settings.get(settings::key::toolsClangdPath);
}

ClangdSearch defaultClangdSearch(std::string configuredPath) {
    ClangdSearch search;
    search.configuredPath = std::move(configuredPath);
    // The clangd bundled beside this executable first (tools/stage.ps1 and tools/stage.sh stage
    // llvm-<pinned>/bin), then the pinned SDK of FROST_SDK and FROST_LLVM_ROOT (tools/env.ps1 and
    // ~/sdk/env.sh set them for development and tests), then PATH. No machine-specific default
    // root (FND-10): the product never guesses a developer's drive.
    if (const std::string exeDir = executableDirectory(); !exeDir.empty()) {
        if (auto bundled = io::joinPath(exeDir,
#ifdef _WIN32
                                        "frost-clang.exe"
#else
                                        "frost-clang"
#endif
                                        ))
            search.bundledFrostPath = std::move(*bundled);
        if (std::string bundledPinned = llvmBin(exeDir); !bundledPinned.empty()) {
            search.sdkDirectories.push_back(std::move(bundledPinned));
        }
        if (auto bundledLlvm = io::joinPath(exeDir, "llvm/bin")) {
            search.sdkDirectories.push_back(std::move(*bundledLlvm));
        }
        if (auto bundledBin = io::joinPath(exeDir, "bin")) {
            search.sdkDirectories.push_back(std::move(*bundledBin));
        }
        search.sdkDirectories.push_back(exeDir);
    }
    if (const std::string sdk = environment("FROST_SDK"); !sdk.empty()) {
        if (std::string pinned = llvmBin(sdk); !pinned.empty()) {
            search.sdkDirectories.push_back(std::move(pinned));
        }
    }
    if (const std::string llvmRoot = environment("FROST_LLVM_ROOT"); !llvmRoot.empty()) {
        if (auto bin = io::joinPath(llvmRoot, "bin")) {
            search.sdkDirectories.push_back(std::move(*bin));
        }
    }
    search.pathVariable = environment("PATH");
    return search;
}

Result<ClangdLocation> findClangd(io::IFileSystem& fs, const ClangdSearch& search) {
    if (!search.configuredPath.empty()) {
        if (!io::isAbsolutePath(search.configuredPath)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("tools.clangd.path '{}' is not an absolute path", search.configuredPath));
        }
        auto normalized = io::normalizePath(search.configuredPath);
        if (!normalized || !isRegularFile(fs, *normalized)) {
            return fail(ErrorCode::NotFound,
                        std::format("tools.clangd.path '{}' does not exist", search.configuredPath));
        }
        return ClangdLocation{.path = std::move(*normalized), .origin = ClangdOrigin::Setting};
    }
    std::vector<std::string> tried;
    if (!search.bundledFrostPath.empty()) {
        auto bundled = io::normalizePath(search.bundledFrostPath);
        if (bundled && io::isAbsolutePath(*bundled) && isRegularFile(fs, *bundled))
            return ClangdLocation{.path = std::move(*bundled), .origin = ClangdOrigin::BundledFrost};
        tried.push_back(search.bundledFrostPath);
    }
    for (const std::string& directory : search.sdkDirectories) {
        if (auto candidate = candidateIn(directory)) {
            if (isRegularFile(fs, *candidate)) {
                return ClangdLocation{.path = std::move(*candidate), .origin = ClangdOrigin::PinnedSdk};
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
            if (isRegularFile(fs, *candidate)) {
                return ClangdLocation{.path = std::move(*candidate), .origin = ClangdOrigin::Path};
            }
        }
    }
    std::string message = "clangd not found (set tools.clangd.path); tried";
    for (const std::string& path : tried) {
        message += ' ';
        message += path;
    }
    message += " and PATH";
    return fail(ErrorCode::NotFound, std::move(message));
}

unsigned defaultClangdJobs(unsigned hardwareThreads) noexcept { return std::clamp(hardwareThreads / 2, 1U, 8U); }

unsigned defaultClangdJobs() noexcept { return defaultClangdJobs(std::thread::hardware_concurrency()); }

std::vector<std::string> clangdArguments(const ClangdLaunchOptions& options,
                                         const std::optional<CompileCommandsLocation>& compileCommands) {
    std::vector<std::string> arguments;
    arguments.emplace_back(options.backgroundIndex ? "--background-index" : "--background-index=false");
    arguments.emplace_back("--header-insertion=never");
    arguments.push_back("--log=" + (options.logLevel.empty() ? std::string("error") : options.logLevel));
    arguments.emplace_back("--pch-storage=memory");
    arguments.push_back(std::format("-j={}", options.jobs != 0 ? options.jobs : defaultClangdJobs()));
    // Never cut a rename at clangd's default of 50 files (clangd.hpp, "Trust").
    arguments.emplace_back("--rename-file-limit=0");
    if (options.trusted) {
        arguments.emplace_back("--limit-references=0");
        arguments.emplace_back("--limit-results=1000");
    } else {
        arguments.emplace_back("--enable-config=false");
    }
    if (compileCommands) {
        arguments.push_back("--compile-commands-dir=" + compileCommands->directory);
    }
    for (const std::string& extra : options.extraArguments) {
        // Restricted mode: nothing may re-enable project config or name a driver to execute.
        if (!options.trusted && (extra.starts_with("--query-driver") || extra.starts_with("-query-driver") ||
                                 extra.starts_with("--enable-config") || extra.starts_with("-enable-config"))) {
            continue;
        }
        arguments.push_back(extra);
    }
    return arguments;
}

IClock::Duration restartDelay(const RestartPolicy& policy, unsigned crash) noexcept {
    IClock::Duration delay = policy.initialDelay;
    for (unsigned i = 1; i < crash && delay < policy.maxDelay; ++i) {
        delay *= 2;
    }
    return std::min(delay, policy.maxDelay);
}

std::string_view toString(ClangdState state) noexcept {
    switch (state) {
    case ClangdState::Stopped: return "stopped";
    case ClangdState::Starting: return "starting";
    case ClangdState::Running: return "running";
    case ClangdState::Restarting: return "restarting";
    case ClangdState::Failed: return "failed";
    }
    return "stopped";
}

} // namespace frost::lang
