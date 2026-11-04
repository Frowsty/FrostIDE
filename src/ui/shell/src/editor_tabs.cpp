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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
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

EditorTabGroup::EditorTabGroup(IEditorFactory& factory, QWidget* parent)
    : QWidget(parent), m_factory(factory), m_bar(new EditorTabBar(this)), m_listButton(new QToolButton(this)),
      m_closeGroupButton(new QToolButton(this)), m_stack(new QStackedWidget(this)), m_empty(new QLabel(m_stack)) {
    setObjectName(QStringLiteral("editorTabs"));
    m_listButton->setObjectName(QStringLiteral("tabListButton"));
    m_listButton->setAutoRaise(true);
    m_listButton->setToolTip(text("Show All Tabs"));
    m_listButton->setPopupMode(QToolButton::InstantPopup);
    const QIcon chevron = shell_detail::themedIcon(icons::kNavChevronDown);
    if (chevron.isNull()) {
        m_listButton->setText(QStringLiteral("â–¾"));
    } else {
        m_listButton->setIcon(chevron);
    }
    auto* listMenu = new QMenu(m_listButton);
    listMenu->setObjectName(QStringLiteral("tabListMenu"));
    connect(listMenu, &QMenu::aboutToShow, this, [this, listMenu] { populateTabList(*listMenu); });
    m_listButton->setMenu(listMenu);
    m_listButton->hide();
    m_closeGroupButton->setObjectName(QStringLiteral("closeEditorGroup"));
    const QIcon closeIcon = shell_detail::themedIcon(icons::kActionsClose);
    if (closeIcon.isNull())
        m_closeGroupButton->setText(QStringLiteral("x"));
    else
        m_closeGroupButton->setIcon(closeIcon);
    m_closeGroupButton->setToolTip(text("Close Editor Group"));
    m_closeGroupButton->setAccessibleName(text("Close Editor Group"));
    m_closeGroupButton->setAutoRaise(true);
    m_closeGroupButton->hide();
    connect(m_closeGroupButton, &QToolButton::clicked, this, &EditorTabGroup::closeGroupRequested);

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
    strip->addWidget(m_closeGroupButton);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(strip);
    layout->addWidget(m_stack, 1);
    m_bar->hide();

    connect(m_bar, &QTabBar::currentChanged, this, &EditorTabGroup::onBarCurrentChanged);
    connect(m_bar, &QTabBar::tabCloseRequested, this, [this](int index) { static_cast<void>(close(index)); });
    connect(m_bar, &EditorTabBar::contextMenuRequested, this, &EditorTabGroup::showContextMenu);
}

EditorTabGroup::~EditorTabGroup() {
    // Views delete their widgets; take them out of the stack first so the stack never sees a
    // dangling child.
    for (Tab& tab : m_tabs) {
        if (QWidget* widget = tab.editor->widget(); widget != nullptr) {
            m_stack->removeWidget(widget);
        }
        tab.editor.reset();
    }
}

