// SPDX-License-Identifier: Apache-2.0
//
// Path rules for frost::io (see frost/io/path.hpp). Everything here is a pure
// string transformation; nothing touches the file system.

#include "frost/io/path.hpp"

#include "frost/io/utf.hpp"

#include <cstddef>
#include <vector>

namespace frost::io {
namespace {

enum class RootKind : unsigned char {
    None,     // Relative (including the Windows "C:x" and "\x" forms).
    Invalid,  // Starts like an absolute path but is malformed ("\\server", "//?/x").
    Posix,    // "/"
    Drive,    // "C:\"
    Unc,      // "\\server\share\"
    Verbatim, // "\\?\..."
    Device,   // "\\.\..."
};

struct Root {
    RootKind kind = RootKind::None;
    // Characters of the input that form the root, including its trailing separator if present.
    std::size_t length = 0;
};

constexpr bool isWindowsSeparator(char c) noexcept { return c == '\\' || c == '/'; }

constexpr bool isAsciiAlpha(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

constexpr bool isSeparator(char c, PathStyle style) noexcept {
    return style == PathStyle::Windows ? isWindowsSeparator(c) : c == '/';
}

// Length of the component starting at `from` (up to the next separator or the end).
std::size_t componentEnd(std::string_view path, std::size_t from, bool backslashOnly) noexcept {
    std::size_t i = from;
    while (i < path.size() && !(backslashOnly ? path[i] == '\\' : isWindowsSeparator(path[i]))) {
        ++i;
    }
    return i;
}

// "\\server\share" with `from` pointing at the server name. Returns the root
// length, or 0 if the server or share is missing.
std::size_t uncRootLength(std::string_view path, std::size_t from, bool backslashOnly) noexcept {
    const std::size_t serverEnd = componentEnd(path, from, backslashOnly);
    if (serverEnd == from || serverEnd >= path.size()) {
        return 0;
    }
    const std::size_t shareStart = serverEnd + 1;
    const std::size_t shareEnd = componentEnd(path, shareStart, backslashOnly);
    if (shareEnd == shareStart) {
        return 0;
    }
    return shareEnd < path.size() ? shareEnd + 1 : shareEnd;
}

// The first component after a "\\?\" or "\\.\" prefix, plus its separator.
std::size_t namespaceRootLength(std::string_view path) noexcept {
    const std::size_t end = componentEnd(path, 4, /*backslashOnly=*/true);
    return end < path.size() ? end + 1 : end;
}

Root parseWindowsRoot(std::string_view p) noexcept {
    if (p.size() >= 2 && isAsciiAlpha(p[0]) && p[1] == ':') {
        if (p.size() >= 3 && isWindowsSeparator(p[2])) {
            return {RootKind::Drive, 3};
        }
        return {RootKind::None, 0}; // "C:" and "C:x" depend on the drive's current directory.
    }
    if (p.size() >= 2 && isWindowsSeparator(p[0]) && isWindowsSeparator(p[1])) {
        if (p.size() >= 4 && (p[2] == '?' || p[2] == '.') && isWindowsSeparator(p[3])) {
            const std::string_view prefix = p.substr(0, 4);
            if (prefix == "\\\\?\\") {
                const std::string_view rest = p.substr(4);
                if (rest.size() >= 3 && isAsciiAlpha(rest[0]) && rest[1] == ':' && rest[2] == '\\') {
                    return {RootKind::Verbatim, 7};
                }
                if (rest.size() >= 4 && (rest[0] == 'U' || rest[0] == 'u') && (rest[1] == 'N' || rest[1] == 'n') &&
                    (rest[2] == 'C' || rest[2] == 'c') && rest[3] == '\\') {
                    const std::size_t length = uncRootLength(p, 8, /*backslashOnly=*/true);
                    return length == 0 ? Root{RootKind::Invalid, 0} : Root{RootKind::Verbatim, length};
                }
                return {RootKind::Verbatim, namespaceRootLength(p)};
            }
            if (prefix == "\\\\.\\") {
                return {RootKind::Device, namespaceRootLength(p)};
            }
            // "//?/" and mixed spellings are normalised by Win32 in surprising ways; refuse them.
            return {RootKind::Invalid, 0};
        }
        const std::size_t length = uncRootLength(p, 2, /*backslashOnly=*/false);
        return length == 0 ? Root{RootKind::Invalid, 0} : Root{RootKind::Unc, length};
    }
    return {RootKind::None, 0};
}

Root parseRoot(std::string_view path, PathStyle style) noexcept {
    if (style == PathStyle::Posix) {
        return !path.empty() && path[0] == '/' ? Root{RootKind::Posix, 1} : Root{RootKind::None, 0};
    }
    return parseWindowsRoot(path);
}

bool isAbsoluteRoot(RootKind kind) noexcept { return kind != RootKind::None && kind != RootKind::Invalid; }

Result<void> checkNoNul(std::string_view path) {
    if (path.find('\0') != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument, "path contains a NUL character");
    }
    return {};
}

// Splits `rest` into components, dropping empty and "." ones and (when
// `resolveDotDot`) resolving ".." against earlier components, never above the root.
std::vector<std::string_view> splitComponents(std::string_view rest, PathStyle style, bool resolveDotDot) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < rest.size()) {
        std::size_t j = i;
        while (j < rest.size() && !isSeparator(rest[j], style)) {
            ++j;
        }
        const std::string_view part = rest.substr(i, j - i);
        if (part.empty() || part == ".") {
            // Skip.
        } else if (part == ".." && resolveDotDot) {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    return parts;
}

std::string joinComponents(std::string root, const std::vector<std::string_view>& parts, char separator) {
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) {
            root += separator;
        }
        root += parts[k];
    }
    return root;
}

