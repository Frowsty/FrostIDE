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
#include <QComboBox>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIODevice>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QTreeView>
#include <QUrl>
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
    : QWidget(parent), m_hierarchyCombo(new QComboBox(this)), m_view(new QTreeView(this)),
      m_searchField(new QLineEdit(this)), m_model(new SolutionTreeModel(this)), m_filter(new SolutionFilterModel(this)) {
    setObjectName(QStringLiteral("solutionExplorer"));
    m_filter->setSourceModel(m_model);

    auto* headerBar = new QWidget(this);
    headerBar->setObjectName(QStringLiteral("solutionExplorerHeader"));
    auto* headerLayout = new QHBoxLayout(headerBar);
    headerLayout->setContentsMargins(6, 3, 6, 3);
    headerLayout->setSpacing(6);
    auto* viewLabel = new QLabel(tr("View:"), headerBar);
    m_hierarchyCombo->setObjectName(QStringLiteral("cppHierarchyCombo"));
    m_hierarchyCombo->addItem(tr("Filters & Folders"),
                              static_cast<int>(project::CppHierarchyMode::FiltersAndFolders));
    m_hierarchyCombo->addItem(tr("Filters Only"), static_cast<int>(project::CppHierarchyMode::Filters));
    m_hierarchyCombo->addItem(tr("Folder Structure"),
                              static_cast<int>(project::CppHierarchyMode::PhysicalFolders));
    m_hierarchyCombo->setCurrentIndex(0);
    headerLayout->addWidget(viewLabel);
    headerLayout->addWidget(m_hierarchyCombo, 1);

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
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setDragEnabled(true);
    m_view->setAcceptDrops(true);
    m_view->setDropIndicatorShown(true);
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    m_view->setDefaultDropAction(Qt::MoveAction);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(headerBar);
    layout->addWidget(m_searchField);
    layout->addWidget(m_view);

    connect(m_hierarchyCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) {
            return;
        }
        const auto mode = static_cast<project::CppHierarchyMode>(m_hierarchyCombo->itemData(idx).toInt());
        if (mode != m_cppHierarchyMode) {
            m_cppHierarchyMode = mode;
            emit cppHierarchyModeChanged(m_cppHierarchyMode);
        }
    });
    connect(m_model, &SolutionTreeModel::filesDroppedOnFilter, this, &SolutionExplorer::moveFilesToFilterRequested);
    connect(m_model, &SolutionTreeModel::physicalFilesDroppedOnDirectory, this,
            [this](const QString& projectPath, const QStringList& filePaths, const QString& targetDirectoryPath) {
                emit movePhysicalPathsRequested(filePaths, targetDirectoryPath, projectPath);
            });
    connect(m_model, &SolutionTreeModel::projectDroppedOnSolutionFolder, this,
            &SolutionExplorer::moveProjectToSolutionFolderRequested);
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) { activate(index, false); });
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        endSpeedSearch();
        const QModelIndex idx = m_view->indexAt(position);
        if (idx.isValid()) {
            if (m_view->selectionModel() && m_view->selectionModel()->isSelected(idx)) {
                m_view->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::NoUpdate);
            } else {
                m_view->setCurrentIndex(idx);
            }
        }
        QMenu* menu = createContextMenu(idx, this);
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

void SolutionExplorer::setCppHierarchyMode(project::CppHierarchyMode mode) {
    m_cppHierarchyMode = mode;
    if (m_hierarchyCombo) {
        const int idx = m_hierarchyCombo->findData(static_cast<int>(mode));
        if (idx >= 0 && m_hierarchyCombo->currentIndex() != idx) {
            m_hierarchyCombo->blockSignals(true);
            m_hierarchyCombo->setCurrentIndex(idx);
            m_hierarchyCombo->blockSignals(false);
        }
    }
}

QStringList SolutionExplorer::selectedProjectFilePaths(project::ProjectId projectId) const {
    QStringList paths;
    const auto solution = m_model->solution();
    if (!solution || !m_view->selectionModel()) {
        return paths;
    }
    for (const QModelIndex& proxyIdx : m_view->selectionModel()->selectedIndexes()) {
        const QModelIndex srcIdx = m_filter->mapToSource(proxyIdx);
        const std::optional<NodeId> nodeId = m_model->nodeOf(srcIdx);
        if (!nodeId) {
            continue;
        }
        const project::Node& node = solution->node(*nodeId);
        if (node.kind == NodeKind::File && node.project == projectId && !node.path.empty()) {
            const QString filePath = QString::fromStdString(node.path);
            if (!paths.contains(filePath)) {
                paths.push_back(filePath);
            }
        }
    }
    return paths;
}

