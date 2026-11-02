// SPDX-License-Identifier: Apache-2.0
//
// EditorTabs and EditorTabBar (frost/ui/shell/editor_tabs.hpp).

#include "frost/ui/shell/editor_tabs.hpp"

#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>
#include <utility>

namespace frost::ui {

using shell_detail::fromUtf8;
using shell_detail::text;

EditorTabBar::EditorTabBar(QWidget* parent) : QTabBar(parent) {
    setObjectName(QStringLiteral("editorTabBar"));
    setTabsClosable(true);
    setMovable(false);
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideNone);
    setExpanding(false);
    setDocumentMode(true);
}

void EditorTabBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        m_middlePressed = tabAt(event->position().toPoint());
        event->accept();
        return;
    }
    QTabBar::mousePressEvent(event);
}

void EditorTabBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        const int index = tabAt(event->position().toPoint());
        const int pressed = std::exchange(m_middlePressed, -1);
        if (index >= 0 && index == pressed) {
            emit tabCloseRequested(index);
        }
        event->accept();
        return;
    }
    QTabBar::mouseReleaseEvent(event);
}

void EditorTabBar::contextMenuEvent(QContextMenuEvent* event) {
    const int index = tabAt(event->pos());
    if (index >= 0) {
        emit contextMenuRequested(index, event->globalPos());
        event->accept();
    }
}

