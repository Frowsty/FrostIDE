// SPDX-License-Identifier: Apache-2.0
//
// SolutionExplorer (frost/ui/toolwindows/solution_explorer.hpp): the tree view over
// SolutionTreeModel through SolutionFilterModel, speed search, activation and the context menu.

#include "frost/ui/toolwindows/solution_explorer.hpp"

#include "frost/build/msbuild_driver.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/file_manager.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "tool_icons.hpp"
#include <QAction>
#include <QClipboard>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

namespace frost::ui {
namespace {

using project::NodeId;
using project::NodeKind;

std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

bool isProjectKind(NodeKind kind) {
    return kind == NodeKind::Project || kind == NodeKind::UnavailableProject || kind == NodeKind::UnsupportedProject;
}

} // namespace

SolutionExplorer::SolutionExplorer(QWidget* parent)
    : QWidget(parent), m_view(new QTreeView(this)), m_searchField(new QLineEdit(this)),
      m_model(new SolutionTreeModel(this)), m_filter(new SolutionFilterModel(this)) {
    setObjectName(QStringLiteral("solutionExplorer"));
    m_filter->setSourceModel(m_model);

    m_searchField->setObjectName(QStringLiteral("speedSearch"));
    m_searchField->setReadOnly(true);
    m_searchField->setFocusPolicy(Qt::NoFocus);
    m_searchField->hide();

    m_view->setObjectName(QStringLiteral("solutionTree"));
    m_view->setModel(m_filter);
    m_view->setItemDelegate(new SolutionItemDelegate(m_view));
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    m_view->setAnimated(false);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_searchField);
    layout->addWidget(m_view);

    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) { activate(index, false); });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        endSpeedSearch();
        QMenu* menu = createContextMenu(m_view->indexAt(position), this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_view->viewport()->mapToGlobal(position));
    });
}

SolutionExplorer::~SolutionExplorer() = default;

void SolutionExplorer::setController(BuildController* controller) {
    if (m_solutionConnection) {
        disconnect(m_solutionConnection);
    }
    m_controller = controller;
    if (m_controller == nullptr) {
        return;
    }
    m_solutionConnection = connect(m_controller, &BuildController::solutionChanged, this,
                                   [this] { setSolution(m_controller->solution()); });
    setSolution(m_controller->solution());
}

void SolutionExplorer::setSolution(std::shared_ptr<const project::SolutionModel> solution, bool preserveSearch) {
    const std::shared_ptr<const project::SolutionModel>& old = m_model->solution();
    if (old == solution) {
        return;
    }
    const bool reload = old && solution && old->path() == solution->path();
    const bool restoreSearch = preserveSearch && reload && m_searchActive;
    const QString query = m_query;
    const QString filterQuery = m_filter->query();
    const QStringList expandedBeforeSearch = m_expandedBeforeSearch;
    QStringList expanded;
    QString selectedKey;
    if (reload) {
        expanded = m_searchActive ? m_expandedBeforeSearch : expandedKeys();
        selectedKey = m_view->currentIndex().data(SolutionTreeModel::KeyRole).toString();
    }
    // A running search is dropped without restoring: its nodes belong to the old model.
    m_searchActive = false;
    m_query.clear();
    m_matches.clear();
    m_searchField->hide();
    m_filter->setQuery({});

    m_model->setSolution(std::move(solution));
    const auto& current = m_model->solution();
    if (!current) {
        return;
    }
    if (reload) {
        restoreExpansion(expanded);
        if (restoreSearch) {
            m_searchActive = true;
            m_expandedBeforeSearch = expandedBeforeSearch;
            m_searchField->show();
            if (!filterQuery.isEmpty())
                updateSpeedSearch(filterQuery);
            if (query != filterQuery)
                updateSpeedSearch(query);
            if (const auto node = current->findNode(toUtf8(selectedKey))) {
                const auto match = std::ranges::find(m_matches, *node);
                if (match != m_matches.end()) {
                    m_matchIndex = static_cast<std::size_t>(match - m_matches.begin());
                    selectNode(*node);
                }
            }
        } else if (const auto node = current->findNode(toUtf8(selectedKey))) {
            selectNode(*node);
        }
    } else {
        m_view->expand(proxyIndexOf(project::SolutionModel::root()));
    }
}

