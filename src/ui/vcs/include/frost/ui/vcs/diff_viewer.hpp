// SPDX-License-Identifier: Apache-2.0
//
// DiffViewer (M5, ADR 0048): the side-by-side diff of two versions of a file (HEAD or the index on
// the left, the working copy on the right) over frost::vcs::SideBySideDiff: changed lines tinted
// (Diff.deletedBackground left, Diff.insertedBackground right, Diff.modifiedBackground for replaced
// blocks), the changed words inside replaced blocks highlighted (Diff.innerChangeBackground), the two
// panes scrolled together, and F7 / Shift+F7 jumping to the next and previous change.
/*
 * Both panes are read-only plain-text views without wrapping, one block per line of git's line
 * model (a line's "\r\n" or "\n" is not shown; a lone '\r' inside a line shows as a space). The
 * right side is read-only in this slice (ADR 0048: editing it needs the editor widget in the right
 * pane; TODO(M6): an editable working-copy side). Scrolling either pane scrolls the other to the
 * line SideBySideDiff::mapLine pairs with its top line.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/core/error.hpp"
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
class QPlainTextEdit;

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
    explicit DiffViewer(QWidget* parent = nullptr);
    ~DiffViewer() override;

    // Shows `diff` with a title over each pane ("HEAD", "Working copy"); the caret goes to the first
    // change.
    void setDiff(vcs::SideBySideDiff diff, const QString& leftTitle, const QString& rightTitle);
    [[nodiscard]] const vcs::SideBySideDiff& diff() const noexcept { return m_diff; }

    [[nodiscard]] QPlainTextEdit* leftPane() const noexcept { return m_left; }
    [[nodiscard]] QPlainTextEdit* rightPane() const noexcept { return m_right; }
    [[nodiscard]] QString leftTitle() const;
    [[nodiscard]] QString rightTitle() const;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyHighlights();
    void goToBlock(std::size_t index);
    void syncFrom(vcs::DiffSide side);

    vcs::SideBySideDiff m_diff;
    QLabel* m_leftTitle;
    QLabel* m_rightTitle;
    QPlainTextEdit* m_left;
    QPlainTextEdit* m_right;
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
