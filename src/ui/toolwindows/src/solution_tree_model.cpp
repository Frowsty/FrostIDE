// SPDX-License-Identifier: Apache-2.0
//
// SolutionTreeModel, SolutionFilterModel and SolutionItemDelegate
// (frost/ui/toolwindows/solution_tree_model.hpp).

#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/icon_ids.hpp"

#include "tool_icons.hpp"
#include <QApplication>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QPainter>
#include <QStyle>
#include <QTreeView>
#include <QUrl>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace frost::ui {
namespace {

using project::NodeId;
using project::NodeKind;

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// C and C++ sources and headers by extension (ASCII case ignored).
bool isCodeFileName(std::string_view path) {
    const std::string_view name = io::fileName(path);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string extension(name.substr(dot + 1));
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static constexpr std::array<std::string_view, 18> kCode{
        "c",   "cc",  "cpp", "cxx", "c++",  "h",   "hh", "hpp", "hxx",
        "h++", "inl", "ipp", "ixx", "cppm", "tpp", "cs", "csx", "lua",
    };
    return std::ranges::find(kCode, extension) != kCode.end();
}

NodeId nodeFromInternalId(quintptr id) { return NodeId{static_cast<std::uint32_t>(id)}; }

} // namespace

std::string_view solutionNodeIconId(const project::SolutionModel& model, NodeId id) noexcept {
    const project::Node& node = model.node(id);
    switch (node.kind) {
    case NodeKind::Solution: return icons::kToolwindowStructure;
    case NodeKind::SolutionFolder:
    case NodeKind::Filter: return icons::kNodesFolder;
    case NodeKind::Project: return icons::kBuildPackage;
    case NodeKind::UnsupportedProject: return icons::kStatusWarning;
    case NodeKind::UnavailableProject: return icons::kStatusFailed;
    case NodeKind::File:
        if (node.itemType == project::ItemType::ClCompile || node.itemType == project::ItemType::ClInclude ||
            isCodeFileName(node.path)) {
            return icons::kNodesFileCode;
        }
        return icons::kNodesFile;
    }
    return icons::kNodesFile;
}

bool speedSearchMatches(QStringView text, QStringView query) noexcept {
    return !query.isEmpty() && text.contains(query, Qt::CaseInsensitive);
}

// ---- SolutionTreeModel.

SolutionTreeModel::SolutionTreeModel(QObject* parent)
    : QAbstractItemModel(parent), m_icons(std::make_unique<detail::IconCache>()) {}

SolutionTreeModel::~SolutionTreeModel() = default;

void SolutionTreeModel::setSolution(std::shared_ptr<const project::SolutionModel> solution) {
    beginResetModel();
    m_solution = std::move(solution);
    m_rowInParent.assign(m_solution ? m_solution->nodeCount() : 0, 0);
    if (m_solution) {
        for (std::size_t i = 0; i < m_solution->nodeCount(); ++i) {
            const auto children = m_solution->children(NodeId{static_cast<std::uint32_t>(i)});
            for (std::size_t row = 0; row < children.size(); ++row) {
                m_rowInParent[children[row].value()] = static_cast<int>(row);
            }
        }
    }
    endResetModel();
}

QModelIndex SolutionTreeModel::indexOf(NodeId id) const {
    if (!m_solution || id.value() >= m_solution->nodeCount()) {
        return {};
    }
    return createIndex(m_rowInParent[id.value()], 0, static_cast<quintptr>(id.value()));
}

std::optional<NodeId> SolutionTreeModel::nodeOf(const QModelIndex& index) const {
    if (!m_solution || !index.isValid() || index.model() != this) {
        return std::nullopt;
    }
    return nodeFromInternalId(index.internalId());
}

QModelIndex SolutionTreeModel::indexOfPath(std::string_view path) const {
    if (!m_solution) {
        return {};
    }
    const std::optional<NodeId> node = m_solution->findFileNode(path);
    return node ? indexOf(*node) : QModelIndex();
}

std::vector<NodeId> SolutionTreeModel::displayOrder() const {
    std::vector<NodeId> order;
    if (!m_solution) {
        return order;
    }
    order.reserve(m_solution->nodeCount());
    std::vector<NodeId> stack{project::SolutionModel::root()};
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        order.push_back(id);
        const auto children = m_solution->children(id);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return order;
}

