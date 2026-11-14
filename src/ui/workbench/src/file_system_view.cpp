// SPDX-License-Identifier: Apache-2.0
//
// FileSystemView (frost/ui/workbench/file_system_view.hpp).

#include "frost/ui/workbench/file_system_view.hpp"

#include "frost/search/fuzzy_path.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/nav/paths.hpp"

#include "file_tree_filter.hpp"
#include "workbench_util.hpp"
#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QIODevice>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <set>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::text;
using workbench_detail::themedIcon;
using workbench_detail::toUtf8;

namespace {

constexpr int kPathRole = FileSystemView::PathRole;
constexpr int kDirectoryRole = FileSystemView::DirectoryRole;

void collectVisible(const QTreeView& tree, const QAbstractItemModel& model, const QModelIndex& parent,
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

// A tree index contains only a node number; no per-row Qt objects are created.
class ProjectTreeModel final : public QAbstractItemModel {
public:
    explicit ProjectTreeModel(QObject* parent) : QAbstractItemModel(parent) {}
    void setFiles(std::shared_ptr<const nav::ProjectFiles> files,
                  std::shared_ptr<const workbench_detail::FileTreeFilter> filter = {}) {
        beginResetModel();
        m_files = std::move(files);
        m_filter = std::move(filter);
        endResetModel();
    }
    [[nodiscard]] const std::shared_ptr<const nav::ProjectFiles>& files() const { return m_files; }
    [[nodiscard]] bool filtered() const { return static_cast<bool>(m_filter); }
    [[nodiscard]] const std::shared_ptr<const workbench_detail::FileTreeFilter>& filterSnapshot() const {
        return m_filter;
    }
    QModelIndex indexForNode(std::size_t node) const {
        if (!m_files || node >= m_files->nodes().size() || (m_filter && !m_filter->visible[node]))
            return {};
        if (node == 0)
            return index(0, 0);
        return createIndex(static_cast<int>(m_filter ? m_filter->rows[node] : m_files->node(node).row), 0,
                           quintptr(node + 1));
    }
    void setColors(FileSystemView::FileColorProvider provider) { m_colors = std::move(provider); }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override {
        if (!m_files || column != 0 || row < 0)
            return {};
        if (!parent.isValid())
            return row == 0 && !m_files->nodes().empty() && (!m_filter || m_filter->visible[0])
                       ? createIndex(0, 0, quintptr(1))
                       : QModelIndex();
        const auto node = parent.internalId() - 1;
        const auto position = static_cast<std::size_t>(row);
        if (m_filter) {
            const auto begin = m_filter->offsets[node];
            const auto end = m_filter->offsets[node + 1];
            return position < end - begin ? createIndex(row, 0, quintptr(m_filter->children[begin + position] + 1))
                                          : QModelIndex();
        }
        const auto& children = m_files->node(node).children;
        return position < children.size() ? createIndex(row, 0, quintptr(children[position] + 1)) : QModelIndex();
    }
    QModelIndex parent(const QModelIndex& child) const override {
        if (!child.isValid() || !m_files)
            return {};
        const auto parentNode = m_files->node(child.internalId() - 1).parent;
        if (!parentNode)
            return {};
        if (*parentNode == 0)
            return createIndex(0, 0, quintptr(1));
        return createIndex(static_cast<int>(m_filter ? m_filter->rows[*parentNode] : m_files->node(*parentNode).row), 0,
                           quintptr(*parentNode + 1));
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        if (!m_files || parent.column() > 0)
            return 0;
        if (!parent.isValid())
            return m_files->nodes().empty() || (m_filter && !m_filter->visible[0]) ? 0 : 1;
        const auto node = parent.internalId() - 1;
        return static_cast<int>(m_filter ? m_filter->offsets[node + 1] - m_filter->offsets[node]
                                         : m_files->node(node).children.size());
    }
    int columnCount(const QModelIndex& = {}) const override { return 1; }
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (!index.isValid() || !m_files)
            return {};
        const auto& node = m_files->node(index.internalId() - 1);
        if (role == Qt::DisplayRole)
            return fromUtf8(node.name);
        if (role == Qt::DecorationRole)
            return node.directory ? themedIcon(icons::kNodesFolder) : workbench_detail::fileIcon(node.name);
        if (role == Qt::ToolTipRole)
            return workbench_detail::nativePath(node.path);
        if (role == kPathRole)
            return fromUtf8(node.path);
        if (role == kDirectoryRole)
            return node.directory;
        if (role == Qt::ForegroundRole && m_colors) {
            const auto color = m_colors(node.path);
            if (color)
                return *color;
        }
        return {};
    }
    QModelIndex indexForPath(const std::string& path) const {
        if (!m_files)
            return {};
        const auto node = m_files->find(path);
        if (!node || (m_filter && !m_filter->visible[*node]))
            return {};
        if (*node == 0)
            return index(0, 0);
        return createIndex(static_cast<int>(m_filter ? m_filter->rows[*node] : m_files->node(*node).row), 0,
                           quintptr(*node + 1));
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        if (!index.isValid() || !m_files)
            return Qt::NoItemFlags;
        const auto nodeIdx = index.internalId() - 1;
        const auto& node = m_files->node(nodeIdx);
        Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (nodeIdx != 0)
            result |= Qt::ItemIsDragEnabled;
        if (node.directory)
            result |= Qt::ItemIsDropEnabled;
        else
            result |= Qt::ItemNeverHasChildren;
        return result;
    }
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }
    QStringList mimeTypes() const override {
        return {QStringLiteral("application/x-frostide-fs-paths"), QStringLiteral("text/uri-list")};
    }
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        if (!m_files)
            return nullptr;
        QStringList paths;
        QList<QUrl> urls;
        for (const QModelIndex& idx : indexes) {
            if (!idx.isValid())
                continue;
            const auto nodeIdx = idx.internalId() - 1;
            if (nodeIdx == 0)
                continue;
            const QString p = fromUtf8(m_files->node(nodeIdx).path);
            if (!p.isEmpty() && !paths.contains(p)) {
                paths.push_back(p);
                urls.push_back(QUrl::fromLocalFile(p));
            }
        }
        if (paths.isEmpty())
            return nullptr;
        auto* mime = new QMimeData();
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << paths;
        mime->setData(QStringLiteral("application/x-frostide-fs-paths"), payload);
        mime->setUrls(urls);
        return mime;
    }
    bool canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const override {
        if (!m_files || !data || !parent.isValid())
            return false;
        return data->hasFormat(QStringLiteral("application/x-frostide-fs-paths")) || data->hasUrls();
    }
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override {
        if (!canDropMimeData(data, action, row, column, parent))
            return false;
        const auto nodeIdx = parent.internalId() - 1;
        const auto& targetNode = m_files->node(nodeIdx);
        const QString targetDir =
            targetNode.directory ? fromUtf8(targetNode.path) : fromUtf8(nav::parentOf(targetNode.path));
        QStringList sourcePaths;
        if (data->hasFormat(QStringLiteral("application/x-frostide-fs-paths"))) {
            QByteArray payload = data->data(QStringLiteral("application/x-frostide-fs-paths"));
            QDataStream stream(&payload, QIODevice::ReadOnly);
            stream >> sourcePaths;
        } else if (data->hasUrls()) {
            for (const QUrl& url : data->urls()) {
                if (url.isLocalFile())
                    sourcePaths.push_back(url.toLocalFile());
            }
        }
        if (sourcePaths.isEmpty() || targetDir.isEmpty())
            return false;
        if (m_dropCallback)
            m_dropCallback(sourcePaths, targetDir);
        return true;
    }
    void setDropCallback(std::function<void(const QStringList&, const QString&)> cb) { m_dropCallback = std::move(cb); }