std::shared_ptr<const project::SolutionModel> SolutionExplorer::solution() const { return m_model->solution(); }

QModelIndex SolutionExplorer::proxyIndexOf(NodeId id) const { return m_filter->mapFromSource(m_model->indexOf(id)); }

void SolutionExplorer::selectNode(NodeId id) {
    const QModelIndex index = proxyIndexOf(id);
    if (!index.isValid()) {
        return;
    }
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        m_view->expand(parent);
    }
    m_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(index);
}

bool SolutionExplorer::selectPath(std::string_view path) {
    const QModelIndex source = m_model->indexOfPath(path);
    if (!source.isValid()) {
        return false;
    }
    endSpeedSearch();
    selectNode(*m_model->nodeOf(source));
    return true;
}

std::optional<NodeId> SolutionExplorer::currentNode() const {
    return m_model->nodeOf(m_filter->mapToSource(m_view->currentIndex()));
}

std::optional<project::ProjectId> SolutionExplorer::currentProject() const {
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid() ||
        static_cast<NodeKind>(index.data(SolutionTreeModel::KindRole).toInt()) != NodeKind::Project) {
        return std::nullopt;
    }
    const int value = index.data(SolutionTreeModel::ProjectIdRole).toInt();
    if (value < 0) {
        return std::nullopt;
    }
    return project::ProjectId{static_cast<std::uint32_t>(value)};
}

QString SolutionExplorer::currentPath() const {
    return m_view->currentIndex().data(SolutionTreeModel::PathRole).toString();
}

QString SolutionExplorer::currentDirectory() const {
    const auto solution = m_model->solution();
    const auto nodeId = currentNode();
    if (!solution || !nodeId) {
        return {};
    }
    const auto& item = solution->node(*nodeId);
    if (item.kind == NodeKind::Solution || item.kind == NodeKind::SolutionFolder) {
        return QString::fromStdString(solution->directory());
    }
    if (item.kind == NodeKind::Filter && !item.path.empty()) {
        return QString::fromStdString(item.path);
    }
    if (item.kind == NodeKind::File && !item.path.empty()) {
        return QString::fromStdString(std::string(io::parentPath(item.path)));
    }
    if (item.project) {
        return QString::fromStdString(std::string(io::parentPath(solution->project(*item.project).path)));
    }
    if (!item.path.empty()) {
        return QString::fromStdString(std::string(io::parentPath(item.path)));
    }
    return QString::fromStdString(solution->directory());
}

QStringList SolutionExplorer::expandedKeys() const {
    QStringList keys;
    for (const NodeId id : m_model->displayOrder()) {
        const QModelIndex index = proxyIndexOf(id);
        if (index.isValid() && m_view->isExpanded(index)) {
            keys.push_back(index.data(SolutionTreeModel::KeyRole).toString());
        }
    }
    return keys;
}

void SolutionExplorer::restoreExpansion(const QStringList& keys) {
    const auto& solution = m_model->solution();
    if (!solution) {
        return;
    }
    m_view->collapseAll();
    for (const QString& key : keys) {
        if (const auto node = solution->findNode(toUtf8(key))) {
            m_view->expand(proxyIndexOf(*node));
        }
    }
}

// ---- Activation and keys.

void SolutionExplorer::activate(const QModelIndex& index, bool fromKeyboard) {
    if (!index.isValid()) {
        return;
    }
    const auto kind = static_cast<NodeKind>(index.data(SolutionTreeModel::KindRole).toInt());
    if (kind == NodeKind::File) {
        const QString path = index.data(SolutionTreeModel::PathRole).toString();
        if (!path.isEmpty()) {
            emit openFile(path);
        }
    } else if (fromKeyboard) {
        m_view->setExpanded(index, !m_view->isExpanded(index));
    }
}