QModelIndex SolutionTreeModel::index(int row, int column, const QModelIndex& parent) const {
    if (!m_solution || column != 0 || row < 0) {
        return {};
    }
    if (!parent.isValid()) {
        return row == 0 ? createIndex(0, 0, quintptr{0}) : QModelIndex();
    }
    const auto children = m_solution->children(nodeFromInternalId(parent.internalId()));
    if (static_cast<std::size_t>(row) >= children.size()) {
        return {};
    }
    return createIndex(row, 0, static_cast<quintptr>(children[static_cast<std::size_t>(row)].value()));
}

QModelIndex SolutionTreeModel::parent(const QModelIndex& child) const {
    if (!m_solution || !child.isValid()) {
        return {};
    }
    const std::optional<NodeId> parentId = m_solution->parent(nodeFromInternalId(child.internalId()));
    return parentId ? indexOf(*parentId) : QModelIndex();
}

int SolutionTreeModel::rowCount(const QModelIndex& parent) const {
    if (!m_solution) {
        return 0;
    }
    if (!parent.isValid()) {
        return 1;
    }
    if (parent.column() != 0) {
        return 0;
    }
    return static_cast<int>(m_solution->children(nodeFromInternalId(parent.internalId())).size());
}

int SolutionTreeModel::columnCount(const QModelIndex& /*parent*/) const { return 1; }

QVariant SolutionTreeModel::data(const QModelIndex& index, int role) const {
    const std::optional<NodeId> id = nodeOf(index);
    if (!id) {
        return {};
    }
    const project::Node& node = m_solution->node(*id);
    switch (role) {
    case Qt::DisplayRole: return qs(node.displayName);
    case Qt::DecorationRole: return m_icons->get(solutionNodeIconId(*m_solution, *id));
    case Qt::ToolTipRole: {
        QString tip = qs(node.path);
        if (node.kind == NodeKind::UnavailableProject && node.project) {
            const project::ProjectInfo& info = m_solution->project(*node.project);
            if (info.error) {
                tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + qs(info.error->message());
            }
        } else if (node.kind == NodeKind::UnsupportedProject && node.project) {
            const project::ProjectInfo& info = m_solution->project(*node.project);
            tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) +
                   tr("%1 projects are not supported yet").arg(qs(project::toString(info.kind)));
        }
        return tip.isEmpty() ? QVariant() : QVariant(tip);
    }
    case NodeIdRole: return QVariant::fromValue(static_cast<quint32>(id->value()));
    case KindRole: return static_cast<int>(node.kind);
    case PathRole: return qs(node.path);
    case ProjectIdRole: return node.project ? static_cast<int>(node.project->value()) : -1;
    case IconIdRole: return qs(solutionNodeIconId(*m_solution, *id));
    case SecondaryTextRole:
        switch (node.kind) {
        case NodeKind::Solution: {
            const std::size_t count = m_solution->projects().size();
            return count == 1 ? tr("1 project") : tr("%1 projects").arg(static_cast<qulonglong>(count));
        }
        case NodeKind::UnavailableProject:
            if (node.project && m_solution->project(*node.project).error &&
                m_solution->project(*node.project).error->message() == "unloaded") {
                return tr("(unloaded)");
            }
            return tr("(unavailable)");
        case NodeKind::UnsupportedProject: return tr("(not supported yet)");
        default: return QString();
        }
    case KeyRole: return qs(node.key);
    case FilterPathRole: return qs(node.filterPath);
    case Qt::ForegroundRole:
        if (m_fileColors && node.kind == NodeKind::File && !node.path.empty()) {
            if (const std::optional<QColor> color = m_fileColors(node.path)) {
                return *color;
            }
        }
        return {};
    default: return {};
    }
}

Qt::ItemFlags SolutionTreeModel::flags(const QModelIndex& index) const {
    const std::optional<NodeId> id = nodeOf(index);
    if (!id) {
        return Qt::NoItemFlags;
    }
    const project::Node& node = m_solution->node(*id);
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (node.kind == NodeKind::File) {
        result |= Qt::ItemNeverHasChildren;
        if (node.project && !node.path.empty()) {
            result |= Qt::ItemIsDragEnabled;
        }
    } else if (node.kind == NodeKind::Filter) {
        result |= Qt::ItemIsDropEnabled;
        if (!node.path.empty()) {
            result |= Qt::ItemIsDragEnabled;
        }
    } else if (node.kind == NodeKind::Project) {
        result |= Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled;
    } else if (node.kind == NodeKind::SolutionFolder || node.kind == NodeKind::Solution) {
        result |= Qt::ItemIsDropEnabled;
    }
    return result;
}

Qt::DropActions SolutionTreeModel::supportedDropActions() const { return Qt::MoveAction | Qt::CopyAction; }

