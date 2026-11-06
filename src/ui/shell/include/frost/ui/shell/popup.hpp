// SPDX-License-Identifier: Apache-2.0
//
// The popup framework (PLAN.md row 241, ADR 0038): Popup, a lightweight keyboard-first popup
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

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;

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
    // A disabled row is shown greyed and cannot be accepted (Find Action's disabled actions).
    bool enabled = true;
    // A section header ("Files", "Actions"): shown in bold, never selected or accepted.
    bool header = false;
};

/*
 * A list with an optional filter field. Typing filters rows by a case-insensitive substring of
 * the text (FilterMode::Substring), or leaves the rows to the owner, who listens to
 * filterTextChanged() and calls setItems() with its own ranking (FilterMode::External: Find
 * Action, Go to File, ADR 0042). Up, Down, Page Up and Page Down move the selection (wrapping at the
 * ends, skipping headers); Enter accepts the selected row unless it is disabled. With
 * acceptOnRelease(key) set (the switcher's Ctrl), releasing that key accepts too. itemAccepted()
 * carries the index into the items given to setItems().
 */
class ListPopup final : public Popup {
    Q_OBJECT

public:
    enum class FilterMode : std::uint8_t { Substring, External };

    ListPopup(QWidget* owner, const QString& title, bool filterable);

    void setFilterMode(FilterMode mode);
    [[nodiscard]] FilterMode filterMode() const noexcept { return m_mode; }
    // The title label (Search Everywhere shows the scope there).
    void setTitle(const QString& title);
    // Optional Rider-style scope tabs bar (Search Everywhere: All / Classes / Files / Symbols / Actions).
    void setScopeTabs(const QStringList& tabs, int activeIndex = 0);
    void setActiveScopeTab(int activeIndex);
    [[nodiscard]] int activeScopeTab() const noexcept { return m_activeScopeTab; }

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
    // The filter text changed (both modes; in External mode the owner re-ranks).
    void filterTextChanged(const QString& text);
    // Tab or Shift+Tab was pressed in the popup (Search Everywhere category cycling).
    void tabPressed(bool backward);
    // A scope tab pill in the header was clicked.
    void scopeTabClicked(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyFilter();
    [[nodiscard]] bool handleKey(QKeyEvent& event);
    [[nodiscard]] bool selectable(int row) const;

    QLabel* m_title;
    QWidget* m_scopeBar = nullptr;
    QHBoxLayout* m_scopeLayout = nullptr;
    std::vector<QToolButton*> m_scopeButtons;
    int m_activeScopeTab = -1;
    QLineEdit* m_filter;
    QListWidget* m_list;
    std::vector<ListPopupItem> m_items;
    std::optional<int> m_acceptOnRelease;
    FilterMode m_mode = FilterMode::Substring;
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