QStringList SolutionExplorer::selectedPhysicalPaths() const {
    QStringList paths;
    const auto solution = m_model->solution();
    if (!solution || !m_view->selectionModel()) {
        return paths;
    }
    for (const QModelIndex& proxyIdx : m_view->selectionModel()->selectedIndexes()) {
        const QModelIndex srcIdx = m_filter->mapToSource(proxyIdx);
        const std::optional<NodeId> nodeId = m_model->nodeOf(srcIdx);
        if (!nodeId) {
            continue;
        }
        const project::Node& node = solution->node(*nodeId);
        if ((node.kind == NodeKind::File || node.kind == NodeKind::Filter) && !node.path.empty()) {
            const QString filePath = QString::fromStdString(node.path);
            if (!paths.contains(filePath)) {
                paths.push_back(filePath);
            }
        }
    }
    return paths;
}

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
            (key->key() == Qt::Key_Delete && mods == Qt::NoModifier) ||
            (mods == Qt::ControlModifier &&
             (key->key() == Qt::Key_C || key->key() == Qt::Key_X || key->key() == Qt::Key_V ||
              key->key() == Qt::Key_D)) ||
            ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && mods == Qt::AltModifier)) {
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
    } else if (watched == m_view->viewport() &&
               (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)) {
        auto* dragEvent = static_cast<QDragMoveEvent*>(event);
        const QMimeData* mime = dragEvent->mimeData();
        if (mime != nullptr &&
            (mime->hasFormat(QStringLiteral("application/x-frostide-solution-files")) ||
             mime->hasFormat(QStringLiteral("application/x-frostide-solution-project")) || mime->hasUrls())) {
            const QModelIndex proxyIdx = m_view->indexAt(dragEvent->position().toPoint());
            const QModelIndex srcIdx = m_filter->mapToSource(proxyIdx);
            if (m_model->canDropMimeData(mime, dragEvent->proposedAction(), -1, 0, srcIdx)) {
                dragEvent->acceptProposedAction();
                return true;
            }
        }
    } else if (watched == m_view->viewport() && event->type() == QEvent::Drop) {
        auto* dropEvent = static_cast<QDropEvent*>(event);
        const QMimeData* mime = dropEvent->mimeData();
        const QModelIndex proxyIdx = m_view->indexAt(dropEvent->position().toPoint());
        const QModelIndex srcIdx = m_filter->mapToSource(proxyIdx);
        if (m_model->dropMimeData(mime, dropEvent->proposedAction(), -1, 0, srcIdx)) {
            dropEvent->acceptProposedAction();
            return true;
        }
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
    if (rawMods == Qt::ControlModifier) {
        if (event.key() == Qt::Key_C) {
            const QStringList paths = selectedPhysicalPaths();
            if (!paths.isEmpty()) {
                emit copyPathsRequested(paths);
                return true;
            }
        } else if (event.key() == Qt::Key_X) {
            const QStringList paths = selectedPhysicalPaths();
            if (!paths.isEmpty()) {
                emit cutPathsRequested(paths);
                return true;
            }
        } else if (event.key() == Qt::Key_V) {
            const QString targetDir = currentDirectory();
            if (!targetDir.isEmpty() && m_model->solution()) {
                QString projPath;
                QString filterPath;
                if (const auto nodeId = currentNode()) {
                    const auto& n = m_model->solution()->node(*nodeId);
                    if (n.project) {
                        projPath = QString::fromStdString(m_model->solution()->project(*n.project).path);
                    }
                    if (!n.filterPath.empty() && n.kind != NodeKind::SolutionFolder) {
                        filterPath = QString::fromStdString(n.filterPath);
                    }
                }
                emit pasteIntoDirectoryRequested(targetDir, projPath, filterPath);
                return true;
            }
        } else if (event.key() == Qt::Key_D) {
            const QModelIndex idx = m_view->currentIndex();
            if (idx.isValid() && m_model->solution()) {
                const auto kind = static_cast<NodeKind>(idx.data(SolutionTreeModel::KindRole).toInt());
                const QString path = idx.data(SolutionTreeModel::PathRole).toString();
                if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty()) {
                    QString projPath;
                    const int projVal = idx.data(SolutionTreeModel::ProjectIdRole).toInt();
                    if (projVal >= 0) {
                        projPath = QString::fromStdString(
                            m_model->solution()->project(project::ProjectId{static_cast<std::uint32_t>(projVal)}).path);
                    }
                    const QString filterPath = idx.data(SolutionTreeModel::FilterPathRole).toString();
                    emit duplicatePathRequested(path, kind == NodeKind::Filter, projPath, filterPath);
                    return true;
                }
            }
        }
    }
    if (enter && rawMods == Qt::AltModifier) {
        const QModelIndex idx = m_view->currentIndex();
        if (idx.isValid()) {
            const auto kind = static_cast<NodeKind>(idx.data(SolutionTreeModel::KindRole).toInt());
            const QString path = idx.data(SolutionTreeModel::PathRole).toString();
            if (kind == NodeKind::Solution) {
                emit solutionPropertiesRequested();
                return true;
            }
            if (kind == NodeKind::Project && !path.isEmpty()) {
                emit projectPropertiesRequested(path);
                return true;
            }
        }
    }
    if ((event.key() == Qt::Key_F2 && rawMods == Qt::NoModifier) ||
        (event.key() == Qt::Key_F6 && rawMods == Qt::ShiftModifier)) {
        const QModelIndex idx = m_view->currentIndex();
        if (idx.isValid()) {
            const auto kind = static_cast<NodeKind>(idx.data(SolutionTreeModel::KindRole).toInt());
            const QString path = idx.data(SolutionTreeModel::PathRole).toString();
            if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty()) {
                emit renameRequested(path, kind == NodeKind::Filter);
            } else if (kind == NodeKind::Filter && m_model->solution()) {
                const QString filterPath = idx.data(SolutionTreeModel::FilterPathRole).toString();
                const int projVal = idx.data(SolutionTreeModel::ProjectIdRole).toInt();
                if (!filterPath.isEmpty() && projVal >= 0) {
                    const QString projPath = QString::fromStdString(
                        m_model->solution()->project(project::ProjectId{static_cast<std::uint32_t>(projVal)}).path);
                    emit renameFilterRequested(projPath, filterPath);
                }
            } else if (kind == NodeKind::SolutionFolder) {
                const QString folderId = idx.data(SolutionTreeModel::FilterPathRole).toString();
                const QString displayName = idx.data(Qt::DisplayRole).toString();
                if (!folderId.isEmpty()) {
                    emit renameSolutionFolderRequested(folderId, displayName);
                }
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
            } else if (kind == NodeKind::Filter && m_model->solution()) {
                const QString filterPath = idx.data(SolutionTreeModel::FilterPathRole).toString();
                const int projVal = idx.data(SolutionTreeModel::ProjectIdRole).toInt();
                if (!filterPath.isEmpty() && projVal >= 0) {
                    const QString projPath = QString::fromStdString(
                        m_model->solution()->project(project::ProjectId{static_cast<std::uint32_t>(projVal)}).path);
                    emit removeFilterRequested(projPath, filterPath);
                }
            } else if (kind == NodeKind::SolutionFolder) {
                const QString folderId = idx.data(SolutionTreeModel::FilterPathRole).toString();
                if (!folderId.isEmpty()) {
                    emit removeSolutionFolderRequested(folderId);
                }
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

namespace {

void addSolutionAndProjectManagementActions(SolutionExplorer& explorer, QMenu& menu, NodeKind kind, const QString& path,
                                            const QString& filterProjectPath,
                                            const std::shared_ptr<const project::SolutionModel>& solution,
                                            const QModelIndex& source) {
    if (kind == NodeKind::Solution) {
        QAction* addSolFolder = menu.addAction(SolutionExplorer::tr("Add New Solution Folder..."));
        addSolFolder->setObjectName(QStringLiteral("addSolutionFolder"));
        QObject::connect(addSolFolder, &QAction::triggered, &explorer,
                         [&explorer] { emit explorer.addSolutionFolderRequested(QString()); });

        QAction* addProj = menu.addAction(SolutionExplorer::tr("Add Existing Project..."));
        addProj->setObjectName(QStringLiteral("addExistingProject"));
        QObject::connect(addProj, &QAction::triggered, &explorer,
                         [&explorer, path] { emit explorer.addExistingProjectRequested(path); });

        QAction* cfgMgr = menu.addAction(SolutionExplorer::tr("Configuration Manager..."));
        cfgMgr->setObjectName(QStringLiteral("configurationManager"));
        QObject::connect(cfgMgr, &QAction::triggered, &explorer,
                         [&explorer] { emit explorer.configurationManagerRequested(); });

        QAction* runCfg = menu.addAction(SolutionExplorer::tr("Edit Run/Debug Configurations..."));
        runCfg->setObjectName(QStringLiteral("editRunConfigurations"));
        QObject::connect(runCfg, &QAction::triggered, &explorer,
                         [&explorer] { emit explorer.editRunConfigurationsRequested(QString()); });

        QAction* props = menu.addAction(SolutionExplorer::tr("Properties..."));
        props->setObjectName(QStringLiteral("solutionProperties"));
        QObject::connect(props, &QAction::triggered, &explorer,
                         [&explorer] { emit explorer.solutionPropertiesRequested(); });
        menu.addSeparator();
        return;
    }

    if (kind == NodeKind::SolutionFolder) {
        const QString folderId = source.data(SolutionTreeModel::FilterPathRole).toString();
        const QString folderName = source.data(Qt::DisplayRole).toString();
        QAction* addSubFolder = menu.addAction(SolutionExplorer::tr("Add New Solution Folder..."));
        addSubFolder->setObjectName(QStringLiteral("addSolutionFolder"));
        QObject::connect(addSubFolder, &QAction::triggered, &explorer,
                         [&explorer, folderId] { emit explorer.addSolutionFolderRequested(folderId); });

        QAction* renameFolder = menu.addAction(SolutionExplorer::tr("Rename Solution Folder..."));
        renameFolder->setObjectName(QStringLiteral("renameSolutionFolder"));
        QObject::connect(renameFolder, &QAction::triggered, &explorer, [&explorer, folderId, folderName] {
            emit explorer.renameSolutionFolderRequested(folderId, folderName);
        });

        QAction* removeFolder = menu.addAction(SolutionExplorer::tr("Remove Solution Folder"));
        removeFolder->setObjectName(QStringLiteral("removeSolutionFolder"));
        QObject::connect(removeFolder, &QAction::triggered, &explorer,
                         [&explorer, folderId] { emit explorer.removeSolutionFolderRequested(folderId); });
        menu.addSeparator();
        return;
    }

    if ((kind == NodeKind::Project || kind == NodeKind::UnavailableProject) && !path.isEmpty()) {
        if (kind == NodeKind::Project) {
            QAction* editFile = menu.addAction(SolutionExplorer::tr("Edit Project File"));
            editFile->setObjectName(QStringLiteral("editProjectFile"));
            QObject::connect(editFile, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.openFile(path); });

            QAction* addRef = menu.addAction(SolutionExplorer::tr("Add Project Reference..."));
            addRef->setObjectName(QStringLiteral("addProjectReference"));
            QObject::connect(addRef, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.addProjectReferenceRequested(path); });

            if (path.endsWith(QStringLiteral(".csproj"), Qt::CaseInsensitive)) {
                QAction* nuget = menu.addAction(SolutionExplorer::tr("Manage NuGet Packages..."));
                nuget->setObjectName(QStringLiteral("manageNuGetPackages"));
                QObject::connect(nuget, &QAction::triggered, &explorer,
                                 [&explorer, path] { emit explorer.manageNuGetPackagesRequested(path); });
            }

            QAction* runCfg = menu.addAction(SolutionExplorer::tr("Edit Run/Debug Configuration..."));
            runCfg->setObjectName(QStringLiteral("editRunConfigurations"));
            QObject::connect(runCfg, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.editRunConfigurationsRequested(path); });
        }

        if (solution) {
            QMenu* moveFolderMenu = menu.addMenu(SolutionExplorer::tr("Move to Solution Folder"));
            moveFolderMenu->setObjectName(QStringLiteral("moveToSolutionFolderMenu"));
            QAction* moveSolRoot = moveFolderMenu->addAction(SolutionExplorer::tr("<Solution Root>"));
            moveSolRoot->setObjectName(QStringLiteral("moveToSolutionRoot"));
            QObject::connect(moveSolRoot, &QAction::triggered, &explorer, [&explorer, path] {
                emit explorer.moveProjectToSolutionFolderRequested(path, QString());
            });
            bool addedSep = false;
            for (std::size_t i = 0; i < solution->nodeCount(); ++i) {
                const project::Node& n = solution->node(NodeId{static_cast<std::uint32_t>(i)});
                if (n.kind == NodeKind::SolutionFolder && !n.filterPath.empty()) {
                    if (!addedSep) {
                        moveFolderMenu->addSeparator();
                        addedSep = true;
                    }
                    const QString folderId = QString::fromStdString(n.filterPath);
                    const QString folderName = QString::fromStdString(n.displayName);
                    QAction* moveAct = moveFolderMenu->addAction(folderName);
                    QObject::connect(moveAct, &QAction::triggered, &explorer, [&explorer, path, folderId] {
                        emit explorer.moveProjectToSolutionFolderRequested(path, folderId);
                    });
                }
            }
            moveFolderMenu->addSeparator();
            QAction* moveNewFolder = moveFolderMenu->addAction(SolutionExplorer::tr("New Solution Folder..."));
            moveNewFolder->setObjectName(QStringLiteral("moveToNewSolutionFolder"));
            QObject::connect(moveNewFolder, &QAction::triggered, &explorer, [&explorer, path] {
                emit explorer.moveProjectToNewSolutionFolderRequested(path);
            });
        }

        if (kind == NodeKind::Project) {
            QAction* unloadProj = menu.addAction(SolutionExplorer::tr("Unload Project"));
            unloadProj->setObjectName(QStringLiteral("unloadProject"));
            QObject::connect(unloadProj, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.unloadProjectRequested(path); });
        } else if (kind == NodeKind::UnavailableProject) {
            QAction* reloadProj = menu.addAction(SolutionExplorer::tr("Reload Project"));
            reloadProj->setObjectName(QStringLiteral("reloadProject"));
            QObject::connect(reloadProj, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.reloadProjectRequested(path); });
        }

        QAction* removeProj = menu.addAction(SolutionExplorer::tr("Remove from Solution"));
        removeProj->setObjectName(QStringLiteral("removeProjectFromSolution"));
        QObject::connect(removeProj, &QAction::triggered, &explorer,
                         [&explorer, path] { emit explorer.removeProjectFromSolutionRequested(path); });

        if (kind == NodeKind::Project) {
            QAction* props = menu.addAction(SolutionExplorer::tr("Properties..."));
            props->setObjectName(QStringLiteral("projectProperties"));
            QObject::connect(props, &QAction::triggered, &explorer,
                             [&explorer, path] { emit explorer.projectPropertiesRequested(path); });
        }
        menu.addSeparator();
        return;
    }

    if (kind == NodeKind::Filter && !filterProjectPath.isEmpty()) {
        QAction* props = menu.addAction(SolutionExplorer::tr("Project Properties..."));
        props->setObjectName(QStringLiteral("projectProperties"));
        QObject::connect(props, &QAction::triggered, &explorer, [&explorer, filterProjectPath] {
            emit explorer.projectPropertiesRequested(filterProjectPath);
        });
        menu.addSeparator();
    }
}

