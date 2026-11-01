// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics directory layout and the cache-root rules (frost/diag/paths.hpp), plus the
// directory preparation for both platforms.

#include "frost/diag/paths.hpp"

#include "frost/io/file_system.hpp"

#include <format>
#include <utility>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace frost::diag {
namespace {

// A drive-absolute ("C:\...") or UNC ("\\server\share\...") path; also the \\?\ forms of both.
bool isDriveOrUncPath(std::string_view path) noexcept {
    const auto separator = [](char c) { return c == '\\' || c == '/'; };
    if (path.size() >= 3 && path[1] == ':' && separator(path[2])) {
        const char drive = path[0];
        return (drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z');
    }
    if (path.size() >= 4 && separator(path[0]) && separator(path[1])) {
        // \\.\ is the device namespace; \\?\ is accepted only around a drive or UNC path.
        if (path[2] == '.' && separator(path[3])) {
            return false;
        }
        if (path[2] == '?' && separator(path[3])) {
            const std::string_view rest = path.substr(4);
            return rest.starts_with("UNC\\") || rest.starts_with("UNC/") || isDriveOrUncPath(rest);
        }
        return true;
    }
    return false;
}

Result<std::string> appendTo(std::string_view base, std::string_view name, io::PathStyle style) {
    auto normalized = io::normalizePath(base, style);
    if (!normalized) {
        return normalized;
    }
    return io::joinPath(*normalized, name, style);
}

} // namespace

Result<std::string> resolveCacheRoot(const EnvironmentLookup& lookup, io::PathStyle style) {
    if (style == io::PathStyle::Windows) {
        const std::optional<std::string> local = lookup("LOCALAPPDATA");
        if (!local || local->empty()) {
            return fail(ErrorCode::NotFound, "LOCALAPPDATA is not set");
        }
        if (local->find('\0') != std::string::npos || !isDriveOrUncPath(*local)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("LOCALAPPDATA is not an absolute drive or UNC path: '{}'", *local));
        }
        return appendTo(*local, "FrostIDE\\caches", style);
    }
    const std::optional<std::string> xdg = lookup("XDG_CACHE_HOME");
    if (xdg && !xdg->empty() && xdg->find('\0') == std::string::npos && io::isAbsolutePath(*xdg, style)) {
        return appendTo(*xdg, "FrostIDE", style);
    }
    const std::optional<std::string> home = lookup("HOME");
    if (!home || home->empty()) {
        return fail(ErrorCode::NotFound, "neither XDG_CACHE_HOME nor HOME is set");
    }
    if (home->find('\0') != std::string::npos || !io::isAbsolutePath(*home, style)) {
        return fail(ErrorCode::InvalidArgument, std::format("HOME is not an absolute path: '{}'", *home));
    }
    return appendTo(*home, ".cache/FrostIDE", style);
}

Result<std::string> resolveCacheRoot() {
    const EnvironmentLookup lookup = [](std::string_view name) -> std::optional<std::string> {
#ifdef _WIN32
        auto wideName = io::toWide(name);
        if (!wideName) {
            return std::nullopt;
        }
        const DWORD needed = GetEnvironmentVariableW(wideName->c_str(), nullptr, 0);
        if (needed == 0) {
            return std::nullopt;
        }
        std::wstring value(needed, L'\0');
        const DWORD got = GetEnvironmentVariableW(wideName->c_str(), value.data(), needed);
        if (got == 0 || got >= needed) {
            return std::nullopt;
        }
        value.resize(got);
        auto utf8 = io::fromWide(value);
        return utf8 ? std::optional<std::string>(std::move(*utf8)) : std::nullopt;
#else
        const std::string key(name);
        const char* value = std::getenv(key.c_str()); // Only start-up code reads the environment.
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
    };
    return resolveCacheRoot(lookup, io::kNativePathStyle);
}

Result<DiagnosticsDirectories> diagnosticsDirectories(std::string_view cacheRoot, io::PathStyle style) {
    auto root = io::normalizePath(cacheRoot, style);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    if (!io::isAbsolutePath(*root, style)) {
        return fail(ErrorCode::InvalidArgument, std::format("cache root is not absolute: '{}'", *root));
    }
    DiagnosticsDirectories directories;
    directories.cacheRoot = *root;
    const auto join = [&](std::string_view base, std::string_view name, std::string& out) -> Result<void> {
        auto joined = io::joinPath(base, name, style);
        if (!joined) {
            return std::unexpected(std::move(joined.error()));
        }
        out = std::move(*joined);
        return {};
    };
    for (auto [base, name, out] : {std::tuple{&directories.cacheRoot, "crashes", &directories.crashes},
                                   std::tuple{&directories.crashes, "crashpad", &directories.crashpadDatabase},
                                   std::tuple{&directories.crashes, "freezes", &directories.freezes},
                                   std::tuple{&directories.cacheRoot, "logs", &directories.logs},
                                   std::tuple{&directories.cacheRoot, "traces", &directories.traces}}) {
        if (auto joined = join(*base, name, *out); !joined) {
            return std::unexpected(std::move(joined.error()));
        }
    }
    return directories;
}

#ifdef _WIN32

Result<void> prepareDiagnosticsDirectories(const DiagnosticsDirectories& directories) {
    io::IFileSystem& fs = io::realFileSystem();
    for (const std::string* path : {&directories.crashpadDatabase, &directories.freezes, &directories.logs}) {
        if (auto created = fs.createDirectories(*path); !created) {
            return std::unexpected(std::move(created.error()).withContext("diagnostics directory"));
        }
    }
    return {};
}

#else

namespace {

// Creates `path` with mode 0700 if missing, then checks it (see paths.hpp).
Result<void> preparePrivateDirectory(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        const int error = errno;
        return fail(error == EACCES ? ErrorCode::PermissionDenied : ErrorCode::IoError,
                    std::format("mkdir '{}': {}", path, std::strerror(error)));
    }
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0) {
        return fail(ErrorCode::IoError, std::format("lstat '{}': {}", path, std::strerror(errno)));
    }
    if (!S_ISDIR(info.st_mode)) {
        return fail(ErrorCode::AlreadyExists, std::format("'{}' is not a directory (or is a symlink)", path));
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        if (info.st_uid != ::geteuid()) {
            return fail(ErrorCode::PermissionDenied,
                        std::format("'{}' is writable by others and owned by uid {}", path, info.st_uid));
        }
        if (::chmod(path.c_str(), 0700) != 0) {
            return fail(ErrorCode::PermissionDenied, std::format("chmod 0700 '{}': {}", path, std::strerror(errno)));
        }
    }
    return {};
}

} // namespace

Result<void> prepareDiagnosticsDirectories(const DiagnosticsDirectories& directories) {
    io::IFileSystem& fs = io::realFileSystem();
    if (auto created = fs.createDirectories(directories.cacheRoot); !created) {
        return std::unexpected(std::move(created.error()).withContext("cache root"));
    }
    for (const std::string* path : {&directories.crashes, &directories.logs}) {
        if (auto prepared = preparePrivateDirectory(*path); !prepared) {
            return prepared;
        }
    }
    for (const std::string* path : {&directories.crashpadDatabase, &directories.freezes}) {
        if (auto prepared = preparePrivateDirectory(*path); !prepared) {
            return prepared;
        }
    }
    return {};
}

#endif

} // namespace frost::diag
