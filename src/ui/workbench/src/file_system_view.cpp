// SPDX-License-Identifier: Apache-2.0
//
// FileSystemView (frost/ui/workbench/file_system_view.hpp).

#include "frost/ui/workbench/file_system_view.hpp"

#include "frost/search/fuzzy_path.hpp"
#include "frost/ui/icon_ids.hpp"

#include "file_tree_filter.hpp"
#include "workbench_util.hpp"
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QTreeView>
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

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kDirectoryRole = Qt::UserRole + 2;

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

private:
    std::shared_ptr<const nav::ProjectFiles> m_files;
    std::shared_ptr<const workbench_detail::FileTreeFilter> m_filter;
    FileSystemView::FileColorProvider m_colors;
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
    m_tree->installEventFilter(this);
    m_search->installEventFilter(this);
    m_empty->setObjectName(QStringLiteral("fileSystemEmpty"));
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_search);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_empty, 1);
    m_tree->hide();
    connect(m_tree, &QTreeView::expanded, this,
            [this](const QModelIndex& index) { m_expanded.insert(toUtf8(index.data(kPathRole).toString())); });
    connect(m_tree, &QTreeView::collapsed, this,
            [this](const QModelIndex& index) { m_expanded.erase(toUtf8(index.data(kPathRole).toString())); });
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
    if (!state.job && !state.ready && state.restore.empty())
        state.timer.stop();
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
    const std::string selected = m_displayCurrent ? selectedPath() : std::string();
    const bool hasRoot = files && !files->root().empty() && !files->nodes().empty();
    auto oldSnapshot = m_model->files();
    auto oldFilter = m_model->filterSnapshot();
    m_model->setFiles(std::move(files));
    if (oldSnapshot && m_filterState) {
        m_filterState->scheduler
            ->spawn(sched::Lane::Background, retireFilteredSnapshot(std::move(oldSnapshot), std::move(oldFilter)))
            .detach();
    }
    m_displayCurrent = true;
    m_tree->setVisible(hasRoot);
    m_empty->setVisible(!hasRoot);
    if (!hasRoot)
        return;
    m_tree->expand(m_model->index(0, 0));
    for (const auto& path : expanded) {
        const QModelIndex index = m_model->indexForPath(path);
        if (index.isValid())
            m_tree->expand(index);
    }
    if (!selected.empty())
        static_cast<void>(select(selected));
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

bool FileSystemView::select(const std::string& path) {
    const QModelIndex found = m_model->indexForPath(path);
    if (!found.isValid())
        return false;
    for (QModelIndex parent = found.parent(); parent.isValid(); parent = parent.parent()) {
        m_tree->expand(parent);
    }
    m_tree->setCurrentIndex(found);
    m_tree->scrollTo(found);
    return true;
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