void addBuildAndSourceActions(SolutionExplorer& explorer, QMenu& menu, BuildController* controller, NodeKind kind,
                              const QModelIndex& source, const QString& path, const QString& filterProjectPath,
                              const std::shared_ptr<const project::SolutionModel>& solution,
                              const std::function<void(project::ProjectId)>& startupHandler,
                              const std::function<void(const QString&)>& addNewHandler,
                              const std::function<void(const QString&)>& addExistingHandler,
                              const std::function<bool(const QString&)>& recoveryAvailable,
                              const std::function<void(const QString&)>& recoverHandler, detail::IconCache& icons) {
    if (kind == NodeKind::Solution || isProjectKind(kind)) {
        const bool solutionNode = kind == NodeKind::Solution;
        const int projectValue = source.data(SolutionTreeModel::ProjectIdRole).toInt();
        std::optional<project::ProjectId> projectId;
        if (!solutionNode && projectValue >= 0)
            projectId = project::ProjectId{static_cast<std::uint32_t>(projectValue)};
        const bool enabled =
            controller != nullptr && controller->canBuild() && (solutionNode || kind == NodeKind::Project);
        struct Command {
            build::BuildAction action;
            QString text;
            const char* name;
        };
        const Command commands[] = {
            {build::BuildAction::Build,
             solutionNode ? SolutionExplorer::tr("Build Solution") : SolutionExplorer::tr("Build"),
             solutionNode ? "buildSolution" : "buildProject"},
            {build::BuildAction::Rebuild,
             solutionNode ? SolutionExplorer::tr("Rebuild Solution") : SolutionExplorer::tr("Rebuild"),
             solutionNode ? "rebuildSolution" : "rebuildProject"},
            {build::BuildAction::Clean,
             solutionNode ? SolutionExplorer::tr("Clean Solution") : SolutionExplorer::tr("Clean"),
             solutionNode ? "cleanSolution" : "cleanProject"},
        };
        for (const Command& command : commands) {
            QAction* action = menu.addAction(command.text);
            action->setObjectName(QString::fromLatin1(command.name));
            action->setEnabled(enabled);
            if (command.action == build::BuildAction::Build)
                action->setIcon(icons.get(icons::kBuildBuild));
            QObject::connect(action, &QAction::triggered, &explorer,
                             [controller, kindAction = command.action, projectId] {
                                 if (controller != nullptr)
                                     static_cast<void>(controller->build(kindAction, projectId));
                             });
        }
        if (!solutionNode && projectId && startupHandler) {
            QAction* startup = menu.addAction(SolutionExplorer::tr("Set as Startup Project"));
            startup->setObjectName(QStringLiteral("setStartupProject"));
            startup->setEnabled(kind == NodeKind::Project);
            QObject::connect(startup, &QAction::triggered, &explorer, [startupHandler, id = *projectId] {
                if (startupHandler)
                    startupHandler(id);
            });
        }
        if (kind == NodeKind::Project && path.endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive)) {
            QAction* addFilter = menu.addAction(SolutionExplorer::tr("Add New Filter..."));
            addFilter->setObjectName(QStringLiteral("addNewFilter"));
            QObject::connect(addFilter, &QAction::triggered, &explorer, [&explorer, path] {
                emit explorer.addFilterRequested(path, QString());
            });
            if (addNewHandler) {
                QAction* addNew = menu.addAction(SolutionExplorer::tr("Add New C/C++ Source..."));
                addNew->setObjectName(QStringLiteral("addNewProjectFile"));
                QObject::connect(addNew, &QAction::triggered, &explorer, [&explorer, solution, path, addNewHandler] {
                    if (explorer.solution() == solution && addNewHandler)
                        addNewHandler(path);
                });
            }
            if (addExistingHandler) {
                QAction* add = menu.addAction(SolutionExplorer::tr("Add Existing C/C++ Files..."));
                add->setObjectName(QStringLiteral("addExistingProjectFile"));
                QObject::connect(add, &QAction::triggered, &explorer, [&explorer, solution, path, addExistingHandler] {
                    if (explorer.solution() == solution && addExistingHandler)
                        addExistingHandler(path);
                });
            }
            QAction* addFolder = menu.addAction(SolutionExplorer::tr("Add Existing Folder..."));
            addFolder->setObjectName(QStringLiteral("addExistingProjectFolder"));
            QObject::connect(addFolder, &QAction::triggered, &explorer, [&explorer, path] {
                emit explorer.addExistingFolderToFilterRequested(path, QString());
            });
        }
        if (kind == NodeKind::Project && recoveryAvailable && recoverHandler && recoveryAvailable(path)) {
            QAction* recover = menu.addAction(SolutionExplorer::tr("Recover Project Edit"));
            recover->setObjectName(QStringLiteral("recoverProjectEdit"));
            QObject::connect(recover, &QAction::triggered, &explorer,
                             [&explorer, solution, path, recoveryAvailable, recoverHandler] {
                                 if (explorer.solution() == solution && recoveryAvailable && recoverHandler &&
                                     recoveryAvailable(path))
                                     recoverHandler(path);
                             });
        }
        menu.addSeparator();
    } else if (kind == NodeKind::Filter &&
               filterProjectPath.endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive)) {
        const QString filterLogicalPath = source.data(SolutionTreeModel::FilterPathRole).toString();
        QAction* addFilter = menu.addAction(SolutionExplorer::tr("Add New Filter..."));
        addFilter->setObjectName(QStringLiteral("addNewFilter"));
        QObject::connect(addFilter, &QAction::triggered, &explorer,
                         [&explorer, filterProjectPath, filterLogicalPath] {
                             emit explorer.addFilterRequested(filterProjectPath, filterLogicalPath);
                         });
        if (addNewHandler) {
            QAction* addNew = menu.addAction(SolutionExplorer::tr("Add New C/C++ Source..."));
            addNew->setObjectName(QStringLiteral("addNewProjectFile"));
            QObject::connect(addNew, &QAction::triggered, &explorer,
                             [&explorer, solution, filterProjectPath, addNewHandler] {
                                 if (explorer.solution() == solution && addNewHandler)
                                     addNewHandler(filterProjectPath);
                             });
        }
        if (addExistingHandler) {
            QAction* add = menu.addAction(SolutionExplorer::tr("Add Existing C/C++ Files..."));
            add->setObjectName(QStringLiteral("addExistingProjectFile"));
            QObject::connect(add, &QAction::triggered, &explorer,
                             [&explorer, solution, filterProjectPath, addExistingHandler] {
                                 if (explorer.solution() == solution && addExistingHandler)
                                     addExistingHandler(filterProjectPath);
                             });
        }
        QAction* addFolder = menu.addAction(SolutionExplorer::tr("Add Existing Folder..."));
        addFolder->setObjectName(QStringLiteral("addExistingProjectFolder"));
        QObject::connect(addFolder, &QAction::triggered, &explorer,
                         [&explorer, filterProjectPath, filterLogicalPath] {
                             emit explorer.addExistingFolderToFilterRequested(filterProjectPath, filterLogicalPath);
                         });
        menu.addSeparator();
    }
}

} // namespace

