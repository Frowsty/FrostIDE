// SPDX-License-Identifier: Apache-2.0
//
// The editor settings adapter (frost/ui/editor/editor_settings.hpp): typed reads of the editor keys
// and the binding that re-applies them to a view after every settings change.

#include "frost/ui/editor/editor_settings.hpp"

#include "frost/settings/keys.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QMetaObject>

#include <algorithm>
#include <utility>

namespace frost::ui {

namespace {

namespace key = settings::key;

[[nodiscard]] std::uint32_t clampCount(std::int64_t value, std::int64_t high) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 1, high));
}

// FileSettings and SettingsSnapshot share get(Key<T>).
template<class Source>
[[nodiscard]] EditorViewSettings read(const Source& source) {
    EditorViewSettings out;
    out.fontFamily = source.get(key::editorFontFamily);
    out.fontSize = static_cast<double>(source.get(key::editorFontSize));
    out.lineSpacing = source.get(key::editorLineSpacing);
    out.ligatures = source.get(key::editorFontLigatures);
    out.softWrap = source.get(key::editorSoftWrap);
    out.lineNumbers = source.get(key::editorLineNumbers);
    out.caretBlinking = source.get(key::editorCaretBlinking);
    out.tabSize = clampCount(source.get(key::editorTabSize), 32);
    out.indentSize = clampCount(source.get(key::editorIndentSize), 32);
    out.insertSpaces = source.get(key::editorInsertSpaces);
    out.rightMargin = clampCount(source.get(key::editorRightMargin), 1000);
    out.showRightMargin = source.get(key::editorShowRightMargin);
    out.lineSeparator = source.get(key::filesLineSeparator);
    out.themeId = source.get(key::uiTheme);
    out.renderer = source.get(key::editorRenderer);
    out.showWhitespace = source.get(key::editorShowWhitespace);
    out.indentGuides = source.get(key::editorIndentGuides);
    return out;
}

} // namespace

EditorViewSettings editorViewSettings(const settings::FileSettings& settings) { return read(settings); }

EditorViewSettings editorViewSettings(const settings::SettingsSnapshot& snapshot) { return read(snapshot); }

EditorSettingsBinding::EditorSettingsBinding(settings::SettingsStore& store, io::IFileSystem& fs, std::string path,
                                             EditorView& view, QObject* parent)
    : QObject(parent), m_store(&store), m_fs(&fs), m_path(std::move(path)), m_view(&view) {
    // The listener may run on any thread: it only queues a refresh on this object's (GUI) thread.
    m_subscription = store.subscribe([this](const settings::SettingsChange&) {
        QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
    });
    refresh();
}

EditorSettingsBinding::~EditorSettingsBinding() { m_subscription.reset(); }

void EditorSettingsBinding::refresh() {
    const std::shared_ptr<const settings::SettingsSnapshot> snapshot = m_store->snapshot();
    EditorViewSettings next = editorViewSettings(*snapshot);
    if (!m_path.empty()) {
        if (Result<settings::FileSettings> file = settings::settingsForFile(snapshot, *m_fs, m_path)) {
            next = editorViewSettings(*file);
        }
    }
    m_current = next;
    if (m_view) {
        m_view->applySettings(next);
    }
}

void EditorSettingsBinding::setPath(std::string path) {
    m_path = std::move(path);
    refresh();
}

} // namespace frost::ui