Result<int> EditorTabGroup::open(const std::string& requested, bool activateTab) {
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

void EditorTabGroup::setEmptyStateWidget(QWidget* widget) {
    if (widget != nullptr && m_stack->indexOf(widget) < 0) {
        m_stack->addWidget(widget);
    }
    m_emptyWidget = widget;
    if (m_tabs.empty()) {
        m_stack->setCurrentWidget(emptyStateWidget());
    }
}

QWidget* EditorTabGroup::emptyStateWidget() const noexcept {
    return m_emptyWidget != nullptr ? m_emptyWidget : static_cast<QWidget*>(m_empty);
}

int EditorTabGroup::currentIndex() const { return m_tabs.empty() ? -1 : m_bar->currentIndex(); }

int EditorTabGroup::indexOf(const std::string& path) const {
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

IEditorView* EditorTabGroup::editor(int index) const {
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return m_tabs[static_cast<std::size_t>(index)].editor.get();
}

QString EditorTabGroup::tabText(int index) const {
    const IEditorView* view = editor(index);
    if (view == nullptr) {
        return {};
    }
    const QString name = QFileInfo(fromUtf8(view->path())).fileName();
    return view->status().dirty ? QStringLiteral("*") + name : name;
}

void EditorTabGroup::activate(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    if (m_bar->currentIndex() == index) {
        onBarCurrentChanged(index);
    } else {
        m_bar->setCurrentIndex(index);
    }
}

void EditorTabGroup::activateNext() {
    if (count() > 1) {
        activate((currentIndex() + 1) % count());
    }
}

void EditorTabGroup::activatePrevious() {
    if (count() > 1) {
        activate((currentIndex() + count() - 1) % count());
    }
}

bool EditorTabGroup::close(int index) {
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
        m_stack->setCurrentWidget(emptyStateWidget());
        emit currentEditorChanged(nullptr);
    }
    emit tabCountChanged(count());
    return true;
}

void EditorTabGroup::closeOthers(int index) {
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

void EditorTabGroup::closeAll() {
    for (int i = count() - 1; i >= 0; --i) {
        static_cast<void>(close(i));
    }
}

void EditorTabGroup::closeAllButPinned() {
    for (int i = count() - 1; i >= 0; --i) {
        if (!isPinned(i)) {
            static_cast<void>(close(i));
        }
    }
}

void EditorTabGroup::setPinned(int index, bool pinned) {
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

bool EditorTabGroup::isPinned(int index) const {
    return index >= 0 && index < count() && m_tabs[static_cast<std::size_t>(index)].pinned;
}

bool EditorTabGroup::isDirty(int index) const {
    const IEditorView* view = editor(index);
    return view != nullptr && view->status().dirty;
}

std::vector<int> EditorTabGroup::mruOrder() const {
    std::vector<int> order(m_tabs.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, [this](int a, int b) {
        return m_tabs[static_cast<std::size_t>(a)].lastActivated > m_tabs[static_cast<std::size_t>(b)].lastActivated;
    });
    return order;
}

void EditorTabGroup::populateTabList(QMenu& menu) {
    menu.clear();
    for (int i = 0; i < count(); ++i) {
        QAction* action = menu.addAction(escapeMnemonics(tabText(i)));
        action->setCheckable(true);
        action->setChecked(i == currentIndex());
        IEditorView* view = editor(i);
        connect(action, &QAction::triggered, this, [this, view] { activate(indexOfEditor(view)); });
    }
}

bool EditorTabGroup::isOverflowing() const {
    return !m_tabs.empty() && m_bar->isVisible() && m_bar->sizeHint().width() > m_bar->width();
}

void EditorTabGroup::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateOverflow();
}

void EditorTabGroup::editorStatusChanged(IEditorView& view) {
    const int index = indexOfEditor(&view);
    if (index < 0) {
        return;
    }
    updateTabText(index);
    emit statusUpdated(&view);
}

int EditorTabGroup::indexOfEditor(const IEditorView* view) const {
    for (int i = 0; i < count(); ++i) {
        if (m_tabs[static_cast<std::size_t>(i)].editor.get() == view) {
            return i;
        }
    }
    return -1;
}

void EditorTabGroup::syncBar(int current) {
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

void EditorTabGroup::updateTabText(int index) {
    if (index < 0 || index >= m_bar->count()) {
        return;
    }
    const std::string& path = m_tabs[static_cast<std::size_t>(index)].editor->path();
    m_bar->setTabText(index, escapeMnemonics(tabText(index)));
    m_bar->setTabToolTip(index, QDir::toNativeSeparators(fromUtf8(path)));
    std::optional<QColor> color;
    if (m_titleColors) {
        color = m_titleColors(path);
    }
    m_bar->setTabTextColor(index, color.value_or(QColor()));
}

void EditorTabGroup::setTitleColorProvider(TitleColorProvider provider) {
    m_titleColors = std::move(provider);
    refreshTitleColors();
}

void EditorTabGroup::refreshTitleColors() {
    for (int i = 0; i < m_bar->count(); ++i) {
        updateTabText(i);
    }
}

void EditorTabGroup::setGroupCloseVisible(bool visible) { m_closeGroupButton->setVisible(visible); }

QColor EditorTabGroup::titleColor(int index) const {
    return index >= 0 && index < m_bar->count() ? m_bar->tabTextColor(index) : QColor();
}

void EditorTabGroup::onBarCurrentChanged(int index) {
    if (m_syncing) {
        return;
    }
    IEditorView* view = editor(index);
    if (view == nullptr) {
        m_stack->setCurrentWidget(emptyStateWidget());
        emit currentEditorChanged(nullptr);
        return;
    }
    m_tabs[static_cast<std::size_t>(index)].lastActivated = ++m_activationClock;
    m_stack->setCurrentWidget(view->widget());
    emit currentEditorChanged(view);
}

void EditorTabGroup::showContextMenu(int index, QPoint globalPos) {
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
    connect(menu.addAction(text("Split Right")), &QAction::triggered, this, [this, at] {
        activate(at());
        emit splitRequested(Qt::Horizontal);
    });
    connect(menu.addAction(text("Split Down")), &QAction::triggered, this, [this, at] {
        activate(at());
        emit splitRequested(Qt::Vertical);
    });
    connect(menu.addAction(text("Close Editor Group")), &QAction::triggered, this,
            [this] { emit closeGroupRequested(); });
    menu.addSeparator();
    const bool pinned = isPinned(index);
    connect(menu.addAction(pinned ? text("Unpin Tab") : text("Pin Tab")), &QAction::triggered, this,
            [this, at, pinned] { setPinned(at(), !pinned); });
    connect(menu.addAction(text("Copy Path")), &QAction::triggered, this,
            [view] { QApplication::clipboard()->setText(QDir::toNativeSeparators(fromUtf8(view->path()))); });
    menu.exec(globalPos);
}

void EditorTabGroup::updateOverflow() { m_listButton->setVisible(isOverflowing()); }

void EditorTabGroup::moveTab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= count() || to >= count()) {
        return;
    }
    Tab moving = std::move(m_tabs[static_cast<std::size_t>(from)]);
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(moving));
}

