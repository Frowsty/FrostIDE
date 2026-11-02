// SPDX-License-Identifier: Apache-2.0
//
// FileSystemView (frost/ui/workbench/file_system_view.hpp).

#include "frost/ui/workbench/file_system_view.hpp"

#include "frost/search/fuzzy_path.hpp"
#include "frost/ui/icon_ids.hpp"

#include "workbench_util.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <set>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::text;
using workbench_detail::themedIcon;
using workbench_detail::toUtf8;

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kDirectoryRole = Qt::UserRole + 2;

// Every expanded folder's path under `parent`.
void collectExpanded(const QTreeView& tree, const QStandardItemModel& model, const QModelIndex& parent,
                     std::set<std::string>& out) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (tree.isExpanded(index)) {
            out.insert(toUtf8(index.data(kPathRole).toString()));
            collectExpanded(tree, model, index, out);
        }
    }
}

void expandMatching(QTreeView& tree, const QStandardItemModel& model, const QModelIndex& parent,
                    const std::set<std::string>& paths) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (paths.contains(toUtf8(index.data(kPathRole).toString()))) {
            tree.expand(index);
            expandMatching(tree, model, index, paths);
        }
    }
}

void collectVisible(const QTreeView& tree, const QStandardItemModel& model, const QModelIndex& parent,
                    QStringList& out) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        if (tree.isRowHidden(row, parent)) {
            continue;
        }
        const QModelIndex index = model.index(row, 0, parent);
        out.push_back(index.data(Qt::DisplayRole).toString());
        if (tree.isExpanded(index)) {
            collectVisible(tree, model, index, out);
        }
    }
}

} // namespace

