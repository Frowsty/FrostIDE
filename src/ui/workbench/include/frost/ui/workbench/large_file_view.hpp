// SPDX-License-Identifier: Apache-2.0
// Bounded read-only large-file tab. GUI calls never read file contents.
#pragma once
#include "frost/ui/shell/editor_host.hpp"
#include "frost/io/file_system.hpp"
namespace frost::ui {
// The view copies fs.randomAccessOpener() and owns its widget. Destruction cancels pending work;
// jobs own their dependencies and hold no view/widget/host/fs pointers. No editable document exists.
[[nodiscard]] std::unique_ptr<IEditorView> makeLargeFileView(io::IFileSystem& fs, std::string path,
                                                           IEditorHost& host);
}