QStringList SolutionTreeModel::mimeTypes() const {
    return {QStringLiteral("application/x-frostide-solution-files"),
            QStringLiteral("application/x-frostide-solution-project"), QStringLiteral("text/uri-list")};
}

QMimeData* SolutionTreeModel::mimeData(const QModelIndexList& indexes) const {
    if (!m_solution) {
        return nullptr;
    }
    for (const QModelIndex& index : indexes) {
        const std::optional<NodeId> id = nodeOf(index);
        if (!id) {
            continue;
        }
        const project::Node& node = m_solution->node(*id);
        if (node.kind == NodeKind::Project && !node.path.empty()) {
            auto* mime = new QMimeData();
            mime->setData(QStringLiteral("application/x-frostide-solution-project"),
                          qs(node.path).toUtf8());
            return mime;
        }
    }
    QString projectPath;
    QStringList filePaths;
    QList<QUrl> urls;
    for (const QModelIndex& index : indexes) {
        const std::optional<NodeId> id = nodeOf(index);
        if (!id) {
            continue;
        }
        const project::Node& node = m_solution->node(*id);
        if ((node.kind != NodeKind::File && node.kind != NodeKind::Filter) || !node.project || node.path.empty()) {
            continue;
        }
        const QString proj = qs(m_solution->project(*node.project).path);
        if (projectPath.isEmpty()) {
            projectPath = proj;
        } else if (projectPath != proj) {
            continue;
        }
        const QString filePath = qs(node.path);
        if (!filePaths.contains(filePath)) {
            filePaths.push_back(filePath);
            urls.push_back(QUrl::fromLocalFile(filePath));
        }
    }
    if (filePaths.isEmpty()) {
        return nullptr;
    }
    auto* mime = new QMimeData();
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << projectPath << filePaths;
    mime->setData(QStringLiteral("application/x-frostide-solution-files"), payload);
    mime->setUrls(urls);
    return mime;
}

bool SolutionTreeModel::canDropMimeData(const QMimeData* data, Qt::DropAction /*action*/, int /*row*/, int /*column*/,
                                        const QModelIndex& parent) const {
    if (!m_solution || data == nullptr) {
        return false;
    }
    const std::optional<NodeId> targetId = nodeOf(parent);
    if (!targetId) {
        return false;
    }
    const project::Node& targetNode = m_solution->node(*targetId);
    if (data->hasFormat(QStringLiteral("application/x-frostide-solution-project"))) {
        return targetNode.kind == NodeKind::Solution || targetNode.kind == NodeKind::SolutionFolder;
    }
    if ((targetNode.kind != NodeKind::Filter && targetNode.kind != NodeKind::Project &&
         targetNode.kind != NodeKind::File) ||
        !targetNode.project) {
        return false;
    }
    const QString targetProjectPath = qs(m_solution->project(*targetNode.project).path);
    if (data->hasFormat(QStringLiteral("application/x-frostide-solution-files"))) {
        QByteArray payload = data->data(QStringLiteral("application/x-frostide-solution-files"));
        QDataStream stream(&payload, QIODevice::ReadOnly);
        QString srcProjectPath;
        QStringList files;
        stream >> srcProjectPath >> files;
        return !files.isEmpty() && srcProjectPath.compare(targetProjectPath, Qt::CaseInsensitive) == 0;
    }
    return data->hasUrls();
}

