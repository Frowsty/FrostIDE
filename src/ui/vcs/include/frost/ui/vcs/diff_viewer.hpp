// SPDX-License-Identifier: Apache-2.0
//
// DiffViewer (M5, ADR 0048): the side-by-side diff of two versions of a file (HEAD or the index on
// the left, the working copy on the right) over frost::vcs::SideBySideDiff: changed lines tinted
// (Diff.deletedBackground left, Diff.insertedBackground right, Diff.modifiedBackground for replaced
// blocks), the changed words inside replaced blocks highlighted (Diff.innerChangeBackground), the two
// panes scrolled together, and F7 / Shift+F7 jumping to the next and previous change.
/*
 * Both panes use the main editor model, renderer, selection and find services, one row per line of git's line
 * model, preserving original text and line endings. The
 * right side becomes editable with setApplyHandler; its Document owns normal transactional undo.
 * Incoming refreshes never replace un-applied edits. Scrolling either pane scrolls the other to the
 * line SideBySideDiff::mapLine pairs with its top line.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/document.hpp"
#include "frost/ui/editor/editor_key_routing.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/vcs/side_by_side.hpp"

#include <QColor>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class QLabel;
class QPushButton;

namespace frost::ui {

class GitController;

// The viewer's colours (Diff.* theme keys).
struct DiffColors {
    QColor inserted;
    QColor deleted;
    QColor modified;
    QColor innerChange;

    // FrostStyle::current()'s theme, or built-in dark colours without a style.
    [[nodiscard]] static DiffColors current();
};

class DiffViewer final : public QWidget {
    Q_OBJECT

public:
    explicit DiffViewer(QWidget* parent = nullptr, EditorServices services = {});
    ~DiffViewer() override;

    // Shows `diff` with a title over each pane ("HEAD", "Working copy"); the caret goes to the first
    // change.
    void setDiff(vcs::SideBySideDiff diff, const QString& leftTitle, const QString& rightTitle);
    [[nodiscard]] const vcs::SideBySideDiff& diff() const noexcept { return m_diff; }

    [[nodiscard]] EditorView* leftPane() const noexcept { return m_left; }
    [[nodiscard]] EditorView* rightPane() const noexcept { return m_right; }
    [[nodiscard]] QString leftTitle() const;
    [[nodiscard]] QString rightTitle() const;
    [[nodiscard]] QString leftText() const;
    [[nodiscard]] QString rightText() const;

    // F7 and Shift+F7: moves both carets to the next (previous) block after (before) the right
    // pane's caret line and scrolls it into view. False when there is none.
    bool nextChange();
    bool previousChange();
    // The block the right pane's caret is in or was moved to, or nullopt.
    [[nodiscard]] std::optional<std::size_t> currentBlock() const noexcept { return m_current; }
    // The line (0-based) at the top of a pane.
    [[nodiscard]] int topLine(vcs::DiffSide side) const;
    // Scrolls a pane so `line` is at its top (the other pane follows).
    void scrollToLine(vcs::DiffSide side, int line);

    // The highlight count of a pane (line tints plus word highlights), for tests.
    [[nodiscard]] int highlightCount(vcs::DiffSide side) const;

    // Enables editing in the real editor model. Apply receives the originally displayed working
    // text and the current committed text; the owner must reject stale sources and save atomically.
    // The owner outlives the callback. GUI thread; no file write occurs inside the viewer.
    using ApplyHandler = std::function<Result<void>(std::string_view, std::string_view)>;
    void setApplyHandler(ApplyHandler handler);
    [[nodiscard]] Result<void> applyWorkingCopy();
    [[nodiscard]] bool hasWorkingEdits() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyHighlights();
    void goToBlock(std::size_t index);
    void syncFrom(vcs::DiffSide side);

    vcs::SideBySideDiff m_diff;
    QLabel* m_leftTitle;
    QLabel* m_rightTitle;
    std::unique_ptr<EditorKeyRouting> m_keys;
    EditorServices m_services;
    std::unique_ptr<text::Document> m_leftDocument;
    std::unique_ptr<text::Document> m_rightDocument;
    EditorView* m_left;
    EditorView* m_right;
    QPushButton* m_apply;
    QLabel* m_status;
    ApplyHandler m_applyHandler;
    std::string m_originalWorking;
    void workingEdited();
    std::optional<std::size_t> m_current;
    bool m_syncing = false;
};

// The version a diff's left side shows.
enum class DiffBase : std::uint8_t { Head, Index };

/*
 * Compare with HEAD (and the Commit window's diff): reads `absolutePath`'s HEAD (or index) version
 * through `controller` and shows it in `viewer` against `workingCopy` (the editor's buffer, or the
 * file's bytes), titled "HEAD" (or "Index") and "Working copy"; a file without that version (new or
 * unversioned) shows an empty left side. `done` runs on the GUI thread with the outcome, and not at
 * all when the viewer or the controller is gone first. Errors: the read's, or the diff's (Cancelled,
 * TooLarge). GUI thread.
 */
void showDiffAgainst(DiffViewer& viewer, GitController& controller, DiffBase base, const std::string& absolutePath,
                     std::string workingCopy, std::function<void(Result<void>)> done = {});

} // namespace frost::ui