private:
    std::shared_ptr<const nav::ProjectFiles> m_files;
    std::shared_ptr<const workbench_detail::FileTreeFilter> m_filter;
    FileSystemView::FileColorProvider m_colors;
    std::function<void(const QStringList&, const QString&)> m_dropCallback;
};

namespace {
struct FilterResult {
    std::shared_ptr<const nav::ProjectFiles> files;
    std::shared_ptr<const workbench_detail::FileTreeFilter> filter;
    std::uint64_t generation = 0;
};

sched::Task<FilterResult> prepareFilter(std::shared_ptr<const nav::ProjectFiles> files, std::string query,
                                        std::uint64_t generation, std::stop_token token) {
    auto filter = workbench_detail::filterFileTree(*files, query, token);
    if (!filter) {
        files.reset();
        co_return FilterResult{};
    }
    co_return FilterResult{std::move(files), std::make_shared<workbench_detail::FileTreeFilter>(std::move(*filter)),
                           generation};
}

sched::Task<void> retireFilteredSnapshot(std::shared_ptr<const nav::ProjectFiles> files,
                                         std::shared_ptr<const workbench_detail::FileTreeFilter> filter = {}) {
    files.reset();
    filter.reset();
    co_return;
}
sched::Task<void> retireExpansionPaths(std::deque<std::set<std::string>> paths) {
    paths.clear();
    co_return;
}

} // namespace

