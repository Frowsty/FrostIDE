// SPDX-License-Identifier: Apache-2.0
//
// The workbench's one open-file path (W1, ADR 0044): canonicalPath, which gives every spelling of
// a file one identity, and OpenFileService, which every caller that opens a file in an editor goes
// through (the command line, File > Open, the Solution Explorer, the Problems and Build output
// windows, the Welcome screen; m1/shell-nav's DocumentRegistry and popups next).
/*
 * Identity. A path is made absolute and normalised (frost::io::normalizePath); on Windows an
 * existing file then takes its real spelling from the file system (GetFinalPathNameByHandleW), so
 * "foo.cpp" and "Foo.cpp" are one document and one tab. The final path is used only when it differs
 * from the input in case alone: a path through a symbolic link, a junction or a subst drive keeps its
 * own spelling, so the document stays where the user opened it. A missing file keeps the
 * normalised spelling. On Linux the file system is case-sensitive and the normalised path is the
 * identity, except that normalizePath keeps ".." there: an existing path with a ".." component
 * takes realpath's answer (so its symbolic links are resolved too).
 *
 * Opening. open() canonicalises, opens the file in a tab of the window (or activates its tab: one
 * tab per document) through the editor factory, which loads it through the save manager's load
 * service and reports refusals and read-only reasons as notifications, then moves the caret.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <string_view>

namespace frost::ui {
class IEditorView;
class ShellWindow;
} // namespace frost::ui

namespace frost::app {

// A place in a file: 1-based line and column; line 0 leaves the caret where it is (a new document:
// at the start), column 0 is column 1.
struct FileLocation {
    int line = 0;
    int column = 0;
};

/*
 * The identity of `path` (see the header comment). Errors: InvalidArgument for a relative path, an
 * embedded NUL or a malformed root (normalizePath's). Blocking: one open and close of the file on
 * Windows, one realpath on Linux when the path has a ".." component. O(path length).
 */
[[nodiscard]] Result<std::string> canonicalPath(std::string_view path);

class OpenFileService {
public:
    // `window` outlives the service.
    explicit OpenFileService(ui::ShellWindow& window) noexcept : m_window(window) {}

    /*
     * Opens `path` (absolute) in an editor tab, or activates its tab, and moves the caret to `at`.
     * Returns the editor. Errors: canonicalPath's; the editor factory's (NotFound, PermissionDenied,
     * IoError, OutOfRange for a file too large to open), each also shown as an error notification.
     */
    [[nodiscard]] Result<ui::IEditorView*> open(std::string_view path, FileLocation at = {});

private:
    ui::ShellWindow& m_window;
};

} // namespace frost::app
