// SPDX-License-Identifier: Apache-2.0
//
// PlaceholderEditorFactory: the stand-in editor until m1/editor-view lands (ADR 0035). It opens a
// file through frost::text's load pipeline (encoding and line-separator detection, ADR 0018) and
// shows it in a read-only plain-text view, so the shell's tabs and status bar have something real
// to host.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/ui/shell/editor_host.hpp"

#include <memory>
#include <string>

namespace frost::ui {

/*
 * Opens read-only viewers. `fs` must outlive the factory and every view it creates. Files are read
 * whole on the UI thread (a placeholder: the real editor loads in the background) and refused
 * above kMaxBytes with OutOfRange. UI thread only.
 */
class PlaceholderEditorFactory final : public IEditorFactory {
public:
    static constexpr std::uint64_t kMaxBytes = std::uint64_t{32} * 1024 * 1024;

    explicit PlaceholderEditorFactory(io::IFileSystem& fs) noexcept : m_fs(fs) {}

    [[nodiscard]] Result<std::unique_ptr<IEditorView>> open(const std::string& path, IEditorHost& host) override;

private:
    io::IFileSystem& m_fs;
};

} // namespace frost::ui