struct FileSystemView::FilterState {
    explicit FilterState(sched::IScheduler* scheduler, FileSystemView* owner) : scheduler(scheduler), timer(owner) {
        timer.setObjectName(QStringLiteral("projectFilterTimer"));
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, owner, [owner] { owner->pollFilter(); });
    }
    sched::IScheduler* scheduler;
    QTimer timer;
    std::optional<sched::JoinHandle<FilterResult>> job;
    std::stop_source stop;
    std::shared_ptr<const nav::ProjectFiles> desired;
    std::optional<FilterResult> ready;
    std::uint64_t generation = 0;
    std::size_t expandAt = 0;
    std::deque<std::set<std::string>> restore;
    std::string restoreRoot;
};

FileSystemView::FileSystemView(QWidget* parent, sched::IScheduler* scheduler)
    : QWidget(parent), m_tree(new QTreeView(this)), m_model(new ProjectTreeModel(this)), m_search(new QLineEdit(this)),
      m_empty(new QLabel(text("No folder is open. Open a folder or a solution to see its files here."), this)) {
    if (scheduler)
        m_filterState = std::make_unique<FilterState>(scheduler, this);
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
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(true);
    m_tree->setDragDropMode(QAbstractItemView::DragDrop);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_model->setDropCallback([this](const QStringList& srcPaths, const QString& dstDir) {
        emit movePathsToDirectoryRequested(srcPaths, dstDir);
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->installEventFilter(this);
    m_tree->viewport()->installEventFilter(this);
    m_search->installEventFilter(this);
    m_empty->setObjectName(QStringLiteral("fileSystemEmpty"));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_empty, 1);
    m_tree->hide();
    connect(m_tree, &QTreeView::customContextMenuRequested, this, [this](QPoint pos) {
        const QModelIndex index = m_tree->indexAt(pos);
        if (index.isValid()) {
            if (m_tree->selectionModel() && m_tree->selectionModel()->isSelected(index)) {
                m_tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            } else {
                m_tree->setCurrentIndex(index);
            }
        }
        std::unique_ptr<QMenu> menu(createContextMenu(index, this));
        if (!menu->isEmpty()) {
            menu->exec(m_tree->viewport()->mapToGlobal(pos));
        }
    });
    connect(m_tree, &QTreeView::expanded, this,
            [this](const QModelIndex& index) { m_expanded.insert(toUtf8(index.data(kPathRole).toString())); });
    connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        const auto path = toUtf8(index.data(kPathRole).toString());
        m_expanded.erase(path);
        if (m_filterState) {
            for (auto& pending : m_filterState->restore)
                pending.erase(path);
        }
    });
    for (auto* bar : {m_tree->verticalScrollBar(), m_tree->horizontalScrollBar()}) {
        connect(bar, &QScrollBar::actionTriggered, this, [this] { m_restoreScroll.reset(); });
        connect(bar, &QScrollBar::sliderPressed, this, [this] { m_restoreScroll.reset(); });
    }
    connect(m_tree, &QTreeView::activated, this, [this](const QModelIndex& index) { activate(index); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& value) {
        if (m_filterState) {
            if (value.isEmpty()) {
                m_search->hide();
                if (m_filterState->desired) {
                    auto desired = m_filterState->desired;
                    setFiles(std::move(desired));
                }
                m_tree->setFocus(Qt::OtherFocusReason);
            } else if (m_filterState->desired && !m_filterState->desired->root().empty()) {
                m_displayCurrent = false;
                m_tree->hide();
                scheduleFilteredFiles(m_filterState->desired);
            }
            return;
        }
        if (value.isEmpty()) {
            m_search->hide();
            m_tree->setFocus(Qt::OtherFocusReason);
        }
        int matches = 0;
        QModelIndex first;
        filter(QModelIndex(), toUtf8(value.toLower()), matches, first);
        if (first.isValid()) {
            m_tree->setCurrentIndex(first);
            m_tree->scrollTo(first);
        }
    });
}

FileSystemView::~FileSystemView() {
    if (!m_filterState)
        return;
    if (!m_expanded.empty())
        m_filterState->restore.emplace_back(std::move(m_expanded));
    if (!m_filterState->restore.empty()) {
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireExpansionPaths(std::move(m_filterState->restore)))
            .detach();
    }
    if (m_filterState->job) {
        m_filterState->stop.request_stop();
        auto result = m_filterState->job->join();
        if (result.files)
            m_filterState->scheduler
                ->spawn(sched::Lane::Background,
                        retireFilteredSnapshot(std::move(result.files), std::move(result.filter)))
                .detach();
    }
    auto old = m_model->files();
    auto oldFilter = m_model->filterSnapshot();
    m_model->setFiles({});
    if (old)
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(old), std::move(oldFilter)))
            .detach();
    if (m_filterState->desired)
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(m_filterState->desired)))
            .detach();
    if (m_filterState->ready && m_filterState->ready->files)
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(m_filterState->ready->files),
                                                                    std::move(m_filterState->ready->filter)))
            .detach();
}

