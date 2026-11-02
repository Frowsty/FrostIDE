// SPDX-License-Identifier: Apache-2.0
//
// canonicalPath and OpenFileService (frost/app/open_file.hpp).

#include "frost/app/open_file.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_window.hpp"

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <cstdlib>
#include <memory>
#endif

#include <algorithm>
#include <optional>
#include <string>

namespace frost::app {

namespace {

#ifdef _WIN32
// ASCII-case-insensitive equality; the file system's own case folding is wider, but a final path
// that differs from the input beyond ASCII case is treated as a different spelling (kept as is).
bool sameIgnoringAsciiCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

// The file system's spelling of the existing `normalized` path, or nullopt (missing, no access).
std::optional<std::string> finalPath(const std::string& normalized) {
    const Result<std::wstring> wide = io::toWin32Path(normalized);
    if (!wide) {
        return std::nullopt;
    }
    // No access rights are needed to ask for the name; FILE_FLAG_BACKUP_SEMANTICS opens folders too.
    HANDLE handle = CreateFileW(wide->c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::wstring buffer(512, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(handle);
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(length);
    // "\\?\C:\a" -> "C:\a", "\\?\UNC\server\share" -> "\\server\share".
    std::wstring_view view(buffer);
    std::wstring plain;
    if (view.starts_with(L"\\\\?\\UNC\\")) {
        plain = L"\\\\" + std::wstring(view.substr(8));
    } else if (view.starts_with(L"\\\\?\\")) {
        plain = std::wstring(view.substr(4));
    } else {
        plain = std::wstring(view);
    }
    Result<std::string> utf8 = io::fromWide(plain);
    if (!utf8) {
        return std::nullopt;
    }
    Result<std::string> normalizedFinal = io::normalizePath(*utf8);
    return normalizedFinal ? std::optional<std::string>(std::move(*normalizedFinal)) : std::nullopt;
}
#else
// True when the absolute POSIX `path` has a ".." component.
bool hasParentComponent(std::string_view path) {
    for (std::size_t start = 0; start < path.size();) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (path.substr(start, end - start) == "..") {
            return true;
        }
        start = end + 1;
    }
    return false;
}

/*
 * normalizePath keeps ".." on POSIX (through a symbolic link it is not lexical), so ".../Lib/../a"
 * and ".../a" would be two documents. An existing path with a ".." component takes realpath's
 * answer, which resolves it as the kernel does; nullopt otherwise (no "..", missing, no access).
 */
std::optional<std::string> resolvedParents(const std::string& normalized) {
    if (!hasParentComponent(normalized)) {
        return std::nullopt;
    }
    // realpath allocates its result with malloc.
    const std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(normalized.c_str(), nullptr), &std::free);
    if (resolved == nullptr) {
        return std::nullopt;
    }
    return std::string(resolved.get());
}
#endif

} // namespace

Result<std::string> canonicalPath(std::string_view path) {
    Result<std::string> normalized = io::normalizePath(path);
    if (!normalized) {
        return normalized;
    }
#ifdef _WIN32
    if (std::optional<std::string> real = finalPath(*normalized);
        real && real->size() == normalized->size() && sameIgnoringAsciiCase(*real, *normalized)) {
        return std::move(*real);
    }
#else
    if (std::optional<std::string> resolved = resolvedParents(*normalized)) {
        return std::move(*resolved);
    }
#endif
    return normalized;
}

Result<ui::IEditorView*> OpenFileService::open(std::string_view path, FileLocation at) {
    Result<std::string> canonical = canonicalPath(path);
    if (!canonical) {
        return std::unexpected(std::move(canonical.error()));
    }
    // ShellWindow::openFile opens or activates the tab and notifies on errors.
    Result<int> index = m_window.openFile(*canonical);
    if (!index) {
        return std::unexpected(std::move(index.error()));
    }
    ui::IEditorView* editor = m_window.tabs().editor(*index);
    if (editor != nullptr && at.line > 0) {
        editor->goToLine(at.line, std::max(1, at.column));
    }
    return editor;
}

} // namespace frost::app
