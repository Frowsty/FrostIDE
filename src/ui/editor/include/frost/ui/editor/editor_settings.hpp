// SPDX-License-Identifier: Apache-2.0
//
// The editor's settings adapter (ADR 0021, ADR 0037): EditorViewSettings, the values an EditorView
// reads from a frost::settings snapshot plus the file's EditorConfig, and EditorSettingsBinding,
// which keeps one view up to date as the settings files change.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/settings/editorconfig.hpp"
#include "frost/settings/store.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <memory>
#include <string>

namespace frost::ui {

class EditorView;

/*
 * What an EditorView reads from the settings (keys.hpp's editor.*, files.lineSeparator, ui.theme).
 * The defaults are the registry's (Rider-like: JetBrains Mono 13 pt, line height 1.2, 4 spaces, no
 * soft wrap, line numbers, a blinking caret, the right margin at 120, the renderer on auto). A plain
 * value.
 */
struct EditorViewSettings {
    std::string fontFamily = "JetBrains Mono";
    double fontSize = 13.0; // Points, before IDE zoom.
    double lineSpacing = 1.2;
    bool ligatures = false;
    bool softWrap = false;
    bool lineNumbers = true;
    bool caretBlinking = true;
    std::uint32_t tabSize = 4;
    std::uint32_t indentSize = 4;
    bool insertSpaces = true;
    std::uint32_t rightMargin = 120;
    bool showRightMargin = true;
    // files.lineSeparator for text that has no line break yet: "system", "lf", "crlf" or "cr".
    std::string lineSeparator = "system";
    // ui.theme; the view paints with the installed FrostStyle's theme, this is informational.
    std::string themeId = "glacier-dark";
    // editor.renderer: "auto", "raster" or "rhi" (editor_backend.hpp; ADR 0043).
    std::string renderer = "auto";

    friend bool operator==(const EditorViewSettings&, const EditorViewSettings&) = default;
};

// The settings of one file: the snapshot's values with its EditorConfig overlay. O(keys log n).
[[nodiscard]] EditorViewSettings editorViewSettings(const settings::FileSettings& settings);
// The snapshot's values alone (an untitled document, or EditorConfig unavailable).
[[nodiscard]] EditorViewSettings editorViewSettings(const settings::SettingsSnapshot& snapshot);

/*
 * Keeps `view`'s settings current: resolves settingsForFile(store.snapshot(), fs, path) (just the
 * snapshot when `path` is empty or not absolute, or EditorConfig cannot be read) and applies it on
 * construction and after every published change. The store's listener may run on any thread; it
 * only posts a queued refresh to this object, which runs on the GUI thread.
 *
 * Lifetime: the store and the file system outlive the binding; the binding may outlive the view
 * (it then does nothing). Destroying the binding unsubscribes. GUI thread only, except the
 * listener. TODO(M1): the hardening phase moves the .editorconfig reads off the GUI thread.
 */
class EditorSettingsBinding final : public QObject {
    Q_OBJECT

public:
    EditorSettingsBinding(settings::SettingsStore& store, io::IFileSystem& fs, std::string path, EditorView& view,
                          QObject* parent = nullptr);
    ~EditorSettingsBinding() override;

    // Resolves the settings now and applies them to the view.
    void refresh();
    // Updates the bound file path (after Save As) and resolves settings again.
    void setPath(std::string path);
    // The settings applied last.
    [[nodiscard]] const EditorViewSettings& current() const noexcept { return m_current; }

private:
    settings::SettingsStore* m_store;
    io::IFileSystem* m_fs;
    std::string m_path;
    QPointer<EditorView> m_view;
    EditorViewSettings m_current;
    settings::Subscription m_subscription;
};

} // namespace frost::ui