void FileSystemView::scheduleFilteredFiles(std::shared_ptr<const nav::ProjectFiles> files) {
    auto& state = *m_filterState;
    auto oldDesired = std::move(state.desired);
    state.desired = std::move(files);
    if (oldDesired)
        state.scheduler->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(oldDesired))).detach();
    ++state.generation;
    state.ready.reset();
    state.stop.request_stop();
    if (!state.job)
        startFilter();
    state.timer.start();
}

void FileSystemView::startFilter() {
    auto& state = *m_filterState;
    if (!state.desired || state.desired->root().empty() || m_search->text().isEmpty())
        return;
    state.stop = std::stop_source();
    state.job.emplace(
        state.scheduler->spawn(sched::Lane::Background, prepareFilter(state.desired, toUtf8(m_search->text().toLower()),
                                                                      state.generation, state.stop.get_token())));
}

void FileSystemView::queueExpansionRestore(const std::string& root) {
    auto& state = *m_filterState;
    if (state.restoreRoot != root) {
        if (!state.restore.empty())
            state.scheduler->spawn(sched::Lane::Background, retireExpansionPaths(std::move(state.restore))).detach();
        if (!m_expanded.empty()) {
            std::deque<std::set<std::string>> obsolete;
            obsolete.emplace_back(std::move(m_expanded));
            state.scheduler->spawn(sched::Lane::Background, retireExpansionPaths(std::move(obsolete))).detach();
        }
        state.restoreRoot = root;
    } else if (!m_expanded.empty()) {
        state.restore.emplace_back(std::move(m_expanded));
    }
    if (!state.restore.empty())
        state.timer.start();
}

void FileSystemView::pollFilter() {
    auto& state = *m_filterState;
    if (state.job && state.job->ready()) {
        auto result = state.job->join();
        state.job.reset();
        if (result.generation == state.generation && result.filter) {
            state.ready = std::move(result);
            publishFilteredFiles();
        } else {
            if (result.files)
                state.scheduler
                    ->spawn(sched::Lane::Background,
                            retireFilteredSnapshot(std::move(result.files), std::move(result.filter)))
                    .detach();
            startFilter();
        }
    }
    if (state.ready && state.ready->filter) {
        const auto& nodes = state.ready->filter->expand;
        const auto end = std::min(nodes.size(), state.expandAt + 64);
        for (; state.expandAt < end; ++state.expandAt)
            m_tree->expand(m_model->indexForNode(nodes[state.expandAt]));
        if (state.expandAt == nodes.size())
            state.ready.reset();
    }
    std::size_t restored = 0;
    while (!state.restore.empty() && restored < 64) {
        if (state.restore.front().empty()) {
            state.restore.pop_front();
            continue;
        }
        auto entry = state.restore.front().extract(state.restore.front().begin());
        const QModelIndex index = m_model->indexForPath(entry.value());
        if (index.isValid())
            m_tree->expand(index);
        m_expanded.insert(std::move(entry.value()));
        ++restored;
    }
    restoreScrollPosition();
    if (!state.job && !state.ready && state.restore.empty())
        state.timer.stop();
}

void FileSystemView::restoreScrollPosition() {
    if (!m_restoreScroll)
        return;
    m_tree->horizontalScrollBar()->setValue(m_restoreScroll->x());
    m_tree->verticalScrollBar()->setValue(m_restoreScroll->y());
    if (!m_filterState || m_filterState->restore.empty())
        m_restoreScroll.reset();
}

void FileSystemView::publishFilteredFiles() {
    auto& state = *m_filterState;
    auto old = m_model->files();
    auto oldFilter = m_model->filterSnapshot();
    queueExpansionRestore(state.ready->files->root());
    m_model->setFiles(state.ready->files, state.ready->filter);
    m_displayCurrent = true;
    m_tree->setVisible(true);
    m_empty->setVisible(false);
    state.expandAt = 0;
    const auto first = state.ready->filter->first;
    if (first < state.ready->files->nodes().size()) {
        const auto index = m_model->indexForNode(first);
        m_tree->setCurrentIndex(index);
        m_tree->scrollTo(index);
    }
    if (old)
        state.scheduler->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(old), std::move(oldFilter)))
            .detach();
}