bool SolutionExplorer::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::ShortcutOverride && !m_searchActive) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        if ((key->key() == Qt::Key_F2 && mods == Qt::NoModifier) ||
            (key->key() == Qt::Key_F6 && mods == Qt::ShiftModifier) ||
            (key->key() == Qt::Key_Delete && mods == Qt::NoModifier)) {
            event->accept();
            return true;
        }
    }
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        return handleKey(*static_cast<QKeyEvent*>(event));
    }
    if (watched == m_view && event->type() == QEvent::FocusOut) {
        endSpeedSearch();
    } else if (watched == m_view->viewport() && event->type() == QEvent::MouseButtonPress) {
        endSpeedSearch();
    }
    return QWidget::eventFilter(watched, event);
}

bool SolutionExplorer::handleKey(QKeyEvent& event) {
    switch (event.key()) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock: return false; // Modifier presses neither start nor end a search.
    default: break;
    }
    const Qt::KeyboardModifiers modifiers = event.modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    const QString text = event.text();
    // Ctrl+Alt with text is AltGr on Windows ('@' on a German layout), so it types too.
    const bool typing = modifiers == Qt::NoModifier || modifiers == (Qt::ControlModifier | Qt::AltModifier);
    const bool printable = typing && !text.isEmpty() && text.at(0).isPrint();
    const bool enter = event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter;
    if (m_searchActive) {
        switch (event.key()) {
        case Qt::Key_Escape: endSpeedSearch(); return true;
        case Qt::Key_Backspace: {
            QString shorter = m_query;
            shorter.chop(1);
            if (shorter.isEmpty()) {
                endSpeedSearch();
            } else {
                updateSpeedSearch(shorter);
            }
            return true;
        }
        case Qt::Key_Up: stepMatch(-1); return true;
        case Qt::Key_Down: stepMatch(1); return true;
        default: break;
        }
        if (enter) {
            endSpeedSearch();
            activate(m_view->currentIndex(), true);
            return true;
        }
        if (printable) {
            updateSpeedSearch(m_query + text);
            return true;
        }
        endSpeedSearch(); // Any other key ends the search and then reaches the tree.
        return false;
    }
    if (printable && text != QStringLiteral(" ")) {
        beginSpeedSearch();
        updateSpeedSearch(text);
        return true;
    }
    if (enter && modifiers == Qt::NoModifier) {
        activate(m_view->currentIndex(), true);
        return true;
    }
    const Qt::KeyboardModifiers rawMods = event.modifiers() & ~Qt::KeypadModifier;
    if ((event.key() == Qt::Key_F2 && rawMods == Qt::NoModifier) ||
        (event.key() == Qt::Key_F6 && rawMods == Qt::ShiftModifier)) {
        const QModelIndex idx = m_view->currentIndex();
        if (idx.isValid()) {
            const auto kind = static_cast<NodeKind>(idx.data(SolutionTreeModel::KindRole).toInt());
            const QString path = idx.data(SolutionTreeModel::PathRole).toString();
            if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty()) {
                emit renameRequested(path, kind == NodeKind::Filter);
            }
        }
        return true;
    }
    if (event.key() == Qt::Key_Delete && rawMods == Qt::NoModifier) {
        const QModelIndex idx = m_view->currentIndex();
        if (idx.isValid()) {
            const auto kind = static_cast<NodeKind>(idx.data(SolutionTreeModel::KindRole).toInt());
            const QString path = idx.data(SolutionTreeModel::PathRole).toString();
            if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty()) {
                emit deleteRequested(path, kind == NodeKind::Filter);
            }
        }
        return true;
    }
    return false;
}

// ---- Speed search.

void SolutionExplorer::beginSpeedSearch() {
    if (m_searchActive || !m_model->solution()) {
        return;
    }
    m_expandedBeforeSearch = expandedKeys();
    m_searchActive = true;
    m_matchIndex = 0;
    m_searchField->show();
}