Result<std::string> notAbsolute(std::string_view path) {
    return fail(ErrorCode::InvalidArgument, "not an absolute path: '" + std::string(path) + "'");
}

} // namespace

bool isAbsolutePath(std::string_view path, PathStyle style) noexcept {
    return path.find('\0') == std::string_view::npos && isAbsoluteRoot(parseRoot(path, style).kind);
}

Result<std::string> normalizePath(std::string_view path, PathStyle style) {
    if (auto ok = checkNoNul(path); !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    const Root root = parseRoot(path, style);
    switch (root.kind) {
    case RootKind::None:
    case RootKind::Invalid: return notAbsolute(path);
    case RootKind::Verbatim:
    case RootKind::Device: return std::string(path); // Literal by definition.
    case RootKind::Posix:
        return joinComponents("/", splitComponents(path.substr(1), style, /*resolveDotDot=*/false), '/');
    case RootKind::Drive: {
        std::string prefix{path[0], ':', '\\'};
        return joinComponents(std::move(prefix), splitComponents(path.substr(3), style, true), '\\');
    }
    case RootKind::Unc: {
        // Rebuild the root with backslashes: "\\server\share\".
        const std::vector<std::string_view> rootParts = splitComponents(path.substr(0, root.length), style, false);
        std::string prefix = "\\\\" + std::string(rootParts[0]) + "\\" + std::string(rootParts[1]) + "\\";
        return joinComponents(std::move(prefix), splitComponents(path.substr(root.length), style, true), '\\');
    }
    }
    return notAbsolute(path);
}

Result<std::string> extendedLengthPath(std::string_view path) {
    auto normalized = normalizePath(path, PathStyle::Windows);
    if (!normalized) {
        return normalized;
    }
    switch (parseWindowsRoot(path).kind) {
    case RootKind::Drive: return "\\\\?\\" + *normalized;
    case RootKind::Unc: return "\\\\?\\UNC\\" + normalized->substr(2);
    default: return normalized; // Verbatim and device paths are already in their final form.
    }
}

Result<std::string> joinPath(std::string_view base, std::string_view name, PathStyle style) {
    if (auto ok = checkNoNul(name); !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    const bool rooted = !name.empty() && isSeparator(name[0], style);
    const bool driveRelative =
        style == PathStyle::Windows && name.size() >= 2 && isAsciiAlpha(name[0]) && name[1] == ':';
    if (name.empty() || rooted || driveRelative) {
        return fail(ErrorCode::InvalidArgument,
                    "joinPath needs a non-empty relative name, got '" + std::string(name) + "'");
    }
    const char separator = style == PathStyle::Windows ? '\\' : '/';
    std::string out(base);
    if (out.empty() || !isSeparator(out.back(), style)) {
        out += separator;
    }
    for (const char c : name) {
        out += isSeparator(c, style) ? separator : c;
    }
    return out;
}

std::string_view parentPath(std::string_view path, PathStyle style) noexcept {
    const Root root = parseRoot(path, style);
    if (!isAbsoluteRoot(root.kind) || path.size() <= root.length) {
        return {};
    }
    std::size_t last = path.size();
    while (last > root.length && !isSeparator(path[last - 1], style)) {
        --last;
    }
    // `last` is one past the final separator (or the root's end); drop that separator unless it belongs to the root.
    if (last <= root.length) {
        return path.substr(0, root.length);
    }
    return path.substr(0, last - 1);
}

std::string_view fileName(std::string_view path, PathStyle style) noexcept {
    const Root root = parseRoot(path, style);
    if (!isAbsoluteRoot(root.kind) || path.size() <= root.length) {
        return {};
    }
    std::size_t start = path.size();
    while (start > root.length && !isSeparator(path[start - 1], style)) {
        --start;
    }
    return path.substr(start);
}

Result<std::string> fromStdPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return fromWide(path.native());
#else
    return path.native();
#endif
}

Result<std::filesystem::path> toStdPath(std::string_view utf8) {
#ifdef _WIN32
    auto wide = toWide(utf8);
    if (!wide) {
        return std::unexpected(std::move(wide.error()));
    }
    return std::filesystem::path(std::move(*wide));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

#ifdef _WIN32
Result<std::wstring> toWin32Path(std::string_view path) {
    auto extended = extendedLengthPath(path);
    if (!extended) {
        return std::unexpected(std::move(extended.error()));
    }
    auto wide = toWide(*extended);
    if (!wide) {
        return std::unexpected(std::move(wide.error()).withContext("path '" + std::string(path) + "'"));
    }
    return wide;
}
#endif

} // namespace frost::io