void FileSystemView::setFiles(std::shared_ptr<const nav::ProjectFiles> files) {
    const bool sameRoot = files && m_model->files() && files->root() == m_model->files()->root();
    if (!sameRoot)
        m_restoreScroll.reset();
    const auto scroll = sameRoot ? std::optional(m_restoreScroll.value_or(QPoint(m_tree->horizontalScrollBar()->value(),
                                                                                 m_tree->verticalScrollBar()->value())))
                                 : std::nullopt;
    if (m_filterState) {
        if (files && !files->root().empty() && !m_search->text().isEmpty()) {
            // A root can be closed and reopened under the same path while a job is pending.
            m_displayCurrent = false;
            m_tree->hide();
            scheduleFilteredFiles(std::move(files));
            return;
        }
        ++m_filterState->generation;
        m_filterState->stop.request_stop();
        auto oldDesired = std::move(m_filterState->desired);
        m_filterState->desired = files;
        if (oldDesired)
            m_filterState->scheduler->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(oldDesired)))
                .detach();
        m_filterState->ready.reset();
    }
    std::set<std::string> expanded;
    if (m_filterState)
        queueExpansionRestore(files ? files->root() : std::string());
    else
        expanded = std::move(m_expanded);
    const std::string selected = sameRoot && m_displayCurrent ? selectedPath() : std::string();
    const bool hasRoot = files && !files->root().empty() && !files->nodes().empty();
    auto oldSnapshot = m_model->files();
    auto oldFilter = m_model->filterSnapshot();
    m_model->setFiles(std::move(files));
    m_restoreScroll = scroll;
    if (oldSnapshot && m_filterState) {
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(oldSnapshot), std::move(oldFilter)))
            .detach();
    }
    m_displayCurrent = true;
    m_tree->setVisible(hasRoot);
    m_empty->setVisible(!hasRoot);
    if (!hasRoot) {
        m_restoreScroll.reset();
        m_pendingSelectPath.clear();
        return;
    }
    if (!sameRoot)
        m_tree->expand(m_model->index(0, 0));
    for (const auto& path : expanded) {
        const QModelIndex index = m_model->indexForPath(path);
        if (index.isValid())
            m_tree->expand(index);
    }
    if (!selected.empty()) {
        // Restoring selection must not reveal a child the user deliberately collapsed.
        m_tree->selectionModel()->setCurrentIndex(m_model->indexForPath(selected), QItemSelectionModel::ClearAndSelect);
    }
    restoreScrollPosition();
    if (!m_pendingSelectPath.empty()) {
        if (select(m_pendingSelectPath) || (m_model->files() && m_model->files()->nodes().size() > 1)) {
            m_pendingSelectPath.clear();
        }
    }
    if (!m_search->text().isEmpty())
        setSpeedSearch(m_search->text());
}

void FileSystemView::setFileColorProvider(FileColorProvider provider) {
    m_fileColors = std::move(provider);
    m_model->setColors(m_fileColors);
    refreshFileColors();
}

void FileSystemView::refreshFileColors() { m_tree->viewport()->update(); }

int FileSystemView::setSpeedSearch(const QString& query) {
    if (!query.isEmpty()) {
        m_search->show();
    }
    m_search->setText(query);
    int matches = 0;
    if (m_filterState) {
        const auto files = m_filterState->desired;
        const auto lower = toUtf8(query.toLower());
        if (files && !lower.empty()) {
            for (const auto& node : files->nodes()) {
                if (search::scoreWord(lower, node.name))
                    ++matches;
            }
        }
        return matches;
    }
    QModelIndex first;
    filter(QModelIndex(), toUtf8(query.toLower()), matches, first);
    return matches;
}

QString FileSystemView::speedSearch() const { return m_search->text(); }

bool FileSystemView::filter(const QModelIndex& parent, const std::string& query, int& matches, QModelIndex& first) {
    bool any = false;
    for (int row = 0; row < m_model->rowCount(parent); ++row) {
        const QModelIndex item = m_model->index(row, 0, parent);
        const bool self = !query.empty() && search::scoreWord(query, toUtf8(item.data().toString())).has_value();
        if (self) {
            ++matches;
            if (!first.isValid())
                first = item;
        }
        const bool below = filter(item, query, matches, first);
        m_tree->setRowHidden(row, parent, !(query.empty() || self || below));
        if (below && !query.empty())
            m_tree->expand(item);
        any = any || self || below;
    }
    return any;
}

std::string FileSystemView::selectedPath() const {
    const QModelIndex index = m_tree->currentIndex();
    return index.isValid() ? toUtf8(index.data(kPathRole).toString()) : std::string();
}

QStringList FileSystemView::selectedPaths() const {
    QStringList paths;
    if (!m_tree->selectionModel()) {
        return paths;
    }
    for (const QModelIndex& idx : m_tree->selectionModel()->selectedIndexes()) {
        const QString p = idx.data(kPathRole).toString();
        if (!p.isEmpty() && !paths.contains(p)) {
            paths.push_back(p);
        }
    }
    if (paths.isEmpty() && m_tree->currentIndex().isValid()) {
        const QString p = m_tree->currentIndex().data(kPathRole).toString();
        if (!p.isEmpty()) {
            paths.push_back(p);
        }
    }
    return paths;
}