void SolutionExplorer::updateSpeedSearch(const QString& query) {
    if (!m_searchActive) {
        return;
    }
    const std::optional<NodeId> previous = m_matches.empty() ? currentNode() : m_matches[m_matchIndex];
    std::vector<NodeId> matches;
    const auto& solution = m_model->solution();
    for (const NodeId id : m_model->displayOrder()) {
        const std::string& name = solution->node(id).displayName;
        if (ui::speedSearchMatches(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())), query)) {
            matches.push_back(id);
        }
    }
    m_query = query;
    m_searchField->setText(query);
    QPalette palette = m_searchField->palette();
    palette.setColor(QPalette::Text, matches.empty() ? detail::iconRoleColor(IconRole::Error, QColor(0xd0, 0x30, 0x30))
                                                     : this->palette().color(QPalette::Text));
    m_searchField->setPalette(palette);
    if (matches.empty()) {
        return; // Keep the last matches shown (header comment).
    }
    m_matches = std::move(matches);
    const auto kept = previous ? std::ranges::find(m_matches, *previous) : m_matches.end();
    m_matchIndex = kept != m_matches.end() ? static_cast<std::size_t>(kept - m_matches.begin()) : 0;
    m_filter->setQuery(query);
    m_view->expandAll();
    selectNode(m_matches[m_matchIndex]);
}

void SolutionExplorer::stepMatch(int delta) {
    if (m_matches.empty()) {
        return;
    }
    const auto count = static_cast<std::ptrdiff_t>(m_matches.size());
    const auto next = ((static_cast<std::ptrdiff_t>(m_matchIndex) + delta) % count + count) % count;
    m_matchIndex = static_cast<std::size_t>(next);
    selectNode(m_matches[m_matchIndex]);
}

void SolutionExplorer::endSpeedSearch() {
    if (!m_searchActive) {
        return;
    }
    const std::optional<NodeId> selected = m_matches.empty() ? currentNode() : m_matches[m_matchIndex];
    m_searchActive = false;
    m_query.clear();
    m_matches.clear();
    m_searchField->clear();
    m_searchField->hide();
    m_filter->setQuery({});
    restoreExpansion(m_expandedBeforeSearch);
    m_expandedBeforeSearch.clear();
    if (selected) {
        selectNode(*selected);
    }
}

// ---- The context menu.

