// SPDX-License-Identifier: Apache-2.0
//
// The Open Containing Folder launch (frost/ui/toolwindows/file_manager.hpp).

#include "frost/ui/toolwindows/file_manager.hpp"

#include "frost/io/path.hpp"

#include <format>
#include <string>
#include <utility>

#ifdef _WIN32
#include "frost/io/os_error.hpp"
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::ui {
namespace {

#ifdef _WIN32
// <Windows directory>\explorer.exe, from GetWindowsDirectoryW.
Result<std::string> explorerPath() {
    std::wstring directory(MAX_PATH, L'\0');
    for (;;) {
        const UINT length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0) {
            return std::unexpected(io::lastWin32Error("GetWindowsDirectoryW"));
        }
        if (length < directory.size()) {
            directory.resize(length);
            break;
        }
        directory.resize(length); // Too small: `length` is the size needed, terminator included.
    }
    auto utf8 = io::fromWide(directory);
    if (!utf8) {
        return std::unexpected(std::move(utf8).error());
    }
    return io::joinPath(*utf8, "explorer.exe");
}
#endif

} // namespace

Result<io::ProcessSpec> fileManagerSpec(std::string_view path, bool isDirectory) {
    if (!io::isAbsolutePath(path)) {
        return fail(ErrorCode::InvalidArgument, std::format("not an absolute path: '{}'", path));
    }
    io::ProcessSpec spec;
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Null;
    spec.stderrMode = io::Stdio::Null;
    spec.killOnClose = false;
#ifdef _WIN32
    auto explorer = explorerPath();
    if (!explorer) {
        return std::unexpected(std::move(explorer).error());
    }
    spec.executable = std::move(*explorer);
    spec.arguments.push_back(isDirectory ? std::string(path) : std::string("/select,") + std::string(path));
    return spec;
#elif defined(__linux__)
    spec.executable = "/usr/bin/xdg-open";
    spec.arguments.emplace_back(isDirectory ? path : io::parentPath(path));
    return spec;
#else
    static_cast<void>(isDirectory);
    return fail(ErrorCode::Unsupported, "no file manager launch on this platform");
#endif
}

Result<void> revealInFileManager(io::IProcessLauncher& launcher, std::string_view path, bool isDirectory) {
    auto spec = fileManagerSpec(path, isDirectory);
    if (!spec) {
        return std::unexpected(std::move(spec).error());
    }
    auto process = launcher.launch(*spec);
    if (!process) {
        return std::unexpected(std::move(process).error());
    }
    return {}; // Destroying an uncontained IProcess leaves the child running.
}

} // namespace frost::ui