QMenu* SolutionExplorer::createContextMenu(const QModelIndex& index, QWidget* parent) {
    auto* menu = new QMenu(parent);
    const QModelIndex source = m_filter->mapToSource(index);
    const std::optional<NodeId> node = m_model->nodeOf(source);
    if (!node) {
        return menu;
    }
    const auto kind = static_cast<NodeKind>(source.data(SolutionTreeModel::KindRole).toInt());
    const QString path = source.data(SolutionTreeModel::PathRole).toString();
    m_contextFilterPath =
        kind == NodeKind::Filter ? source.data(SolutionTreeModel::FilterPathRole).toString() : QString();
    detail::IconCache icons;
    const auto solution = m_model->solution();
    QString filterProjectPath;
    if (solution && kind == NodeKind::Filter) {
        const auto& item = solution->node(*node);
        if (item.project)
            filterProjectPath = QString::fromStdString(solution->project(*item.project).path);
    }

    if (kind == NodeKind::File) {
        QAction* open = menu->addAction(tr("Open"));
        open->setObjectName(QStringLiteral("openFile"));
        connect(open, &QAction::triggered, this, [this, path] { emit openFile(path); });
        const auto& item = solution->node(*node);
        if (item.project && !item.path.empty() &&
            solution->project(*item.project).kind == project::ProjectKind::Cpp) {
            const QString projectPath = QString::fromStdString(solution->project(*item.project).path);
            QStringList filesToActOn = selectedProjectFilePaths(*item.project);
            if (filesToActOn.isEmpty() || !filesToActOn.contains(path)) {
                filesToActOn = QStringList{path};
            }

            QMenu* moveMenu = menu->addMenu(tr("Move to Filter"));
            moveMenu->setObjectName(QStringLiteral("moveToFilterMenu"));
            QAction* moveRoot = moveMenu->addAction(tr("<Project Root>"));
            moveRoot->setObjectName(QStringLiteral("moveToFilterRoot"));
            connect(moveRoot, &QAction::triggered, this, [this, projectPath, filesToActOn] {
                emit moveFilesToFilterRequested(projectPath, filesToActOn, QString());
            });

            QStringList projectFilters;
            for (std::size_t i = 0; i < solution->nodeCount(); ++i) {
                const project::Node& n = solution->node(NodeId{static_cast<std::uint32_t>(i)});
                if (n.kind == NodeKind::Filter && n.project == *item.project && !n.filterPath.empty()) {
                    const QString fp = QString::fromStdString(n.filterPath);
                    if (!projectFilters.contains(fp)) {
                        projectFilters.push_back(fp);
                    }
                }
            }
            projectFilters.sort(Qt::CaseInsensitive);
            if (!projectFilters.isEmpty()) {
                moveMenu->addSeparator();
                for (const QString& fp : projectFilters) {
                    QAction* moveAct = moveMenu->addAction(fp);
                    connect(moveAct, &QAction::triggered, this, [this, projectPath, filesToActOn, fp] {
                        emit moveFilesToFilterRequested(projectPath, filesToActOn, fp);
                    });
                }
            }
            moveMenu->addSeparator();
            QAction* moveNew = moveMenu->addAction(tr("New Filter..."));
            moveNew->setObjectName(QStringLiteral("moveToNewFilter"));
            connect(moveNew, &QAction::triggered, this, [this, projectPath, filesToActOn] {
                emit moveFilesToNewFilterRequested(projectPath, filesToActOn);
            });

            if (m_removeProjectFileHandler) {
                const QString key = QString::fromStdString(item.key);
                QAction* remove = menu->addAction(filesToActOn.size() > 1
                                                      ? tr("Remove %1 Files from Project").arg(filesToActOn.size())
                                                      : tr("Remove from Project"));
                remove->setObjectName(QStringLiteral("removeFromProject"));
                connect(remove, &QAction::triggered, this,
                        [this, key, path, projectPath, filesToActOn, solution] {
                            const auto current = m_model->solution();
                            if (current != solution || !m_removeProjectFileHandler)
                                return;
                            if (filesToActOn.size() > 1) {
                                emit removeMultipleProjectFilesRequested(projectPath, filesToActOn);
                                return;
                            }
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
        }
        menu->addSeparator();
    }

    addBuildAndSourceActions(*this, *menu, m_controller, kind, source, path, filterProjectPath, solution,
                             m_startupHandler, m_addNewProjectFileHandler, m_addProjectFileHandler, m_recoveryAvailable,
                             m_recoverProjectHandler, icons);

    if ((m_newFileHandler || m_newFolderHandler) && solution) {
        const auto& item = solution->node(*node);
        QString targetDir;
        if (item.kind == NodeKind::Solution || item.kind == NodeKind::SolutionFolder)
            targetDir = QString::fromStdString(solution->directory());
        else if (item.kind == NodeKind::Filter && !item.path.empty())
            targetDir = QString::fromStdString(item.path);
        else if (item.kind == NodeKind::File && !item.path.empty())
            targetDir = QString::fromStdString(std::string(io::parentPath(item.path)));
        else if (item.project)
            targetDir = QString::fromStdString(std::string(io::parentPath(solution->project(*item.project).path)));
        else if (!item.path.empty())
            targetDir = QString::fromStdString(std::string(io::parentPath(item.path)));
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

    if ((kind == NodeKind::File || kind == NodeKind::Filter) && !path.isEmpty() && solution) {
        const bool isDir = kind == NodeKind::Filter;
        const auto& item = solution->node(*node);
        const QString projPath =
            item.project ? QString::fromStdString(solution->project(*item.project).path) : QString();
        const QString filterLogicalPath = QString::fromStdString(item.filterPath);
        QStringList clipTargets = selectedPhysicalPaths();
        if (clipTargets.isEmpty() || !clipTargets.contains(path)) {
            clipTargets = QStringList{path};
        }

        QAction* cutAct = menu->addAction(tr("Cut"));
        cutAct->setObjectName(QStringLiteral("cutItem"));
        connect(cutAct, &QAction::triggered, this, [this, clipTargets] { emit cutPathsRequested(clipTargets); });

        QAction* copyAct = menu->addAction(tr("Copy"));
        copyAct->setObjectName(QStringLiteral("copyItem"));
        connect(copyAct, &QAction::triggered, this, [this, clipTargets] { emit copyPathsRequested(clipTargets); });

        QString pasteTargetDir = isDir ? path : QString::fromStdString(std::string(io::parentPath(toUtf8(path))));
        QAction* pasteAct = menu->addAction(tr("Paste"));
        pasteAct->setObjectName(QStringLiteral("pasteItem"));
        pasteAct->setEnabled(!pasteTargetDir.isEmpty() && (!m_canPasteProvider || m_canPasteProvider()));
        connect(pasteAct, &QAction::triggered, this, [this, pasteTargetDir, projPath, filterLogicalPath] {
            emit pasteIntoDirectoryRequested(pasteTargetDir, projPath, filterLogicalPath);
        });

        QAction* dupAct = menu->addAction(tr("Duplicate..."));
        dupAct->setObjectName(QStringLiteral("duplicateItem"));
        connect(dupAct, &QAction::triggered, this, [this, path, isDir, projPath, filterLogicalPath] {
            emit duplicatePathRequested(path, isDir, projPath, filterLogicalPath);
        });

        QAction* renameAct = menu->addAction(tr("Rename..."));
        renameAct->setObjectName(QStringLiteral("renameItem"));
        connect(renameAct, &QAction::triggered, this, [this, path, isDir] { emit renameRequested(path, isDir); });
        QAction* deleteAct = menu->addAction(tr("Delete..."));
        deleteAct->setObjectName(QStringLiteral("deleteItem"));
        connect(deleteAct, &QAction::triggered, this, [this, path, isDir] { emit deleteRequested(path, isDir); });
        menu->addSeparator();
    } else if (kind == NodeKind::Filter && !m_contextFilterPath.isEmpty() &&
               filterProjectPath.endsWith(QStringLiteral(".vcxproj"), Qt::CaseInsensitive)) {
        const QString filterLogicalPath = m_contextFilterPath;
        const QString projDir = QString::fromStdString(std::string(io::parentPath(toUtf8(filterProjectPath))));
        QAction* pasteAct = menu->addAction(tr("Paste"));
        pasteAct->setObjectName(QStringLiteral("pasteItem"));
        pasteAct->setEnabled(!projDir.isEmpty() && (!m_canPasteProvider || m_canPasteProvider()));
        connect(pasteAct, &QAction::triggered, this, [this, projDir, filterProjectPath, filterLogicalPath] {
            emit pasteIntoDirectoryRequested(projDir, filterProjectPath, filterLogicalPath);
        });
        QAction* renameAct = menu->addAction(tr("Rename Filter..."));
        renameAct->setObjectName(QStringLiteral("renameFilter"));
        connect(renameAct, &QAction::triggered, this, [this, filterProjectPath, filterLogicalPath] {
            emit renameFilterRequested(filterProjectPath, filterLogicalPath);
        });
        QAction* removeAct = menu->addAction(tr("Remove Filter"));
        removeAct->setObjectName(QStringLiteral("removeFilter"));
        connect(removeAct, &QAction::triggered, this, [this, filterProjectPath, filterLogicalPath] {
            emit removeFilterRequested(filterProjectPath, filterLogicalPath);
        });
        menu->addSeparator();
    } else if ((kind == NodeKind::Project || kind == NodeKind::Solution) && !path.isEmpty()) {
        const QString targetDir = kind == NodeKind::Solution
                                      ? QString::fromStdString(solution ? solution->directory() : std::string{})
                                      : QString::fromStdString(std::string(io::parentPath(toUtf8(path))));
        const QString projPath = kind == NodeKind::Project ? path : QString();
        if (!targetDir.isEmpty()) {
            QAction* pasteAct = menu->addAction(tr("Paste"));
            pasteAct->setObjectName(QStringLiteral("pasteItem"));
            pasteAct->setEnabled(!m_canPasteProvider || m_canPasteProvider());
            connect(pasteAct, &QAction::triggered, this, [this, targetDir, projPath] {
                emit pasteIntoDirectoryRequested(targetDir, projPath, QString());
            });
            menu->addSeparator();
        }
    }

    addSolutionAndProjectManagementActions(*this, *menu, kind, path, filterProjectPath, solution, source);

    const QString effectivePath = !path.isEmpty() ? path : filterProjectPath;
    if (m_terminalHandler && !effectivePath.isEmpty()) {
        QAction* terminal = menu->addAction(tr("Open in Terminal"));
        terminal->setObjectName(QStringLiteral("openInTerminal"));
        connect(terminal, &QAction::triggered, this, [this, effectivePath] {
            if (m_terminalHandler)
                m_terminalHandler(effectivePath);
        });
    }
    QAction* reveal = menu->addAction(tr("Open Containing Folder"));
    reveal->setObjectName(QStringLiteral("openContainingFolder"));
    reveal->setEnabled(m_launcher != nullptr && !effectivePath.isEmpty());
    connect(reveal, &QAction::triggered, this, [this, effectivePath] { openContainingFolder(effectivePath); });

    QAction* copy = menu->addAction(tr("Copy Path"));
    copy->setObjectName(QStringLiteral("copyPath"));
    copy->setIcon(icons.get(icons::kEditCopy));
    copy->setEnabled(!effectivePath.isEmpty());
    connect(copy, &QAction::triggered, this, [effectivePath] { QGuiApplication::clipboard()->setText(effectivePath); });
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