int EditorTabGroup::pinnedCount() const { return static_cast<int>(std::ranges::count_if(m_tabs, &Tab::pinned)); }

EditorTabs::EditorTabs(IEditorFactory& factory, QWidget* parent)
    : QWidget(parent), m_factory(factory), m_root(new QSplitter(Qt::Horizontal, this)) {
    setObjectName(QStringLiteral("editorTabs"));
    m_root->setObjectName(QStringLiteral("editorGroups"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_root);
    m_active = makeGroup();
    m_root->addWidget(m_active);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (!now)
            return;
        for (EditorTabGroup* group : m_groups) {
            if (group == now || group->isAncestorOf(now)) {
                setActiveGroup(group);
                break;
            }
        }
    });
}

EditorTabs::~EditorTabs() {
    disconnect(qApp, nullptr, this, nullptr);
    m_active = nullptr;
    const auto groups = std::exchange(m_groups, {});
    for (EditorTabGroup* group : groups)
        disconnect(group, nullptr, this, nullptr);
    for (EditorTabGroup* group : groups)
        delete group;
}

EditorTabGroup* EditorTabs::makeGroup() {
    auto* group = new EditorTabGroup(m_factory);
    group->setTitleColorProvider(m_titleColors);
    m_groups.push_back(group);
    connect(group, &EditorTabGroup::currentEditorChanged, this, [this, group](IEditorView* editor) {
        if (group == m_active)
            emit currentEditorChanged(editor);
    });
    connect(group, &EditorTabGroup::statusUpdated, this, &EditorTabs::statusUpdated);
    connect(group, &EditorTabGroup::tabCountChanged, this, [this](int) { emit tabCountChanged(totalCount()); });
    connect(group, &EditorTabGroup::splitRequested, this, [this, group](Qt::Orientation orientation) {
        setActiveGroup(group);
        (void)split(orientation);
    });
    connect(group, &EditorTabGroup::closeGroupRequested, this, [this, group] {
        setActiveGroup(group);
        (void)closeGroup();
    });
    return group;
}

void EditorTabs::setActiveGroup(EditorTabGroup* group) {
    if (group == m_active || std::ranges::find(m_groups, group) == m_groups.end())
        return;
    m_active = group;
    emit currentEditorChanged(group->currentEditor());
}

bool EditorTabs::split(Qt::Orientation orientation) {
    IEditorView* current = currentEditor();
    if (!current)
        return false;
    const std::string path = current->path();
    const EditorViewState state = current->viewState();
    EditorTabGroup* group = makeGroup();
    auto opened = group->open(path);
    if (!opened) {
        std::erase(m_groups, group);
        delete group;
        return false;
    }
    group->currentEditor()->restoreViewState(state);
    auto* parent = qobject_cast<QSplitter*>(m_active->parentWidget());
    if (!parent) {
        std::erase(m_groups, group);
        delete group;
        return false;
    }
    const int index = parent->indexOf(m_active);
    if (parent->count() == 1 || parent->orientation() == orientation) {
        parent->setOrientation(orientation);
        parent->insertWidget(index + 1, group);
    } else {
        auto* nested = new QSplitter(orientation);
        parent->replaceWidget(index, nested);
        nested->addWidget(m_active);
        nested->addWidget(group);
    }
    for (auto* pane : m_groups)
        pane->setGroupCloseVisible(true);
    setActiveGroup(group);
    group->currentEditor()->widget()->setFocus(Qt::OtherFocusReason);
    return true;
}

