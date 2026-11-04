// SPDX-License-Identifier: Apache-2.0
//
// EditorFindBar (ADR 0037; feature row 85): the find/replace bar an EditorView shows above its text,
// Rider-style. It holds the query, the case / whole-word / regex toggles, next and previous, the
// replace row with Replace and Replace All, and the match count; EditorView runs the searches
// through frost::text_editor's search commands.
#pragma once

#include "frost/text/editor/search.hpp"

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace frost::ui {

/*
 * The bar's widgets and their signals. Keys in the find field: Enter is next, Shift+Enter previous,
 * F3 / Shift+F3 likewise, Escape closes; in the replace field Enter replaces the current match.
 * English labels. TODO(M1): wrap them in tr() once m1/shell-frame's frost/ui/tr.hpp lands.
 * GUI thread only; owned by its EditorView (a child widget).
 */
class EditorFindBar final : public QWidget {
    Q_OBJECT

public:
    explicit EditorFindBar(QWidget* parent = nullptr);

    // Shows or hides the replace row.
    void setReplaceVisible(bool visible);
    [[nodiscard]] bool replaceVisible() const noexcept { return m_replaceVisible; }

    [[nodiscard]] QString findText() const;
    [[nodiscard]] QString replaceText() const;
    [[nodiscard]] text::SearchOptions options() const;
    // Sets the query without emitting queryChanged.
    void setFindText(const QString& text);
    void setOptions(text::SearchOptions options);
    // Focuses the find field with its text selected.
    void focusFind();
    // Focuses the replace field with its text selected.
    void focusReplace();

    // The match count: "3/12", "12 results", "0 results", or the pattern's error.
    void showMatches(std::size_t current, std::size_t total);
    void showError(const QString& message);
    [[nodiscard]] QString matchText() const;

    // The widgets, for tests and the shell's styling.
    [[nodiscard]] QLineEdit* findField() const noexcept { return m_find; }
    [[nodiscard]] QLineEdit* replaceField() const noexcept { return m_replace; }
    [[nodiscard]] QToolButton* caseButton() const noexcept { return m_case; }
    [[nodiscard]] QToolButton* wordButton() const noexcept { return m_word; }
    [[nodiscard]] QToolButton* regexButton() const noexcept { return m_regex; }

signals:
    // The query text or an option changed.
    void queryChanged();
    void findNextRequested();
    void findPreviousRequested();
    void replaceRequested();
    void replaceAllRequested();
    void replaceModeRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* m_find;
    QLineEdit* m_replace;
    QToolButton* m_case;
    QToolButton* m_word;
    QToolButton* m_regex;
    QToolButton* m_previous;
    QToolButton* m_next;
    QToolButton* m_close;
    QLabel* m_matches;
    QWidget* m_replaceRow;
    QPushButton* m_replaceOne;
    QPushButton* m_replaceAll;
    bool m_replaceVisible = false;
    bool m_silent = false;
};

} // namespace frost::ui