bool SolutionTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                     const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    std::optional<NodeId> targetId = nodeOf(parent);
    if (!targetId) {
        return false;
    }
    if (data->hasFormat(QStringLiteral("application/x-frostide-solution-project"))) {
        const project::Node& targetNode = m_solution->node(*targetId);
        const QString projPath = QString::fromUtf8(data->data(QStringLiteral("application/x-frostide-solution-project")));
        const QString folderId = targetNode.kind == NodeKind::SolutionFolder ? qs(targetNode.filterPath) : QString();
        if (!projPath.isEmpty()) {
            emit projectDroppedOnSolutionFolder(projPath, folderId);
            return true;
        }
        return false;
    }
    if (m_solution->node(*targetId).kind == NodeKind::File) {
        targetId = m_solution->parent(*targetId);
        if (!targetId) {
            return false;
        }
    }
    const project::Node& targetNode = m_solution->node(*targetId);
    if (!targetNode.project) {
        return false;
    }
    const project::ProjectInfo& projInfo = m_solution->project(*targetNode.project);
    const QString targetProjectPath = qs(projInfo.path);
    QStringList files;
    if (data->hasFormat(QStringLiteral("application/x-frostide-solution-files"))) {
        QByteArray payload = data->data(QStringLiteral("application/x-frostide-solution-files"));
        QDataStream stream(&payload, QIODevice::ReadOnly);
        QString srcProjectPath;
        stream >> srcProjectPath >> files;
    } else if (data->hasUrls()) {
        for (const QUrl& url : data->urls()) {
            if (url.isLocalFile()) {
                files.push_back(url.toLocalFile());
            }
        }
    }
    if (files.isEmpty()) {
        return false;
    }
    const bool isPhysicalTarget =
        (targetNode.kind == NodeKind::Filter && !targetNode.path.empty()) ||
        (targetNode.kind == NodeKind::Project &&
         (m_solution->cppHierarchyMode() == project::CppHierarchyMode::PhysicalFolders ||
          projInfo.kind == project::ProjectKind::CSharp));
    if (isPhysicalTarget) {
        const QString targetDir = targetNode.kind == NodeKind::Filter
                                      ? qs(targetNode.path)
                                      : qs(std::string(io::parentPath(projInfo.path)));
        emit physicalFilesDroppedOnDirectory(targetProjectPath, files, targetDir);
        return true;
    }
    const QString targetFilterPath =
        targetNode.kind == NodeKind::Filter ? qs(targetNode.filterPath) : QString();
    emit filesDroppedOnFilter(targetProjectPath, files, targetFilterPath);
    return true;
}

// ---- SolutionFilterModel.

SolutionFilterModel::SolutionFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(true);
}

void SolutionFilterModel::setQuery(const QString& query) {
    if (query == m_query) {
        return;
    }
    beginFilterChange();
    m_query = query;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool SolutionFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (m_query.isEmpty()) {
        return true;
    }
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return speedSearchMatches(index.data(Qt::DisplayRole).toString(), m_query);
}

void SolutionTreeModel::setFileColorProvider(FileColorProvider provider) {
    m_fileColors = std::move(provider);
    refreshFileColors();
}

void SolutionTreeModel::refreshFileColors() {
    std::vector<QModelIndex> parents{QModelIndex()};
    while (!parents.empty()) {
        const QModelIndex parent = parents.back();
        parents.pop_back();
        const int rows = rowCount(parent);
        if (rows == 0) {
            continue;
        }
        emit dataChanged(index(0, 0, parent), index(rows - 1, 0, parent), {Qt::ForegroundRole});
        for (int row = 0; row < rows; ++row) {
            parents.push_back(index(row, 0, parent));
        }
    }
}

// ---- SolutionItemDelegate.

SolutionItemDelegate::SolutionItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent), m_icons(std::make_unique<detail::IconCache>()) {}

SolutionItemDelegate::~SolutionItemDelegate() = default;

void SolutionItemDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    const auto kind = static_cast<NodeKind>(index.data(SolutionTreeModel::KindRole).toInt());
    if (kind != NodeKind::SolutionFolder && kind != NodeKind::Filter) {
        return;
    }
    const auto* tree = qobject_cast<const QTreeView*>(option->widget);
    if (tree != nullptr && tree->isExpanded(index)) {
        if (QIcon open = m_icons->get(icons::kNodesFolderOpen); !open.isNull()) {
            option->icon = open;
        }
    }
}

void SolutionItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget* widget = opt.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QString secondary = index.data(SolutionTreeModel::SecondaryTextRole).toString();
    if (secondary.isEmpty()) {
        return;
    }
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const int nameWidth = std::min(opt.fontMetrics.horizontalAdvance(opt.text), textRect.width());
    const int gap = opt.fontMetrics.horizontalAdvance(QLatin1Char(' ')) * 2;
    const QRect rect = textRect.adjusted(nameWidth + gap, 0, 0, 0);
    if (rect.width() <= 0) {
        return;
    }
    const bool selected = (opt.state & QStyle::State_Selected) != 0;
    painter->save();
    painter->setFont(opt.font);
    painter->setPen(selected ? opt.palette.color(QPalette::HighlightedText)
                             : detail::themeColor(ColorKey::LabelSecondaryForeground,
                                                  opt.palette.color(QPalette::PlaceholderText)));
    painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft,
                      opt.fontMetrics.elidedText(secondary, Qt::ElideRight, rect.width()));
    painter->restore();
}

QSize SolutionItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    const QString secondary = index.data(SolutionTreeModel::SecondaryTextRole).toString();
    if (!secondary.isEmpty()) {
        size.rwidth() += option.fontMetrics.horizontalAdvance(secondary) +
                         option.fontMetrics.horizontalAdvance(QLatin1Char(' ')) * 2;
    }
    return size;
}

} // namespace frost::ui