bool EditorTabs::closeGroup() {
    if (m_groups.size() <= 1)
        return false;
    EditorTabGroup* closing = m_active;
    closing->closeAll();
    if (closing->count() != 0)
        return false;
    QWidget* welcome = closing->emptyStateWidget();
    if (welcome == closing->emptyState())
        welcome = nullptr;
    const bool wasFirst = closing == m_groups.front();
    std::erase(m_groups, closing);
    if (m_groups.size() == 1)
        m_groups.front()->setGroupCloseVisible(false);
    if (wasFirst && welcome)
        m_groups.front()->setEmptyStateWidget(welcome);
    m_active = nullptr;
    setActiveGroup(m_groups.front());
    auto* branch = qobject_cast<QSplitter*>(closing->parentWidget());
    closing->hide();
    closing->setParent(this);
    closing->deleteLater();
    while (branch && branch != m_root && branch->count() <= 1) {
        auto* parent = qobject_cast<QSplitter*>(branch->parentWidget());
        if (!parent)
            break;
        const int index = parent->indexOf(branch);
        const QList<int> sizes = parent->sizes();
        if (branch->count() == 1) {
            QWidget* remaining = branch->widget(0);
            remaining->setParent(nullptr);
            parent->replaceWidget(index, remaining);
        } else {
            branch->setParent(nullptr);
        }
        branch->setParent(this);
        branch->deleteLater();
        parent->setSizes(sizes);
        branch = parent;
    }
    if (IEditorView* editor = currentEditor())
        editor->widget()->setFocus(Qt::OtherFocusReason);
    return true;
}

std::string EditorTabs::saveLayout() const {
    const std::function<QJsonObject(QWidget*)> encode = [&](QWidget* widget) -> QJsonObject {
        if (auto* group = qobject_cast<EditorTabGroup*>(widget)) {
            auto it = std::ranges::find(m_groups, group);
            return {{QStringLiteral("group"), static_cast<int>(it - m_groups.begin())}};
        }
        auto* splitter = qobject_cast<QSplitter*>(widget);
        if (!splitter)
            return {};
        QJsonArray children;
        for (int i = 0; i < splitter->count(); ++i)
            children.append(encode(splitter->widget(i)));
        QJsonArray sizes;
        for (int size : splitter->sizes())
            sizes.append(size);
        return {{QStringLiteral("vertical"), splitter->orientation() == Qt::Vertical},
                {QStringLiteral("children"), children},
                {QStringLiteral("sizes"), sizes}};
    };
    return QJsonDocument(encode(m_root)).toJson(QJsonDocument::Compact).toStdString();
}

bool EditorTabs::restoreLayout(const std::string& layout) {
    if (totalCount() != 0 || layout.empty() || layout.size() > 65536)
        return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(layout), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    std::vector<int> ids;
    const std::function<bool(const QJsonObject&, int)> valid = [&](const QJsonObject& node, int depth) {
        if (depth > 16)
            return false;
        if (node.contains(QStringLiteral("group"))) {
            const QJsonValue value = node.value(QStringLiteral("group"));
            const int id = value.toInt(-1);
            if (!value.isDouble() || id < 0 || id >= 64 || std::ranges::find(ids, id) != ids.end())
                return false;
            ids.push_back(id);
            return true;
        }
        const QJsonValue childrenValue = node.value(QStringLiteral("children"));
        if (!childrenValue.isArray())
            return false;
        const QJsonArray children = childrenValue.toArray();
        if (children.empty() || children.size() > 64)
            return false;
        for (const QJsonValue& child : children)
            if (!child.isObject() || !valid(child.toObject(), depth + 1))
                return false;
        return true;
    };
    if (!valid(document.object(), 0) || ids.empty())
        return false;
    std::ranges::sort(ids);
    for (int i = 0; i < static_cast<int>(ids.size()); ++i)
        if (ids[static_cast<std::size_t>(i)] != i)
            return false;

    QWidget* welcome = m_groups.front()->emptyStateWidget();
    if (welcome == m_groups.front()->emptyState())
        welcome = nullptr;
    if (welcome)
        welcome->setParent(this);
    m_active = nullptr;
    while (!m_groups.empty()) {
        EditorTabGroup* group = m_groups.back();
        m_groups.pop_back();
        delete group;
    }
    while (m_root->count())
        delete m_root->widget(0);
    std::vector<EditorTabGroup*> ordered(ids.size());
    const std::function<QWidget*(const QJsonObject&)> build = [&](const QJsonObject& node) -> QWidget* {
        if (node.contains(QStringLiteral("group"))) {
            auto* group = makeGroup();
            ordered[static_cast<std::size_t>(node.value(QStringLiteral("group")).toInt())] = group;
            return group;
        }
        auto* splitter = new QSplitter(node.value(QStringLiteral("vertical")).toBool() ? Qt::Vertical : Qt::Horizontal);
        for (const auto& child : node.value(QStringLiteral("children")).toArray())
            splitter->addWidget(build(child.toObject()));
        QList<int> sizes;
        for (const auto& size : node.value(QStringLiteral("sizes")).toArray())
            sizes.push_back(std::max(0, size.toInt()));
        if (sizes.size() == splitter->count())
            splitter->setSizes(sizes);
        return splitter;
    };
    const QJsonObject rootNode = document.object();
    if (rootNode.contains(QStringLiteral("group"))) {
        m_root->addWidget(build(rootNode));
    } else {
        m_root->setOrientation(rootNode.value(QStringLiteral("vertical")).toBool() ? Qt::Vertical : Qt::Horizontal);
        for (const auto& child : rootNode.value(QStringLiteral("children")).toArray())
            m_root->addWidget(build(child.toObject()));
        QList<int> sizes;
        for (const auto& size : rootNode.value(QStringLiteral("sizes")).toArray())
            sizes.push_back(std::max(0, size.toInt()));
        if (sizes.size() == m_root->count())
            m_root->setSizes(sizes);
    }
    m_groups = std::move(ordered);
    for (auto* pane : m_groups)
        pane->setGroupCloseVisible(m_groups.size() > 1);
    m_active = m_groups.front();
    if (welcome)
        m_active->setEmptyStateWidget(welcome);
    emit currentEditorChanged(nullptr);
    return true;
}