FileSystemView::FileSystemView(QWidget* parent)
    : QWidget(parent), m_tree(new QTreeView(this)), m_model(new QStandardItemModel(this)),
      m_search(new QLineEdit(this)),
      m_empty(new QLabel(text("No folder is open. Open a folder or a solution to see its files here."), this)) {
    setObjectName(QStringLiteral("fileSystemView"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_search->setObjectName(QStringLiteral("speedSearch"));
    m_search->setPlaceholderText(text("Search for:"));
    m_search->hide();
    m_tree->setObjectName(QStringLiteral("fileSystemTree"));
    m_tree->setModel(m_model);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->installEventFilter(this);
    m_search->installEventFilter(this);
    m_empty->setObjectName(QStringLiteral("fileSystemEmpty"));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_empty, 1);
    m_tree->hide();
    connect(m_tree, &QTreeView::activated, this, [this](const QModelIndex& index) { activate(index); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& value) {
        if (value.isEmpty()) {
            m_search->hide();
            m_tree->setFocus(Qt::OtherFocusReason);
        }
        int matches = 0;
        QModelIndex first;
        filter(m_model->invisibleRootItem(), toUtf8(value.toLower()), matches, first);
        if (first.isValid()) {
            m_tree->setCurrentIndex(first);
            m_tree->scrollTo(first);
        }
    });
}

void FileSystemView::setFiles(const nav::ProjectFiles& files) {
    std::set<std::string> expanded;
    collectExpanded(*m_tree, *m_model, QModelIndex(), expanded);
    const std::string selected = selectedPath();
    m_model->clear();
    const bool hasRoot = !files.root().empty() && !files.nodes().empty();
    m_tree->setVisible(hasRoot);
    m_empty->setVisible(!hasRoot);
    if (!hasRoot) {
        return;
    }
    const nav::ProjectFiles::Node& root = files.node(0);
    auto* rootItem = new QStandardItem(themedIcon(icons::kNodesFolder), fromUtf8(root.name));
    rootItem->setData(fromUtf8(root.path), kPathRole);
    rootItem->setData(true, kDirectoryRole);
    m_model->appendRow(rootItem);
    addChildren(rootItem, files, 0);
    m_tree->expand(rootItem->index());
    expandMatching(*m_tree, *m_model, QModelIndex(), expanded);
    if (!selected.empty()) {
        static_cast<void>(select(selected));
    }
    if (!m_search->text().isEmpty()) {
        setSpeedSearch(m_search->text());
    }
}

void FileSystemView::addChildren(QStandardItem* parent, const nav::ProjectFiles& files, std::size_t node) {
    for (const std::size_t child : files.node(node).children) {
        const nav::ProjectFiles::Node& entry = files.node(child);
        const QIcon icon = entry.directory ? themedIcon(icons::kNodesFolder) : workbench_detail::fileIcon(entry.name);
        auto* item = new QStandardItem(icon, fromUtf8(entry.name));
        item->setData(fromUtf8(entry.path), kPathRole);
        item->setData(entry.directory, kDirectoryRole);
        item->setToolTip(workbench_detail::nativePath(entry.path));
        parent->appendRow(item);
        if (entry.directory) {
            addChildren(item, files, child);
        }
    }
}

int FileSystemView::setSpeedSearch(const QString& query) {
    if (!query.isEmpty()) {
        m_search->show();
    }
    m_search->setText(query);
    int matches = 0;
    QModelIndex first;
    filter(m_model->invisibleRootItem(), toUtf8(query.toLower()), matches, first);
    return matches;
}

QString FileSystemView::speedSearch() const { return m_search->text(); }

bool FileSystemView::filter(QStandardItem* parent, const std::string& query, int& matches, QModelIndex& first) {
    bool any = false;
    for (int row = 0; row < parent->rowCount(); ++row) {
        QStandardItem* item = parent->child(row);
        const bool self = !query.empty() && search::scoreWord(query, toUtf8(item->text())).has_value();
        if (self) {
            ++matches;
            if (!first.isValid()) {
                first = item->index();
            }
        }
        const bool below = filter(item, query, matches, first);
        const bool visible = query.empty() || self || below;
        m_tree->setRowHidden(row, parent->index(), !visible);
        if (below && !query.empty()) {
            m_tree->expand(item->index());
        }
        any = any || self || below;
    }
    return any;
}

std::string FileSystemView::selectedPath() const {
    const QModelIndex index = m_tree->currentIndex();
    return index.isValid() ? toUtf8(index.data(kPathRole).toString()) : std::string();
}

bool FileSystemView::select(const std::string& path) {
    const QModelIndexList found =
        m_model->match(m_model->index(0, 0), kPathRole, fromUtf8(path), 1, Qt::MatchExactly | Qt::MatchRecursive);
    if (found.isEmpty()) {
        return false;
    }
    for (QModelIndex parent = found.front().parent(); parent.isValid(); parent = parent.parent()) {
        m_tree->expand(parent);
    }
    m_tree->setCurrentIndex(found.front());
    m_tree->scrollTo(found.front());
    return true;
}

void FileSystemView::activateSelected() { activate(m_tree->currentIndex()); }

QStringList FileSystemView::visibleNames() const {
    QStringList names;
    collectVisible(*m_tree, *m_model, QModelIndex(), names);
    return names;
}

void FileSystemView::activate(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    if (index.data(kDirectoryRole).toBool()) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
        return;
    }
    emit fileActivated(index.data(kPathRole).toString());
}

bool FileSystemView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    if (watched == m_search) {
        if (key->key() == Qt::Key_Escape) {
            m_search->clear();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateSelected();
            return true;
        }
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            QCoreApplication::sendEvent(m_tree, event);
            return true;
        }
        return false;
    }
    if (watched == m_tree) {
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && m_search->isHidden()) {
            activateSelected();
            return true;
        }
        // Printable text starts the speed search (Rider: just start typing in a tree).
        const QString typed = key->text();
        const bool plain = (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0;
        if (plain && !typed.isEmpty() && typed.front().isPrint() && !typed.front().isSpace()) {
            m_search->show();
            m_search->setFocus(Qt::OtherFocusReason);
            m_search->setText(m_search->text() + typed);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace frost::ui
