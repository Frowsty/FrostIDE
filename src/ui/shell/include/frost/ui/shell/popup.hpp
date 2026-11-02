// SPDX-License-Identifier: Apache-2.0
//
// The popup framework (PLAN.md row 241, ADR 0035): Popup, a lightweight keyboard-first popup
// window base (Escape cancels, a click outside closes, it paints the theme's popup surface), and
// two building blocks on it: ListPopup (an optional filter field over a list; Find Action, the
// Ctrl+Tab switcher, Quick Switch Theme) and InputPopup (one validated text field; Go to Line).
#pragma once

#include <QFrame>
#include <QIcon>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;

namespace frost::ui {

/*
 * A Qt::Popup window owned by `owner` (the shell window, whose child it is). It grabs the keyboard
 * while open; Escape closes it with Cancelled, a click outside or hiding it otherwise with
 * Dismissed, and subclasses close it with Accepted. closed() is emitted exactly once per open.
 * With deleteOnClose (the default) the popup deletes itself (deleteLater) after closing. The
 * shell's shortcut dispatcher sets ContextKey::PopupFocus while one is open. GUI thread only.
 */
class Popup : public QFrame {
    Q_OBJECT

public:
    enum class CloseReason : std::uint8_t { Accepted, Cancelled, Dismissed };
    Q_ENUM(CloseReason)

    explicit Popup(QWidget* owner);
    ~Popup() override;

    // Shows the popup horizontally centred on `window`, its top edge at `topFraction` of the
    // window's height (Find Action and Go to Line sit near the top).
    void showCentered(QWidget* window, double topFraction = 0.2);
    // Shows the popup below `anchor`, left-aligned with it.
    void showBelow(QWidget* anchor);
    // Closes it with `reason` (no-op when not open).
    void closePopup(CloseReason reason);
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    void setDeleteOnClose(bool on) noexcept { m_deleteOnClose = on; }

signals:
    void closed(frost::ui::Popup::CloseReason reason);

protected:
    // Opens: shows the window and gives focus to focusProxy() or the popup.
    void openAt(QPoint globalTopLeft);
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_open = false;
    bool m_deleteOnClose = true;
};

// One ListPopup row.
struct ListPopupItem {
    QString text;
    // Secondary text drawn after the main text (a shortcut, a path).
    QString detail;
    QIcon icon;
    QVariant data;
};

/*
 * A list with an optional filter field. Typing filters rows by a case-insensitive substring of
 * the text; Up, Down, Page Up and Page Down move the selection (wrapping at the ends); Enter
 * accepts the selected row. With acceptOnRelease(key) set (the switcher's Ctrl), releasing that
 * key accepts too. itemAccepted() carries the index into the items given to setItems().
 */
class ListPopup final : public Popup {
    Q_OBJECT

public:
    ListPopup(QWidget* owner, const QString& title, bool filterable);

    void setItems(std::vector<ListPopupItem> items);
    [[nodiscard]] const std::vector<ListPopupItem>& items() const noexcept { return m_items; }
    // Selects the visible row showing item `index` (ignored for filtered-out or invalid items).
    void setCurrentItem(int index);
    // The item index of the selected row, or -1.
    [[nodiscard]] int currentItem() const;
    // Rows that pass the filter.
    [[nodiscard]] int visibleCount() const;
    // Moves the selection by `delta` visible rows, wrapping around.
    void moveSelection(int delta);
    void setFilterText(const QString& text);
    // Accepts the selected row (closes with Accepted); no-op without one.
    void acceptCurrent();
    void setAcceptOnRelease(std::optional<int> qtKey) noexcept { m_acceptOnRelease = qtKey; }

    [[nodiscard]] QLineEdit* filterField() const noexcept { return m_filter; }
    [[nodiscard]] QListWidget* list() const noexcept { return m_list; }

signals:
    void itemAccepted(int index, const QVariant& data);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyFilter();
    [[nodiscard]] bool handleKey(QKeyEvent& event);

    QLabel* m_title;
    QLineEdit* m_filter;
    QListWidget* m_list;
    std::vector<ListPopupItem> m_items;
    std::optional<int> m_acceptOnRelease;
};

/*
 * One text field with a prompt. Enter runs the validator: a message keeps the popup open and shows
 * it; nullopt emits submitted() and closes with Accepted.
 */
class InputPopup final : public Popup {
    Q_OBJECT

public:
    // Returns an error message for invalid input, nullopt for valid input.
    using Validator = std::function<std::optional<QString>(const QString&)>;

    InputPopup(QWidget* owner, const QString& prompt, const QString& initial = {});

    void setValidator(Validator validator) { m_validator = std::move(validator); }
    // Validates and submits the current text.
    void submit();
    [[nodiscard]] QLineEdit* field() const noexcept { return m_field; }
    [[nodiscard]] QLabel* errorLabel() const noexcept { return m_error; }

signals:
    void submitted(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* m_field;
    QLabel* m_error;
    Validator m_validator;
};

} // namespace frost::ui