bool FileSystemView::selectedIsDirectory() const {
    const QModelIndex index = m_tree->currentIndex();
    return index.isValid() && index.data(kDirectoryRole).toBool();
}

std::string FileSystemView::selectedDirectory() const {
    const QModelIndex index = m_tree->currentIndex();
    if (index.isValid()) {
        std::string path = toUtf8(index.data(kPathRole).toString());
        if (index.data(kDirectoryRole).toBool()) {
            return path;
        }
        return std::string(nav::parentOf(path));
    }
    if (m_model->files() && !m_model->files()->root().empty()) {
        return m_model->files()->root();
    }
    return {};
}

QModelIndex FileSystemView::indexOfPath(const std::string& path) const { return m_model->indexForPath(path); }

bool FileSystemView::select(const std::string& path) {
    const QModelIndex found = m_model->indexForPath(path);
    if (!found.isValid())
        return false;
    m_pendingSelectPath.clear();
    m_restoreScroll.reset(); // Explicit navigation wins over passive refresh restoration.
    for (QModelIndex parent = found.parent(); parent.isValid(); parent = parent.parent()) {
        m_tree->expand(parent);
    }
    m_tree->setCurrentIndex(found);
    m_tree->scrollTo(found);
    return true;
}

void FileSystemView::selectWhenAvailable(std::string path) {
    if (path.empty()) {
        m_pendingSelectPath.clear();
        return;
    }
    if (m_displayCurrent && select(path)) {
        m_pendingSelectPath.clear();
        return;
    }
    m_pendingSelectPath = std::move(path);
}

