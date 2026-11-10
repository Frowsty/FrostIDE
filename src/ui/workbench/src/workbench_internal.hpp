// SPDX-License-Identifier: Apache-2.0
//
// Internal Workbench helpers: path validation, TODO comment scanning, popup item metadata, and
// background snapshot retirement.
#pragma once

#include "frost/io/path.hpp"
#include "frost/ui/nav/paths.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include "workbench_util.hpp"
#include <QVariantMap>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::ui {

// Actions run from the main window, not from popups or dialogs (as the shell's own).
inline constexpr input::ContextRule kWindowRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};
// The i18n context of action names from the registry (the shell's).
inline constexpr std::string_view kActionsContext = "frost::input::actions";

[[nodiscard]] inline QString lowerExtension(std::string_view path) {
    const std::string_view name = nav::baseName(path);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? QString() : workbench_detail::fromUtf8(name.substr(dot)).toLower();
}

[[nodiscard]] inline bool isUnder(std::string_view root, std::string_view path) {
    if (root.empty() || path.size() <= root.size() || !path.starts_with(root)) {
        return false;
    }
    const char next = path[root.size()];
    return next == '/' || next == '\\' || root.back() == '/' || root.back() == '\\';
}

[[nodiscard]] inline bool isReservedWindowsDeviceStem(std::string_view stem) noexcept {
    std::string upper(stem);
    std::ranges::transform(upper, upper.begin(),
                           [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' &&
        upper[3] <= '9') {
        return true;
    }
    return false;
}

[[nodiscard]] inline Result<std::string> validateNewItemTarget(io::IFileSystem& fs,
                                                               const nav::DocumentRegistry& documents,
                                                               std::string_view directory,
                                                               std::string_view relativeName) {
    if (directory.empty()) {
        return fail(ErrorCode::InvalidArgument, "No folder is open to create an item in.");
    }
    if (relativeName.empty()) {
        return fail(ErrorCode::InvalidArgument, "The name must not be empty.");
    }
    if (relativeName.front() == ' ' || relativeName.back() == ' ' || relativeName.front() == '/' ||
        relativeName.front() == '\\' || io::isAbsolutePath(relativeName, io::PathStyle::Posix) ||
        io::isAbsolutePath(relativeName, io::PathStyle::Windows)) {
        return fail(ErrorCode::InvalidArgument, "Enter a relative name inside the target folder.");
    }
    std::size_t pos = 0;
    while (pos <= relativeName.size()) {
        const std::size_t sep = relativeName.find_first_of("/\\", pos);
        const std::string_view segment =
            sep == std::string_view::npos ? relativeName.substr(pos) : relativeName.substr(pos, sep - pos);
        if (segment.empty() || segment == "." || segment == "..") {
            return fail(ErrorCode::InvalidArgument, "The path may not contain empty, '.' or '..' segments.");
        }
        if (segment.back() == '.' || segment.back() == ' ') {
            return fail(ErrorCode::InvalidArgument, "A file or folder name may not end with a dot or space.");
        }
        for (const char ch : segment) {
            const auto uc = static_cast<unsigned char>(ch);
            if (uc < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
                return fail(ErrorCode::InvalidArgument,
                            "The name contains characters that are not allowed in file names.");
            }
        }
        const std::size_t dot = segment.find('.');
        const std::string_view stem = dot == std::string_view::npos ? segment : segment.substr(0, dot);
        if (isReservedWindowsDeviceStem(stem)) {
            return fail(ErrorCode::InvalidArgument, "The name is a reserved device name.");
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1;
    }
    const std::string baseDir = documents.canonicalize(directory);
    auto baseStatus = fs.status(baseDir);
    if (!baseStatus) {
        return std::unexpected(std::move(baseStatus.error()));
    }
    if (baseStatus->type != io::FileType::Directory) {
        return fail(ErrorCode::InvalidArgument, "'" + baseDir + "' is not a directory.");
    }
    const io::PathStyle style = nav::styleOf(baseDir);
    auto joined = io::joinPath(baseDir, relativeName, style);
    if (!joined) {
        return std::unexpected(std::move(joined.error()));
    }
    auto normalized = io::normalizePath(*joined, style);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (!isUnder(baseDir, *normalized)) {
        return fail(ErrorCode::InvalidArgument, "The target path must stay inside the selected folder.");
    }
    auto exists = fs.exists(*normalized);
    if (!exists) {
        return std::unexpected(std::move(exists.error()));
    }
    if (*exists) {
        return fail(ErrorCode::AlreadyExists, "'" + std::string(nav::baseName(*normalized)) + "' already exists.");
    }
    return *normalized;
}

[[nodiscard]] inline bool isTodoScannablePath(std::string_view path) noexcept {
    if (path.empty()) {
        return false;
    }
    const auto toLowerAscii = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    };
    const auto containsSegmentInsensitive = [&](std::string_view seg) noexcept {
        for (std::size_t i = 0; i + seg.size() + 2 <= path.size(); ++i) {
            const char prev = path[i];
            if (prev != '/' && prev != '\\') {
                continue;
            }
            const char next = path[i + 1 + seg.size()];
            if (next != '/' && next != '\\') {
                continue;
            }
            bool match = true;
            for (std::size_t j = 0; j < seg.size(); ++j) {
                if (toLowerAscii(path[i + 1 + j]) != seg[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return true;
            }
        }
        return false;
    };

    static constexpr std::string_view kIgnoredDirs[] = {
        "library", "temp", "logs", "usersettings", "memorycaptures", "packagecache",
        "obj",     "bin",  "out",  "build",        "node_modules",   ".git",
        ".vs",     ".svn", ".hg",  "artifacts",    "shadercache",    "il2cpp_cache"};
    for (std::string_view seg : kIgnoredDirs) {
        if (containsSegmentInsensitive(seg)) {
            return false;
        }
    }

    const std::string_view name = nav::baseName(path);
    if (name == "CMakeLists.txt" || name == "Makefile" || name == "Dockerfile") {
        return true;
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= name.size()) {
        return false;
    }
    const std::string_view rawExt = name.substr(dot);
    if (rawExt.size() > 12) {
        return false;
    }
    char extBuf[12]{};
    for (std::size_t i = 0; i < rawExt.size(); ++i) {
        extBuf[i] = toLowerAscii(rawExt[i]);
    }
    const std::string_view ext(extBuf, rawExt.size());

    static constexpr std::string_view kSourceExts[] = {
        ".c",     ".cc",   ".cpp",  ".cxx",  ".c++",  ".cppm",   ".ixx",   ".h",       ".hh",   ".hpp",
        ".hxx",   ".inl",  ".ipp",  ".cs",   ".csx",  ".fs",     ".vb",    ".lua",     ".luau", ".hlsl",
        ".hlsli", ".glsl", ".vert", ".frag", ".comp", ".shader", ".cginc", ".compute", ".uss",  ".uxml",
        ".py",    ".js",   ".ts",   ".jsx",  ".tsx",  ".rs",     ".go",    ".java",    ".kt",   ".swift",
        ".cmake", ".txt",  ".md",   ".sh",   ".ps1",  ".bat",    ".cmd"};
    for (std::string_view allowed : kSourceExts) {
        if (ext == allowed) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool hasCommentPrefixBefore(std::string_view line, std::size_t pos) noexcept {
    const std::string_view prefix = line.substr(0, pos);
    if (prefix.find("//") != std::string_view::npos || prefix.find("/*") != std::string_view::npos ||
        prefix.find("--") != std::string_view::npos || prefix.find("<!--") != std::string_view::npos ||
        prefix.find('#') != std::string_view::npos) {
        return true;
    }
    const std::size_t firstNonSpace = prefix.find_first_not_of(" \t");
    return firstNonSpace != std::string_view::npos && (prefix[firstNonSpace] == '*' || prefix[firstNonSpace] == ';');
}

[[nodiscard]] inline bool mayContainTodoTag(std::string_view content) noexcept {
    const auto toUpper = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    };
    for (std::size_t i = 0; i + 3 <= content.size(); ++i) {
        const char c0 = toUpper(content[i]);
        if (c0 == 'T' && i + 4 <= content.size() && toUpper(content[i + 1]) == 'O' && toUpper(content[i + 2]) == 'D' &&
            toUpper(content[i + 3]) == 'O') {
            return true;
        }
        if (c0 == 'F' && i + 5 <= content.size() && toUpper(content[i + 1]) == 'I' && toUpper(content[i + 2]) == 'X' &&
            toUpper(content[i + 3]) == 'M' && toUpper(content[i + 4]) == 'E') {
            return true;
        }
        if (c0 == 'H' && i + 4 <= content.size() && toUpper(content[i + 1]) == 'A' && toUpper(content[i + 2]) == 'C' &&
            toUpper(content[i + 3]) == 'K') {
            return true;
        }
        if (c0 == 'B' && toUpper(content[i + 1]) == 'U' && toUpper(content[i + 2]) == 'G') {
            return true;
        }
        if (c0 == 'U' && i + 6 <= content.size() && toUpper(content[i + 1]) == 'N' && toUpper(content[i + 2]) == 'D' &&
            toUpper(content[i + 3]) == 'O' && toUpper(content[i + 4]) == 'N' && toUpper(content[i + 5]) == 'E') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::optional<std::pair<std::size_t, std::string_view>> findTodoTag(std::string_view line) {
    static constexpr std::string_view kTags[] = {"TODO", "FIXME", "HACK", "UNDONE", "BUG"};
    const auto isWordChar = [](char c) noexcept {
        const auto uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '_';
    };
    const auto toUpperAscii = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    };
    const auto matchesTagAt = [&](std::size_t pos, std::string_view tag, bool& exactUpper) noexcept {
        if (pos + tag.size() > line.size()) {
            return false;
        }
        exactUpper = true;
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (line[pos + i] != tag[i]) {
                exactUpper = false;
            }
            if (toUpperAscii(line[pos + i]) != tag[i]) {
                return false;
            }
        }
        return true;
    };
    std::optional<std::pair<std::size_t, std::string_view>> best;
    for (std::string_view tag : kTags) {
        for (std::size_t pos = 0; pos + tag.size() <= line.size(); ++pos) {
            bool exactUpper = false;
            if (!matchesTagAt(pos, tag, exactUpper)) {
                continue;
            }
            const bool leftOk = (pos == 0) || !isWordChar(line[pos - 1]);
            const std::size_t end = pos + tag.size();
            const bool rightOk = (end >= line.size()) || !isWordChar(line[end]);
            if (!leftOk || !rightOk) {
                continue;
            }
            const bool inComment = hasCommentPrefixBefore(line, pos);
            const bool hasColon = (end < line.size() && line[end] == ':');
            const bool isPrimaryTag = (tag == "TODO" || tag == "FIXME");
            if (!inComment && !hasColon && !(isPrimaryTag && exactUpper)) {
                continue;
            }
            if (!best || pos < best->first) {
                best = std::pair<std::size_t, std::string_view>{pos, tag};
            }
            break;
        }
    }
    return best;
}

[[nodiscard]] inline QVariant fileData(const std::string& path, std::optional<int> line = std::nullopt,
                                       std::optional<int> column = std::nullopt) {
    QVariantMap map;
    map.insert(QStringLiteral("kind"), QStringLiteral("file"));
    map.insert(QStringLiteral("path"), workbench_detail::fromUtf8(path));
    if (line) {
        map.insert(QStringLiteral("line"), *line);
        map.insert(QStringLiteral("column"), column.value_or(1));
    }
    return map;
}

// The scheduler owns the final reference so large old trees are freed away from the GUI.
inline sched::Task<void> retireProjectFiles(std::shared_ptr<const nav::ProjectFiles> files) {
    files.reset();
    co_return;
}

inline void retireSnapshot(sched::IScheduler* scheduler, std::shared_ptr<const nav::ProjectFiles> files) {
    if (scheduler && files) {
        scheduler->spawn(sched::Lane::Background, retireProjectFiles(std::move(files))).detach();
    }
}

inline sched::Task<void> retireSolutionFiles(std::shared_ptr<const std::vector<std::string>> files) {
    files.reset();
    co_return;
}

inline void retireSolutionSnapshot(sched::IScheduler* scheduler,
                                   std::shared_ptr<const std::vector<std::string>> files) {
    if (scheduler && files) {
        scheduler->spawn(sched::Lane::Background, retireSolutionFiles(std::move(files))).detach();
    }
}

} // namespace frost::ui