QMenu* SolutionExplorer::createContextMenu(const QModelIndex& index, QWidget* parent) {
    auto* menu = new QMenu(parent);
    const QModelIndex source = m_filter->mapToSource(index);
    const std::optional<NodeId> node = m_model->nodeOf(source);
    if (!node) {
        return menu;
    }
    const auto kind = static_cast<NodeKind>(source.data(SolutionTreeModel::KindRole).toInt());
    const QString path = source.data(SolutionTreeModel::PathRole).toString();
    detail::IconCache icons;

    if (kind == NodeKind::File) {
        QAction* open = menu->addAction(tr("Open"));
        open->setObjectName(QStringLiteral("openFile"));
        connect(open, &QAction::triggered, this, [this, path] { emit openFile(path); });
        const auto solution = m_model->solution();
        const auto& item = solution->node(*node);
        if (m_removeProjectFileHandler && item.project && !item.path.empty() &&
            solution->project(*item.project).kind == project::ProjectKind::Cpp) {
            const QString projectPath = QString::fromStdString(solution->project(*item.project).path);
            const QString key = QString::fromStdString(item.key);
            QAction* remove = menu->addAction(tr("Remove from Project"));
            remove->setObjectName(QStringLiteral("removeFromProject"));
            connect(remove, &QAction::triggered, this, [this, key, path, projectPath, solution] {
                const auto current = m_model->solution();
                if (current != solution || !m_removeProjectFileHandler)
                    return;
                const auto id = current->findNode(toUtf8(key));
                if (!id)
                    return;
                const auto& selected = current->node(*id);
                if (selected.kind != NodeKind::File || !selected.project ||
                    QString::fromStdString(selected.path) != path ||
                    QString::fromStdString(current->project(*selected.project).path) != projectPath)
                    return;
                m_removeProjectFileHandler(projectPath, path);
            });
        }
        menu->addSeparator();
    }

    if (kind == NodeKind::Solution || isProjectKind(kind)) {
        const bool solutionNode = kind == NodeKind::Solution;
        const int projectValue = source.data(SolutionTreeModel::ProjectIdRole).toInt();
        std::optional<project::ProjectId> projectId;
        if (!solutionNode && projectValue >= 0) {
            projectId = project::ProjectId{static_cast<std::uint32_t>(projectValue)};
        }
        const bool enabled =
            m_controller != nullptr && m_controller->canBuild() && (solutionNode || kind == NodeKind::Project);
        struct Command {
            build::BuildAction action;
            QString text;
            const char* name;
        };
        const Command commands[] = {
            {build::BuildAction::Build, solutionNode ? tr("Build Solution") : tr("Build"),
             solutionNode ? "buildSolution" : "buildProject"},
            {build::BuildAction::Rebuild, solutionNode ? tr("Rebuild Solution") : tr("Rebuild"),
             solutionNode ? "rebuildSolution" : "rebuildProject"},
            {build::BuildAction::Clean, solutionNode ? tr("Clean Solution") : tr("Clean"),
             solutionNode ? "cleanSolution" : "cleanProject"},
        };
        for (const Command& command : commands) {
            QAction* action = menu->addAction(command.text);
            action->setObjectName(QString::fromLatin1(command.name));
            action->setEnabled(enabled);
            if (command.action == build::BuildAction::Build) {
                action->setIcon(icons.get(icons::kBuildBuild));
            }
            connect(action, &QAction::triggered, this, [this, kindAction = command.action, projectId] {
                if (m_controller != nullptr) {
                    static_cast<void>(m_controller->build(kindAction, projectId)); // Failures: requestFailed().
                }
            });
        }
        if (!solutionNode && projectId && m_startupHandler) {
            // Visual Studio's and Rider's Set as Startup Project (the run service decides what can run).
            QAction* startup = menu->addAction(tr("Set as Startup Project"));
            startup->setObjectName(QStringLiteral("setStartupProject"));
            startup->setEnabled(kind == NodeKind::Project);
            connect(startup, &QAction::triggered, this, [this, id = *projectId] {
                if (m_startupHandler) {
                    m_startupHandler(id);
                }
            });
        }
        if (kind == NodeKind::Project && path.endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive) &&
            m_addNewProjectFileHandler) {
            const auto solution = m_model->solution();
            QAction* addNew = menu->addAction(tr("Add New C/C++ Source..."));
            addNew->setObjectName(QStringLiteral("addNewProjectFile"));
            connect(addNew, &QAction::triggered, this, [this, solution, path] {
                if (m_model->solution() == solution && m_addNewProjectFileHandler)
                    m_addNewProjectFileHandler(path);
            });
        }
        if (kind == NodeKind::Project && path.endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive) &&
            m_addProjectFileHandler) {
            const auto solution = m_model->solution();
            QAction* add = menu->addAction(tr("Add Existing C/C++ Source"));
            add->setObjectName(QStringLiteral("addExistingProjectFile"));
            connect(add, &QAction::triggered, this, [this, solution, path] {
                if (m_model->solution() == solution && m_addProjectFileHandler)
                    m_addProjectFileHandler(path);
            });
        }
        if (kind == NodeKind::Project && m_recoveryAvailable && m_recoverProjectHandler && m_recoveryAvailable(path)) {
            const auto solution = m_model->solution();
            QAction* recover = menu->addAction(tr("Recover Project Edit"));
            recover->setObjectName(QStringLiteral("recoverProjectEdit"));
            connect(recover, &QAction::triggered, this, [this, solution, path] {
                if (m_model->solution() == solution && m_recoveryAvailable && m_recoverProjectHandler &&
                    m_recoveryAvailable(path))
                    m_recoverProjectHandler(path);
            });
        }
        menu->addSeparator();
    }

    if ((m_newFileHandler || m_newFolderHandler) && m_model->solution()) {
        const auto solution = m_model->solution();
        const auto& item = solution->node(*node);
        QString targetDir;
        if (item.kind == NodeKind::Solution || item.kind == NodeKind::SolutionFolder) {
            targetDir = QString::fromStdString(solution->directory());
        } else if (item.kind == NodeKind::Filter && !item.path.empty()) {
            targetDir = QString::fromStdString(item.path);
        } else if (item.kind == NodeKind::File && !item.path.empty()) {
            targetDir = QString::fromStdString(std::string(io::parentPath(item.path)));
        } else if (item.project) {
            targetDir = QString::fromStdString(std::string(io::parentPath(solution->project(*item.project).path)));
        } else if (!item.path.empty()) {
            targetDir = QString::fromStdString(std::string(io::parentPath(item.path)));
        }
        if (!targetDir.isEmpty()) {
            if (m_newFileHandler) {
                QAction* newFile = menu->addAction(tr("New File..."));
                newFile->setObjectName(QStringLiteral("newFile"));
                connect(newFile, &QAction::triggered, this, [this, targetDir] {
                    if (m_newFileHandler)
                        m_newFileHandler(targetDir);
                });
            }
            if (m_newFolderHandler) {
                QAction* newFolder = menu->addAction(tr("New Folder..."));
                newFolder->setObjectName(QStringLiteral("newFolder"));
                connect(newFolder, &QAction::triggered, this, [this, targetDir] {
                    if (m_newFolderHandler)
                        m_newFolderHandler(targetDir);
                });
            }
            menu->addSeparator();
        }
    }

    if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty()) {
        const bool isDir = kind == NodeKind::Filter;
        QAction* renameAct = menu->addAction(tr("Rename..."));
        renameAct->setObjectName(QStringLiteral("renameItem"));
        connect(renameAct, &QAction::triggered, this, [this, path, isDir] { emit renameRequested(path, isDir); });
        QAction* deleteAct = menu->addAction(tr("Delete..."));
        deleteAct->setObjectName(QStringLiteral("deleteItem"));
        connect(deleteAct, &QAction::triggered, this, [this, path, isDir] { emit deleteRequested(path, isDir); });
        menu->addSeparator();
    }

    if (m_terminalHandler && !path.isEmpty()) {
        QAction* terminal = menu->addAction(tr("Open in Terminal"));
        terminal->setObjectName(QStringLiteral("openInTerminal"));
        connect(terminal, &QAction::triggered, this, [this, path] {
            if (m_terminalHandler)
                m_terminalHandler(path);
        });
    }
    QAction* reveal = menu->addAction(tr("Open Containing Folder"));
    reveal->setObjectName(QStringLiteral("openContainingFolder"));
    reveal->setEnabled(m_launcher != nullptr && !path.isEmpty());
    connect(reveal, &QAction::triggered, this, [this, path] { openContainingFolder(path); });

    QAction* copy = menu->addAction(tr("Copy Path"));
    copy->setObjectName(QStringLiteral("copyPath"));
    copy->setIcon(icons.get(icons::kEditCopy));
    copy->setEnabled(!path.isEmpty());
    connect(copy, &QAction::triggered, this, [path] { QGuiApplication::clipboard()->setText(path); });
    return menu;
}

void SolutionExplorer::openContainingFolder(const QString& path) {
    if (m_launcher == nullptr || path.isEmpty()) {
        return;
    }
    auto revealed = revealInFileManager(*m_launcher, toUtf8(path), false);
    if (!revealed) {
        const std::string& message = revealed.error().message();
        emit commandFailed(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    }
}

} // namespace frost::ui