QMenu* FileSystemView::createContextMenu(const QModelIndex& index, QWidget* parent) {
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("fileSystemContextMenu"));
    if (!m_displayCurrent || !m_model->files() || m_model->files()->root().empty()) {
        return menu;
    }
    QString targetPath;
    bool isDirectory = true;
    if (index.isValid()) {
        targetPath = index.data(kPathRole).toString();
        isDirectory = index.data(kDirectoryRole).toBool();
    } else {
        targetPath = fromUtf8(m_model->files()->root());
        isDirectory = true;
    }
    const QString directory = isDirectory ? targetPath : fromUtf8(nav::parentOf(toUtf8(targetPath)));
    if (index.isValid() && !isDirectory) {
        QAction* open = menu->addAction(text("Open"));
        open->setObjectName(QStringLiteral("openFile"));
        connect(open, &QAction::triggered, this, [this, targetPath] { emit fileActivated(targetPath); });
        menu->addSeparator();
    }
    QAction* newFile = menu->addAction(text("New File..."));
    newFile->setObjectName(QStringLiteral("newFile"));
    connect(newFile, &QAction::triggered, this, [this, directory] { emit newFileRequested(directory); });
    QAction* newFolder = menu->addAction(text("New Folder..."));
    newFolder->setObjectName(QStringLiteral("newFolder"));
    connect(newFolder, &QAction::triggered, this, [this, directory] { emit newFolderRequested(directory); });

    menu->addSeparator();
    QStringList selPaths = selectedPaths();
    if (index.isValid() && !targetPath.isEmpty() && !selPaths.contains(targetPath)) {
        selPaths = QStringList{targetPath};
    }
    if (index.isValid() && toUtf8(targetPath) != m_model->files()->root() && !selPaths.isEmpty()) {
        const QString addLabel = selPaths.size() > 1
                                     ? text("Add %1 Items to Project").arg(selPaths.size())
                                     : (isDirectory ? text("Add Folder to Project") : text("Add to Project"));
        QAction* addSelected = menu->addAction(addLabel);
        addSelected->setObjectName(QStringLiteral("addSelectedToProject"));
        connect(addSelected, &QAction::triggered, this,
                [this, selPaths] { emit addPathsToProjectRequested(selPaths); });
    }
    QAction* addExistingFiles = menu->addAction(text("Add Existing Files to Project..."));
    addExistingFiles->setObjectName(QStringLiteral("addExistingFilesToProject"));
    connect(addExistingFiles, &QAction::triggered, this,
            [this, directory] { emit addExistingFilesToProjectRequested(directory); });
    QAction* addExistingFolder = menu->addAction(text("Add Existing Folder to Project..."));
    addExistingFolder->setObjectName(QStringLiteral("addExistingFolderToProject"));
    connect(addExistingFolder, &QAction::triggered, this,
            [this, directory] { emit addExistingFolderToProjectRequested(directory); });
    if (index.isValid() && toUtf8(targetPath) != m_model->files()->root()) {
        menu->addSeparator();
        QAction* cutAct = menu->addAction(text("Cut"));
        cutAct->setObjectName(QStringLiteral("cutItems"));
        connect(cutAct, &QAction::triggered, this, [this, selPaths] { emit cutPathsRequested(selPaths); });
        QAction* copyAct = menu->addAction(text("Copy"));
        copyAct->setObjectName(QStringLiteral("copyItems"));
        connect(copyAct, &QAction::triggered, this, [this, selPaths] { emit copyPathsRequested(selPaths); });
        QAction* pasteAct = menu->addAction(text("Paste"));
        pasteAct->setObjectName(QStringLiteral("pasteItems"));
        pasteAct->setEnabled(!m_canPaste || m_canPaste());
        connect(pasteAct, &QAction::triggered, this,
                [this, directory] { emit pasteIntoDirectoryRequested(directory); });
        QAction* dupAct = menu->addAction(text("Duplicate..."));
        dupAct->setObjectName(QStringLiteral("duplicateItem"));
        connect(dupAct, &QAction::triggered, this,
                [this, targetPath, isDirectory] { emit duplicatePathRequested(targetPath, isDirectory); });
        menu->addSeparator();
        QAction* renameAct = menu->addAction(text("Rename..."));
        renameAct->setObjectName(QStringLiteral("renameItem"));
        connect(renameAct, &QAction::triggered, this,
                [this, targetPath, isDirectory] { emit renameRequested(targetPath, isDirectory); });
        QAction* deleteAct = menu->addAction(text("Delete..."));
        deleteAct->setObjectName(QStringLiteral("deleteItem"));
        connect(deleteAct, &QAction::triggered, this,
                [this, targetPath, isDirectory] { emit deleteRequested(targetPath, isDirectory); });
    } else {
        menu->addSeparator();
        QAction* pasteAct = menu->addAction(text("Paste"));
        pasteAct->setObjectName(QStringLiteral("pasteItems"));
        pasteAct->setEnabled(!m_canPaste || m_canPaste());
        connect(pasteAct, &QAction::triggered, this,
                [this, directory] { emit pasteIntoDirectoryRequested(directory); });
    }
    if (index.isValid() && isDirectory && m_directoryRoles && toUtf8(targetPath) != m_model->files()->root()) {
        // Mark Directory as (UX-23): the folder's role decides whether the project tree, search and
        // the watcher walk it; the current role is checked, Default drops the user's override.
        menu->addSeparator();
        QMenu* roles = menu->addMenu(text("Mark Directory as"));
        roles->setObjectName(QStringLiteral("markDirectoryAs"));
        const project::DirectoryRoleDecision current = m_directoryRoles(toUtf8(targetPath));
        const auto addRole = [&](project::DirectoryRole role, const QString& label, const QString& name) {
            QAction* action = roles->addAction(label);
            action->setObjectName(name);
            action->setCheckable(true);
            action->setChecked(current.role == role);
            const int value = static_cast<int>(role);
            connect(action, &QAction::triggered, this,
                    [this, targetPath, value] { emit directoryRoleRequested(targetPath, value); });
        };
        addRole(project::DirectoryRole::Source, text("Sources"), QStringLiteral("markDirectorySource"));
        addRole(project::DirectoryRole::Library, text("Library (searched, third-party)"),
                QStringLiteral("markDirectoryLibrary"));
        addRole(project::DirectoryRole::Generated, text("Generated (build output, not searched)"),
                QStringLiteral("markDirectoryGenerated"));
        addRole(project::DirectoryRole::Excluded, text("Excluded (not searched)"),
                QStringLiteral("markDirectoryExcluded"));
        roles->addSeparator();
        QAction* reset = roles->addAction(text("Default"));
        reset->setObjectName(QStringLiteral("markDirectoryDefault"));
        reset->setEnabled(current.reason == project::DirectoryRoleReason::Override);
        connect(reset, &QAction::triggered, this, [this, targetPath] { emit directoryRoleRequested(targetPath, -1); });
    }
    menu->addSeparator();
    QAction* copy = menu->addAction(text("Copy Path"));
    copy->setObjectName(QStringLiteral("copyPath"));
    connect(copy, &QAction::triggered, this,
            [targetPath] { QApplication::clipboard()->setText(QDir::toNativeSeparators(targetPath)); });
    QAction* reveal = menu->addAction(text("Open Containing Folder"));
    reveal->setObjectName(QStringLiteral("openContainingFolder"));
    connect(reveal, &QAction::triggered, this, [this, targetPath] { emit openContainingFolderRequested(targetPath); });
    QAction* terminal = menu->addAction(text("Open in Terminal"));
    terminal->setObjectName(QStringLiteral("openInTerminal"));
    connect(terminal, &QAction::triggered, this, [this, directory] { emit openInTerminalRequested(directory); });
    return menu;
}

