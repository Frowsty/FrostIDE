// SPDX-License-Identifier: Apache-2.0
//
// EditorChangeMarkers (M5, ADR 0048): keeps one EditorView's VCS change stripes current. It reads
// the file's HEAD version once (and again after a commit, checkout or pull moved HEAD), diffs the
// buffer against it with frost::diff debounced as the user types, and gives the view its markers;
// a click on a stripe opens a popup with the old text of that change and Rollback Lines.
/*
 * Base text. The base comes from a BaseProvider (GitController::readHeadContent in the IDE, a fake in
 * tests): the HEAD blob as a checkout writes it. A file without a HEAD version (unversioned, new, no
 * commit yet) gets no markers. The base is read again on refreshBase() (the owner calls it when the
 * status generation moved).
 *
 * Diffing runs on the GUI thread after `debounce` of quiet typing (300 ms by default); one diff of an
 * ordinary file is well inside frost::diff's B21 cap. TODO(M6): diff off the GUI thread for large
 * files.
 *
 * Rollback Lines replaces the change's lines with the base's (one undoable edit through
 * EditorView::apply). GUI thread only; the view must outlive this object (it is the parent).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/vcs/line_changes.hpp"

#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace frost::ui {

class EditorView;

class EditorChangeMarkers final : public QObject {
    Q_OBJECT

public:
    // Delivers the base text of `path` (nullopt: no base) on the GUI thread, possibly later.
    using BaseProvider =
        std::function<void(const std::string& path, std::function<void(Result<std::optional<std::string>>)> done)>;

    // Follows `view` (also the parent) and reads its base at once.
    EditorChangeMarkers(EditorView& view, BaseProvider provider);
    ~EditorChangeMarkers() override;

    void setDebounce(std::chrono::milliseconds debounce);
    // Reads the base again (after HEAD moved).
    void refreshBase();
    // Diffs now instead of after the debounce.
    void recompute();

    [[nodiscard]] bool hasBase() const noexcept { return m_base.has_value(); }
    [[nodiscard]] const std::vector<vcs::LineChange>& changes() const noexcept { return m_changes; }
    // The base text of change `index` ("" for an added block).
    [[nodiscard]] std::string baseText(std::size_t index) const;
    // Rolls change `index` back in the buffer; false for an invalid index or a read-only editor.
    bool rollback(std::size_t index);

    // Opens the change popup for change `index` at `globalPos` (a stripe click does this).
    void showChangePopup(std::size_t index, QPoint globalPos);
    // The open popup, or nullptr; its Rollback Lines button.
    [[nodiscard]] Popup* popup() const noexcept { return m_popup; }
    [[nodiscard]] QPushButton* popupRollbackButton() const noexcept { return m_popupRollback; }

signals:
    // The markers were recomputed (also when the base arrived or went).
    void changesUpdated();

private:
    void onBase(Result<std::optional<std::string>> base, std::uint64_t request);

    QPointer<EditorView> m_view;
    BaseProvider m_provider;
    QTimer m_debounce;
    std::optional<diff::LineSequence> m_base;
    diff::LineSequence m_current; // The buffer text the changes were computed against.
    std::vector<vcs::LineChange> m_changes;
    std::uint64_t m_request = 0;
    QPointer<Popup> m_popup;
    QPointer<QPushButton> m_popupRollback;
};

} // namespace frost::ui
