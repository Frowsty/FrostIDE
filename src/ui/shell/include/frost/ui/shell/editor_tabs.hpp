// SPDX-License-Identifier: Apache-2.0
//
// EditorTabs: the editor area's tab strip and the stack of open editors (PLAN.md row 55, ADR 0035):
// open (or re-activate), close, close others, close all, close all but pinned, pin, the dirty
// marker, middle-click close, an MRU order for the Ctrl+Tab switcher, and a drop-down list of all
// tabs for when they overflow the strip.
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/shell/editor_host.hpp"

#include <QTabBar>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QMenu;
class QStackedWidget;
class QToolButton;

namespace frost::ui {

/*
 * The tab bar: middle-click closes a tab (tabCloseRequested), a right click asks for the context
 * menu at that tab (contextMenuRequested). GUI thread only.
 */
class EditorTabBar final : public QTabBar {
    Q_OBJECT

public:
    explicit EditorTabBar(QWidget* parent = nullptr);

signals:
    void contextMenuRequested(int index, QPoint globalPos);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    int m_middlePressed = -1;
};

/*
 * The tabs and the editors they own. Tabs are ordered pinned first, then by opening order; the
 * MRU order (most recently activated first) drives the switcher. One tab per path: open() of an
 * open path activates its tab. Closing asks IEditorView::requestClose() first; a veto keeps the
 * tab. The factory must outlive the widget. GUI thread only. Every operation is O(tabs).
 */
class EditorTabs final : public QWidget, private IEditorHost {
    Q_OBJECT

public:
    explicit EditorTabs(IEditorFactory& factory, QWidget* parent = nullptr);
    ~EditorTabs() override;

    // Opens `path` (UTF-8) in a new tab, or activates its tab; returns the tab index. Errors:
    // the factory's.
    [[nodiscard]] Result<int> open(const std::string& path, bool activate = true);

    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_tabs.size()); }
    [[nodiscard]] int currentIndex() const;
    // -1 when `path` is not open.
    [[nodiscard]] int indexOf(const std::string& path) const;
    // nullptr for an index out of range.
    [[nodiscard]] IEditorView* editor(int index) const;
    [[nodiscard]] IEditorView* currentEditor() const { return editor(currentIndex()); }
    // The tab text as shown: the file name, with a leading '*' while dirty.
    [[nodiscard]] QString tabText(int index) const;

    void activate(int index);
    // The next or previous tab, wrapping around.
    void activateNext();
    void activatePrevious();

    // Closes one tab; false when the index is invalid or the editor vetoed.
    bool close(int index);
    // Closes every unpinned tab except `index`.
    void closeOthers(int index);
    // Closes every tab, pinned ones too.
    void closeAll();
    // Closes every unpinned tab.
    void closeAllButPinned();

    // Pins or unpins a tab; pinned tabs move to the front, keep no close button and survive
    // closeOthers and closeAllButPinned.
    void setPinned(int index, bool pinned);
    [[nodiscard]] bool isPinned(int index) const;
    [[nodiscard]] bool isDirty(int index) const;

    // Tab indexes, most recently activated first.
    [[nodiscard]] std::vector<int> mruOrder() const;

    [[nodiscard]] EditorTabBar* tabBar() const noexcept { return m_bar; }
    // The button that lists every tab (shown while the tabs overflow the strip).
    [[nodiscard]] QToolButton* tabListButton() const noexcept { return m_listButton; }
    // Builds the menu of every tab that the list button shows; exposed for tests.
    void populateTabList(QMenu& menu);
    // True while the tabs need more width than the strip has (the list button is shown).
    [[nodiscard]] bool isOverflowing() const;
    // The empty-state label shown while no tab is open.
    [[nodiscard]] QLabel* emptyState() const noexcept { return m_empty; }

signals:
    // The active editor changed (nullptr: none left).
    void currentEditorChanged(frost::ui::IEditorView* editor);
    // An editor's status changed (the active one or not).
    void statusUpdated(frost::ui::IEditorView* editor);
    // A tab was opened or closed.
    void tabCountChanged(int count);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Tab {
        std::unique_ptr<IEditorView> editor;
        bool pinned = false;
        std::uint64_t lastActivated = 0;
    };

    void editorStatusChanged(IEditorView& editor) override;
    [[nodiscard]] int indexOfEditor(const IEditorView* editor) const;
    // Re-creates the tab bar from m_tabs (texts, tooltips, close buttons, the current tab).
    void syncBar(int current);
    void updateTabText(int index);
    void onBarCurrentChanged(int index);
    void showContextMenu(int index, QPoint globalPos);
    void updateOverflow();
    // Moves tab `from` to `to` in m_tabs and the bar.
    void moveTab(int from, int to);
    [[nodiscard]] int pinnedCount() const;

    IEditorFactory& m_factory;
    EditorTabBar* m_bar;
    QToolButton* m_listButton;
    QStackedWidget* m_stack;
    QLabel* m_empty;
    std::vector<Tab> m_tabs;
    std::uint64_t m_activationClock = 0;
    bool m_syncing = false;
};

} // namespace frost::ui