void FileSystemView::activateSelected() { activate(m_tree->currentIndex()); }

QStringList FileSystemView::visibleNames() const {
    QStringList names;
    collectVisible(*m_tree, *m_model, QModelIndex(), names);
    return names;
}

void FileSystemView::activate(const QModelIndex& index) {
    if (!m_displayCurrent || !index.isValid()) {
        return;
    }
    if (index.data(kDirectoryRole).toBool()) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
        return;
    }
    emit fileActivated(index.data(kPathRole).toString());
}

bool FileSystemView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel ||
        event->type() == QEvent::KeyPress)
        m_restoreScroll.reset(); // User navigation wins over a pending batched restore.
    if (watched == m_tree->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* dragEvent = static_cast<QDropEvent*>(event);
            const QModelIndex targetIdx = m_tree->indexAt(dragEvent->position().toPoint());
            if (targetIdx.isValid() &&
                m_model->canDropMimeData(dragEvent->mimeData(), Qt::MoveAction, -1, -1, targetIdx)) {
                dragEvent->setDropAction(Qt::MoveAction);
                dragEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);
            const QModelIndex targetIdx = m_tree->indexAt(dropEvent->position().toPoint());
            if (targetIdx.isValid() &&
                m_model->dropMimeData(dropEvent->mimeData(), Qt::MoveAction, -1, -1, targetIdx)) {
                dropEvent->setDropAction(Qt::MoveAction);
                dropEvent->accept();
                return true;
            }
        }
    }
    if (watched == m_tree && event->type() == QEvent::ShortcutOverride && m_search->isHidden()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        if ((key->key() == Qt::Key_F2 && mods == Qt::NoModifier) ||
            (key->key() == Qt::Key_F6 && mods == Qt::ShiftModifier) ||
            (key->key() == Qt::Key_Delete && mods == Qt::NoModifier) ||
            (mods == Qt::ControlModifier && (key->key() == Qt::Key_C || key->key() == Qt::Key_X ||
                                             key->key() == Qt::Key_V || key->key() == Qt::Key_D))) {
            event->accept();
            return true;
        }
    }
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
            if (m_displayCurrent)
                QCoreApplication::sendEvent(m_tree, event);
            return true;
        }
        return false;
    }
    if (watched == m_tree) {
        if (m_search->isHidden()) {
            const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
            if ((key->key() == Qt::Key_F2 && mods == Qt::NoModifier) ||
                (key->key() == Qt::Key_F6 && mods == Qt::ShiftModifier)) {
                const QModelIndex idx = m_tree->currentIndex();
                if (idx.isValid() && m_model->files() &&
                    toUtf8(idx.data(kPathRole).toString()) != m_model->files()->root()) {
                    emit renameRequested(idx.data(kPathRole).toString(), idx.data(kDirectoryRole).toBool());
                }
                return true;
            }
            if (key->key() == Qt::Key_Delete && mods == Qt::NoModifier) {
                const QModelIndex idx = m_tree->currentIndex();
                if (idx.isValid() && m_model->files() &&
                    toUtf8(idx.data(kPathRole).toString()) != m_model->files()->root()) {
                    emit deleteRequested(idx.data(kPathRole).toString(), idx.data(kDirectoryRole).toBool());
                }
                return true;
            }
            if (mods == Qt::ControlModifier) {
                if (key->key() == Qt::Key_C) {
                    const QStringList sel = selectedPaths();
                    if (!sel.isEmpty())
                        emit copyPathsRequested(sel);
                    return true;
                }
                if (key->key() == Qt::Key_X) {
                    const QStringList sel = selectedPaths();
                    if (!sel.isEmpty())
                        emit cutPathsRequested(sel);
                    return true;
                }
                if (key->key() == Qt::Key_V) {
                    const std::string dir = selectedDirectory();
                    if (!dir.empty())
                        emit pasteIntoDirectoryRequested(fromUtf8(dir));
                    return true;
                }
                if (key->key() == Qt::Key_D) {
                    const QModelIndex idx = m_tree->currentIndex();
                    if (idx.isValid() && m_model->files() &&
                        toUtf8(idx.data(kPathRole).toString()) != m_model->files()->root()) {
                        emit duplicatePathRequested(idx.data(kPathRole).toString(), idx.data(kDirectoryRole).toBool());
                    }
                    return true;
                }
            }
        }
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