EditorTabs::EditorTabs(IEditorFactory& factory, QWidget* parent)
    : QWidget(parent), m_factory(factory), m_bar(new EditorTabBar(this)), m_listButton(new QToolButton(this)),
      m_stack(new QStackedWidget(this)), m_empty(new QLabel(m_stack)) {
    setObjectName(QStringLiteral("editorTabs"));
    m_listButton->setObjectName(QStringLiteral("tabListButton"));
    m_listButton->setAutoRaise(true);
    m_listButton->setToolTip(text("Show All Tabs"));
    m_listButton->setPopupMode(QToolButton::InstantPopup);
    const QIcon chevron = shell_detail::themedIcon(icons::kNavChevronDown);
    if (chevron.isNull()) {
        m_listButton->setText(QStringLiteral("▾"));
    } else {
        m_listButton->setIcon(chevron);
    }
    auto* listMenu = new QMenu(m_listButton);
    listMenu->setObjectName(QStringLiteral("tabListMenu"));
    connect(listMenu, &QMenu::aboutToShow, this, [this, listMenu] { populateTabList(*listMenu); });
    m_listButton->setMenu(listMenu);
    m_listButton->hide();

    // The empty state names the way in (the M0 window's placeholder label keeps its object name).
    m_empty->setObjectName(QStringLiteral("placeholder"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setText(text("No files are open"));
    m_stack->addWidget(m_empty);

    auto* strip = new QHBoxLayout();
    strip->setContentsMargins(0, 0, 0, 0);
    strip->setSpacing(0);
    strip->addWidget(m_bar, 1);
    strip->addWidget(m_listButton);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(strip);
    layout->addWidget(m_stack, 1);
    m_bar->hide();

    connect(m_bar, &QTabBar::currentChanged, this, &EditorTabs::onBarCurrentChanged);
    connect(m_bar, &QTabBar::tabCloseRequested, this, [this](int index) { static_cast<void>(close(index)); });
    connect(m_bar, &EditorTabBar::contextMenuRequested, this, &EditorTabs::showContextMenu);
}

EditorTabs::~EditorTabs() {
    // Views delete their widgets; take them out of the stack first so the stack never sees a
    // dangling child.
    for (Tab& tab : m_tabs) {
        if (QWidget* widget = tab.editor->widget(); widget != nullptr) {
            m_stack->removeWidget(widget);
        }
        tab.editor.reset();
    }
}

Result<int> EditorTabs::open(const std::string& requested, bool activateTab) {
    const std::string path = m_factory.canonicalPath(requested);
    if (const int existing = indexOf(path); existing >= 0) {
        if (activateTab) {
            activate(existing);
        }
        return existing;
    }
    auto editor = m_factory.open(path, *this);
    if (!editor) {
        return std::unexpected(std::move(editor.error()));
    }
    m_stack->addWidget((*editor)->widget());
    m_tabs.push_back(Tab{std::move(*editor), false, 0});
    const int index = count() - 1;
    syncBar(activateTab ? index : std::max(currentIndex(), 0));
    if (activateTab || count() == 1) {
        activate(activateTab ? index : 0);
    }
    emit tabCountChanged(count());
    return index;
}

int EditorTabs::currentIndex() const { return m_tabs.empty() ? -1 : m_bar->currentIndex(); }

int EditorTabs::indexOf(const std::string& path) const {
    for (int i = 0; i < count(); ++i) {
        if (m_tabs[static_cast<std::size_t>(i)].editor->path() == path) {
            return i;
        }
    }
    // Another spelling of an open document ("foo.cpp" for "Foo.cpp" on Windows).
    if (const std::string canonical = m_factory.canonicalPath(path); canonical != path) {
        for (int i = 0; i < count(); ++i) {
            if (m_tabs[static_cast<std::size_t>(i)].editor->path() == canonical) {
                return i;
            }
        }
    }
    return -1;
}

IEditorView* EditorTabs::editor(int index) const {
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return m_tabs[static_cast<std::size_t>(index)].editor.get();
}

QString EditorTabs::tabText(int index) const {
    const IEditorView* view = editor(index);
    if (view == nullptr) {
        return {};
    }
    const QString name = QFileInfo(fromUtf8(view->path())).fileName();
    return view->status().dirty ? QStringLiteral("*") + name : name;
}

void EditorTabs::activate(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    if (m_bar->currentIndex() == index) {
        onBarCurrentChanged(index);
    } else {
        m_bar->setCurrentIndex(index);
    }
}

void EditorTabs::activateNext() {
    if (count() > 1) {
        activate((currentIndex() + 1) % count());
    }
}

void EditorTabs::activatePrevious() {
    if (count() > 1) {
        activate((currentIndex() + count() - 1) % count());
    }
}

bool EditorTabs::close(int index) {
    IEditorView* view = editor(index);
    if (view == nullptr || !view->requestClose()) {
        return false;
    }
    const bool wasCurrent = index == currentIndex();
    std::unique_ptr<IEditorView> closing = std::move(m_tabs[static_cast<std::size_t>(index)].editor);
    m_tabs.erase(m_tabs.begin() + index);
    m_stack->removeWidget(closing->widget());
    closing.reset();

    // The most recently used remaining tab takes over, as in the IntelliJ platform.
    int next = -1;
    if (!m_tabs.empty()) {
        const std::vector<int> order = mruOrder();
        next = wasCurrent ? order.front()
                          : std::clamp(m_bar->currentIndex() - (m_bar->currentIndex() > index ? 1 : 0), 0, count() - 1);
    }
    syncBar(next);
    if (next >= 0) {
        activate(next);
    } else {
        m_stack->setCurrentWidget(m_empty);
        emit currentEditorChanged(nullptr);
    }
    emit tabCountChanged(count());
    return true;
}

void EditorTabs::closeOthers(int index) {
    IEditorView* keep = editor(index);
    if (keep == nullptr) {
        return;
    }
    for (int i = count() - 1; i >= 0; --i) {
        if (editor(i) != keep && !isPinned(i)) {
            static_cast<void>(close(i));
        }
    }
    activate(indexOfEditor(keep));
}

void EditorTabs::closeAll() {
    for (int i = count() - 1; i >= 0; --i) {
        static_cast<void>(close(i));
    }
}

void EditorTabs::closeAllButPinned() {
    for (int i = count() - 1; i >= 0; --i) {
        if (!isPinned(i)) {
            static_cast<void>(close(i));
        }
    }
}

void EditorTabs::setPinned(int index, bool pinned) {
    if (index < 0 || index >= count() || isPinned(index) == pinned) {
        return;
    }
    const IEditorView* current = currentEditor();
    const int pinnedBefore = pinnedCount();
    m_tabs[static_cast<std::size_t>(index)].pinned = pinned;
    // Pinning moves the tab to the end of the pinned group; unpinning to the start of the others.
    moveTab(index, pinned ? pinnedBefore : pinnedBefore - 1);
    syncBar(indexOfEditor(current));
}

bool EditorTabs::isPinned(int index) const {
    return index >= 0 && index < count() && m_tabs[static_cast<std::size_t>(index)].pinned;
}

bool EditorTabs::isDirty(int index) const {
    const IEditorView* view = editor(index);
    return view != nullptr && view->status().dirty;
}

std::vector<int> EditorTabs::mruOrder() const {
    std::vector<int> order(m_tabs.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, [this](int a, int b) {
        return m_tabs[static_cast<std::size_t>(a)].lastActivated > m_tabs[static_cast<std::size_t>(b)].lastActivated;
    });
    return order;
}

void EditorTabs::populateTabList(QMenu& menu) {
    menu.clear();
    for (int i = 0; i < count(); ++i) {
        QAction* action = menu.addAction(escapeMnemonics(tabText(i)));
        action->setCheckable(true);
        action->setChecked(i == currentIndex());
        IEditorView* view = editor(i);
        connect(action, &QAction::triggered, this, [this, view] { activate(indexOfEditor(view)); });
    }
}

bool EditorTabs::isOverflowing() const {
    return !m_tabs.empty() && m_bar->isVisible() && m_bar->sizeHint().width() > m_bar->width();
}

void EditorTabs::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateOverflow();
}

void EditorTabs::editorStatusChanged(IEditorView& view) {
    const int index = indexOfEditor(&view);
    if (index < 0) {
        return;
    }
    updateTabText(index);
    emit statusUpdated(&view);
}

int EditorTabs::indexOfEditor(const IEditorView* view) const {
    for (int i = 0; i < count(); ++i) {
        if (m_tabs[static_cast<std::size_t>(i)].editor.get() == view) {
            return i;
        }
    }
    return -1;
}

void EditorTabs::syncBar(int current) {
    m_syncing = true;
    while (m_bar->count() > 0) {
        m_bar->removeTab(m_bar->count() - 1);
    }
    const QIcon pin = shell_detail::themedIcon(icons::kActionsPin);
    for (int i = 0; i < count(); ++i) {
        m_bar->addTab(QString());
        updateTabText(i);
        const Tab& tab = m_tabs[static_cast<std::size_t>(i)];
        m_bar->setTabToolTip(i, QDir::toNativeSeparators(fromUtf8(tab.editor->path())));
        m_bar->setTabIcon(i, tab.pinned ? pin : QIcon());
        for (const QTabBar::ButtonPosition side : {QTabBar::LeftSide, QTabBar::RightSide}) {
            if (QWidget* button = m_bar->tabButton(i, side); button != nullptr) {
                button->setVisible(!tab.pinned);
            }
        }
    }
    if (current >= 0 && current < count()) {
        m_bar->setCurrentIndex(current);
    }
    m_bar->setVisible(count() > 0);
    m_syncing = false;
    updateOverflow();
}

void EditorTabs::updateTabText(int index) {
    if (index < 0 || index >= m_bar->count()) {
        return;
    }
    m_bar->setTabText(index, escapeMnemonics(tabText(index)));
}

void EditorTabs::onBarCurrentChanged(int index) {
    if (m_syncing) {
        return;
    }
    IEditorView* view = editor(index);
    if (view == nullptr) {
        m_stack->setCurrentWidget(m_empty);
        emit currentEditorChanged(nullptr);
        return;
    }
    m_tabs[static_cast<std::size_t>(index)].lastActivated = ++m_activationClock;
    m_stack->setCurrentWidget(view->widget());
    emit currentEditorChanged(view);
}

void EditorTabs::showContextMenu(int index, QPoint globalPos) {
    IEditorView* view = editor(index);
    if (view == nullptr) {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("editorTabContextMenu"));
    const auto at = [this, view] { return indexOfEditor(view); };
    connect(menu.addAction(text("Close")), &QAction::triggered, this, [this, at] { static_cast<void>(close(at())); });
    connect(menu.addAction(text("Close Other Tabs")), &QAction::triggered, this, [this, at] { closeOthers(at()); });
    connect(menu.addAction(text("Close All Tabs")), &QAction::triggered, this, [this] { closeAll(); });
    connect(menu.addAction(text("Close All but Pinned")), &QAction::triggered, this, [this] { closeAllButPinned(); });
    menu.addSeparator();
    const bool pinned = isPinned(index);
    connect(menu.addAction(pinned ? text("Unpin Tab") : text("Pin Tab")), &QAction::triggered, this,
            [this, at, pinned] { setPinned(at(), !pinned); });
    connect(menu.addAction(text("Copy Path")), &QAction::triggered, this,
            [view] { QApplication::clipboard()->setText(QDir::toNativeSeparators(fromUtf8(view->path()))); });
    menu.exec(globalPos);
}

void EditorTabs::updateOverflow() { m_listButton->setVisible(isOverflowing()); }

void EditorTabs::moveTab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= count() || to >= count()) {
        return;
    }
    Tab moving = std::move(m_tabs[static_cast<std::size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(moving));
}

int EditorTabs::pinnedCount() const { return static_cast<int>(std::ranges::count_if(m_tabs, &Tab::pinned)); }

} // namespace frost::ui