Result<int> EditorTabs::open(const std::string& path, bool activate) { return m_active->open(path, activate); }
int EditorTabs::count() const noexcept { return m_active ? m_active->count() : 0; }
int EditorTabs::totalCount() const noexcept {
    int n = 0;
    for (auto* group : m_groups)
        n += group->count();
    return n;
}
int EditorTabs::groupCount() const noexcept { return static_cast<int>(m_groups.size()); }
int EditorTabs::currentIndex() const { return m_active ? m_active->currentIndex() : -1; }
int EditorTabs::indexOf(const std::string& path) const { return m_active->indexOf(path); }
IEditorView* EditorTabs::editor(int index) const { return m_active->editor(index); }
IEditorView* EditorTabs::currentEditor() const { return m_active ? m_active->currentEditor() : nullptr; }
QString EditorTabs::tabText(int index) const { return m_active->tabText(index); }
void EditorTabs::activate(int index) { m_active->activate(index); }
void EditorTabs::activateNext() { m_active->activateNext(); }
void EditorTabs::activatePrevious() { m_active->activatePrevious(); }
bool EditorTabs::close(int index) { return m_active->close(index); }
void EditorTabs::closeOthers(int index) { m_active->closeOthers(index); }
void EditorTabs::closeAll() {
    for (auto* group : m_groups)
        group->closeAll();
}
void EditorTabs::closeAllButPinned() { m_active->closeAllButPinned(); }
void EditorTabs::setPinned(int index, bool pinned) { m_active->setPinned(index, pinned); }
bool EditorTabs::isPinned(int index) const { return m_active->isPinned(index); }
bool EditorTabs::isDirty(int index) const { return m_active->isDirty(index); }
std::vector<int> EditorTabs::mruOrder() const { return m_active->mruOrder(); }
EditorTabBar* EditorTabs::tabBar() const noexcept { return m_active->tabBar(); }
QToolButton* EditorTabs::tabListButton() const noexcept { return m_active->tabListButton(); }
void EditorTabs::populateTabList(QMenu& menu) { m_active->populateTabList(menu); }
bool EditorTabs::isOverflowing() const { return m_active->isOverflowing(); }
QLabel* EditorTabs::emptyState() const noexcept { return m_active->emptyState(); }
void EditorTabs::setEmptyStateWidget(QWidget* widget) { m_groups.front()->setEmptyStateWidget(widget); }
QWidget* EditorTabs::emptyStateWidget() const noexcept { return m_groups.front()->emptyStateWidget(); }
void EditorTabs::setTitleColorProvider(TitleColorProvider provider) {
    m_titleColors = std::move(provider);
    for (auto* group : m_groups)
        group->setTitleColorProvider(m_titleColors);
}
void EditorTabs::refreshTitleColors() {
    for (auto* group : m_groups)
        group->refreshTitleColors();
}
QColor EditorTabs::titleColor(int index) const { return m_active->titleColor(index); }

} // namespace frost::ui
