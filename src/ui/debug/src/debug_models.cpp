// SPDX-License-Identifier: Apache-2.0
//
// FramesModel and VariablesModel (see frost/ui/debug_models.hpp): the frame rows, and the variable
// tree with its lazy, paged children.

#include "frost/ui/debug_models.hpp"

#include "frost/ui/debug_controller.hpp"

#include <QBrush>
#include <QColor>
#include <QFont>

#include <algorithm>
#include <utility>

namespace frost::ui {

// --- FramesModel ---

FramesModel::FramesModel(DebugController& controller, QObject* parent)
    : QAbstractListModel(parent), m_controller(controller) {
    connect(&m_controller, &DebugController::framesChanged, this, &FramesModel::refresh);
    connect(&m_controller, &DebugController::frameSelected, this, &FramesModel::refresh);
}

void FramesModel::refresh() {
    beginResetModel();
    endResetModel();
}

int FramesModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    const debug::FrameList& list = m_controller.frames();
    return static_cast<int>(list.frames.size()) + (list.complete || list.frames.empty() ? 0 : 1);
}

bool FramesModel::isLoadMoreRow(int row) const {
    const debug::FrameList& list = m_controller.frames();
    return !list.complete && row == static_cast<int>(list.frames.size());
}

int FramesModel::selectedRow() const {
    const auto& selected = m_controller.selectedFrame();
    if (!selected) {
        return -1;
    }
    const auto& frames = m_controller.frames().frames;
    const auto it = std::ranges::find(frames, selected->id, &debug::DebugFrame::id);
    return it == frames.end() ? -1 : static_cast<int>(it - frames.begin());
}

QVariant FramesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const auto& frames = m_controller.frames().frames;
    if (isLoadMoreRow(index.row())) {
        if (role == Qt::DisplayRole) {
            return QStringLiteral("Load more frames...");
        }
        if (role == kFrameIdRole) {
            return QVariant::fromValue<qlonglong>(-1);
        }
        return {};
    }
    if (index.row() < 0 || index.row() >= static_cast<int>(frames.size())) {
        return {};
    }
    const debug::DebugFrame& frame = frames[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole: {
        const QString name = QString::fromStdString(frame.name);
        if (frame.hasSource()) {
            const std::string& label = frame.sourceName.empty() ? frame.path : frame.sourceName;
            const QString file = label.empty() ? QStringLiteral("Loaded source") : QString::fromStdString(label);
            return QStringLiteral("%1, %2:%3").arg(name, file).arg(frame.line);
        }
        return frame.module.empty() ? name : QStringLiteral("%1 [%2]").arg(name, QString::fromStdString(frame.module));
    }
    case Qt::ToolTipRole:
        if (!frame.hasSource())
            return QString::fromStdString(frame.name);
        if (frame.path.empty())
            return QStringLiteral("%1:%2")
                .arg(frame.sourceName.empty() ? QStringLiteral("Loaded source")
                                              : QString::fromStdString(frame.sourceName))
                .arg(frame.line);
        return QStringLiteral("%1:%2").arg(QString::fromStdString(frame.path)).arg(frame.line);
    case Qt::ForegroundRole:
        if (!frame.hasSource() || frame.subtle) {
            return QBrush(QColor(0x8C, 0x8F, 0x94)); // Library frames are dimmed, as in Rider.
        }
        return {};
    case Qt::FontRole: {
        if (index.row() == selectedRow()) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    }
    case kFrameIdRole: return QVariant::fromValue<qlonglong>(frame.id);
    default: return {};
    }
}

// --- VariablesModel ---

struct VariablesModel::Node {
    enum class Kind : std::uint8_t { Root, Scope, Watch, Variable, LoadMore, Message };

    std::uint64_t id = 0;
    Kind kind = Kind::Variable;
    Node* parent = nullptr;
    QString name;
    QString value;
    QString type;
    QString evaluateName;
    QString memoryReference;
    bool error = false;
    std::int64_t reference = 0;
    std::int64_t indexed = 0;
    std::int64_t nextStart = 0; // A LoadMore row: where the next page starts.
    bool fetched = false;
    bool fetching = false;
    std::vector<std::unique_ptr<Node>> children;

    [[nodiscard]] int row() const {
        if (parent == nullptr) {
            return 0;
        }
        const auto it =
            std::ranges::find_if(parent->children, [this](const auto& child) { return child.get() == this; });
        return static_cast<int>(it - parent->children.begin());
    }
};

VariablesModel::VariablesModel(DebugController& controller, Mode mode, QObject* parent)
    : QAbstractItemModel(parent), m_controller(controller), m_mode(mode), m_root(std::make_unique<Node>()) {
    m_root->kind = Node::Kind::Root;
    connect(&m_controller, &DebugController::frameSelected, this, &VariablesModel::reload);
    connect(&m_controller, &DebugController::valuesFormatChanged, this, &VariablesModel::reload);
    connect(&m_controller, &DebugController::stateChanged, this, [this] {
        if (!m_controller.paused()) {
            ++m_generation;
            clearRoots();
            if (m_mode == Mode::Watches) {
                for (const auto& watch : m_root->children)
                    evaluateWatch(*watch);
            }
        }
    });
}

VariablesModel::~VariablesModel() = default;

VariablesModel::Node* VariablesModel::nodeFor(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : m_root.get();
}

QModelIndex VariablesModel::indexFor(const Node* node, int column) const {
    if (node == nullptr || node == m_root.get()) {
        return {};
    }
    return createIndex(node->row(), column, const_cast<Node*>(node));
}

VariablesModel::Node* VariablesModel::find(std::uint64_t id) const {
    const auto it = m_nodes.find(id);
    return it == m_nodes.end() ? nullptr : it->second;
}

VariablesModel::Node* VariablesModel::adopt(std::unique_ptr<Node> node) {
    node->id = m_nextId++;
    m_nodes.emplace(node->id, node.get());
    return node.release();
}

void VariablesModel::forget(Node& node) {
    for (const auto& child : node.children) {
        forget(*child);
    }
    m_nodes.erase(node.id);
}

void VariablesModel::clearChildren(Node& node) {
    if (node.children.empty()) {
        return;
    }
    beginRemoveRows(indexFor(&node), 0, static_cast<int>(node.children.size()) - 1);
    for (const auto& child : node.children) {
        forget(*child);
    }
    node.children.clear();
    endRemoveRows();
}

void VariablesModel::clearRoots() {
    beginResetModel();
    for (const auto& child : m_root->children) {
        forget(*child);
    }
    if (m_mode == Mode::Scopes) {
        m_root->children.clear();
    } else {
        for (const auto& watch : m_root->children) {
            watch->children.clear();
            watch->fetched = false;
            watch->fetching = false;
            m_nodes.emplace(watch->id, watch.get());
        }
    }
    endResetModel();
}

void VariablesModel::reload() {
    ++m_generation;
    if (m_mode == Mode::Watches) {
        for (const auto& watch : m_root->children) {
            evaluateWatch(*watch);
        }
        return;
    }
    clearRoots();
    const auto& frame = m_controller.selectedFrame();
    if (!frame || !m_controller.paused()) {
        return;
    }
    const std::uint64_t generation = m_generation;
    const QPointer<VariablesModel> self(this);
    ++m_pending;
    m_controller.scopes(frame->id, [self, generation](Result<debug::ScopeList> scopes) {
        if (!self) {
            return;
        }
        --self->m_pending;
        if (generation != self->m_generation) {
            return;
        }
        std::vector<std::unique_ptr<Node>> roots;
        if (!scopes) {
            auto message = std::make_unique<Node>();
            message->kind = Node::Kind::Message;
            message->name = QStringLiteral("Variables are not available");
            message->value = QString::fromStdString(scopes.error().message());
            message->error = true;
            roots.push_back(std::move(message));
        } else {
            for (const debug::dap::Scope& scope : *scopes) {
                auto node = std::make_unique<Node>();
                node->kind = Node::Kind::Scope;
                node->name = QString::fromStdString(scope.name);
                node->reference = scope.variablesReference;
                node->indexed = scope.indexedVariables;
                roots.push_back(std::move(node));
            }
        }
        if (roots.empty()) {
            return;
        }
        self->beginInsertRows({}, 0, static_cast<int>(roots.size()) - 1);
        for (auto& root : roots) {
            Node* raw = self->adopt(std::move(root));
            raw->parent = self->m_root.get();
            self->m_root->children.emplace_back(raw);
        }
        self->endInsertRows();
    });
}

void VariablesModel::addWatch(const QString& expression) {
    const QString trimmed = expression.trimmed();
    if (m_mode != Mode::Watches || trimmed.isEmpty()) {
        return;
    }
    if (std::ranges::any_of(m_root->children, [&](const auto& watch) { return watch->name == trimmed; })) return;
    auto node = std::make_unique<Node>();
    node->kind = Node::Kind::Watch;
    node->name = trimmed;
    const int row = static_cast<int>(m_root->children.size());
    beginInsertRows({}, row, row);
    Node* raw = adopt(std::move(node));
    raw->parent = m_root.get();
    m_root->children.emplace_back(raw);
    const auto id = raw->id;
    const QPointer<VariablesModel> self(this);
    endInsertRows();
    if (self) if (auto* current = self->find(id)) self->evaluateWatch(*current);
}

void VariablesModel::removeWatch(int row) {
    if (m_mode != Mode::Watches || row < 0 || row >= static_cast<int>(m_root->children.size())) {
        return;
    }
    beginRemoveRows({}, row, row);
    forget(*m_root->children[static_cast<std::size_t>(row)]);
    m_root->children.erase(m_root->children.begin() + row);
    endRemoveRows();
}

QStringList VariablesModel::watches() const {
    QStringList out;
    for (const auto& watch : m_root->children) {
        out.push_back(watch->name);
    }
    return out;
}

void VariablesModel::evaluateWatch(Node& node) {
    clearChildren(node);
    node.fetched = false;
    node.fetching = false;
    node.reference = 0;
    node.memoryReference.clear();
    if (!m_controller.paused()) {
        node.value = m_controller.debugging() ? QStringLiteral("(the program is running)")
                                              : QStringLiteral("(no debug session)");
        node.type.clear();
        node.error = false;
        emit dataChanged(indexFor(&node, 0), indexFor(&node, 2));
        return;
    }
    const std::uint64_t id = node.id;
    const std::uint64_t generation = m_generation;
    const QPointer<VariablesModel> self(this);
    ++m_pending;
    m_controller.evaluate(node.name, m_context, [self, id, generation](Result<debug::dap::Variable> result) {
        if (!self) {
            return;
        }
        --self->m_pending;
        Node* target = self->find(id);
        if (generation != self->m_generation || !self->m_controller.paused() || target == nullptr) {
            return;
        }
        self->clearChildren(*target);
        if (result) {
            target->value = QString::fromStdString(result->value);
            target->type = QString::fromStdString(result->type);
            target->evaluateName = QString::fromStdString(result->evaluateName);
            target->memoryReference = QString::fromStdString(result->memoryReference);
            target->reference = result->variablesReference;
            target->indexed = result->indexedVariables;
            target->error = false;
        } else {
            target->value = QString::fromStdString(result.error().message());
            target->type.clear();
            target->reference = 0;
            target->error = true;
        }
        target->fetched = false;
        emit self->dataChanged(self->indexFor(target, 0), self->indexFor(target, 2));
    });
}

void VariablesModel::fetch(Node& node, std::int64_t start, std::int64_t count) {
    node.fetching = true;
    const std::uint64_t id = node.id;
    const std::uint64_t generation = m_generation;
    const QPointer<VariablesModel> self(this);
    ++m_pending;
    m_controller.variables(node.reference, start, count,
                           [self, id, start, count, generation](Result<debug::VariableList> list) {
                               if (self) {
                                   --self->m_pending;
                                   if (generation == self->m_generation && self->m_controller.paused())
                                       self->addChildren(id, std::move(list), start, count);
                               }
                           });
}

void VariablesModel::addChildren(std::uint64_t id, Result<debug::VariableList> list, std::int64_t start,
                                 std::int64_t count) {
    Node* node = find(id);
    if (node == nullptr) {
        return;
    }
    node->fetching = false;
    node->fetched = true;
    // A "Load more" row is replaced by the page it asked for.
    if (!node->children.empty() && node->children.back()->kind == Node::Kind::LoadMore) {
        const int last = static_cast<int>(node->children.size()) - 1;
        beginRemoveRows(indexFor(node), last, last);
        forget(*node->children.back());
        node->children.pop_back();
        endRemoveRows();
    }
    std::vector<std::unique_ptr<Node>> rows;
    if (!list) {
        auto message = std::make_unique<Node>();
        message->kind = Node::Kind::Message;
        message->value = QString::fromStdString(list.error().message());
        message->error = true;
        rows.push_back(std::move(message));
    } else {
        for (const debug::dap::Variable& variable : *list) {
            auto child = std::make_unique<Node>();
            child->kind = Node::Kind::Variable;
            child->name = QString::fromStdString(variable.name);
            child->value = QString::fromStdString(variable.value);
            child->type = QString::fromStdString(variable.type);
            child->evaluateName = QString::fromStdString(variable.evaluateName);
            child->memoryReference = QString::fromStdString(variable.memoryReference);
            child->reference = variable.variablesReference;
            child->indexed = variable.indexedVariables;
            rows.push_back(std::move(child));
        }
        const std::int64_t loaded = start + static_cast<std::int64_t>(list->size());
        if (count > 0 && static_cast<std::int64_t>(list->size()) == count && loaded < node->indexed) {
            auto more = std::make_unique<Node>();
            more->kind = Node::Kind::LoadMore;
            more->name = QStringLiteral("Load more (%1 of %2 shown)").arg(loaded).arg(node->indexed);
            more->nextStart = loaded;
            rows.push_back(std::move(more));
        }
    }
    if (rows.empty()) {
        return;
    }
    const int first = static_cast<int>(node->children.size());
    beginInsertRows(indexFor(node), first, first + static_cast<int>(rows.size()) - 1);
    for (auto& row : rows) {
        Node* raw = adopt(std::move(row));
        raw->parent = node;
        node->children.emplace_back(raw);
    }
    endInsertRows();
}

void VariablesModel::loadMore(const QModelIndex& index) {
    Node* node = nodeFor(index);
    if (!index.isValid() || node->kind != Node::Kind::LoadMore || node->parent == nullptr || node->parent->fetching) {
        return;
    }
    fetch(*node->parent, node->nextStart, kPage);
}

bool VariablesModel::isLoadMoreRow(const QModelIndex& index) const {
    return index.isValid() && nodeFor(index)->kind == Node::Kind::LoadMore;
}

QString VariablesModel::valueAt(const QModelIndex& index) const {
    return index.isValid() ? nodeFor(index)->value : QString();
}

QString VariablesModel::expressionAt(const QModelIndex& index) const {
    return index.isValid() ? nodeFor(index)->evaluateName : QString();
}
QString VariablesModel::memoryReferenceAt(const QModelIndex& index) const {
    return index.isValid() ? nodeFor(index)->memoryReference : QString();
}

QString VariablesModel::nameAt(const QModelIndex& index) const {
    return index.isValid() ? nodeFor(index)->name : QString();
}

std::int64_t VariablesModel::parentReferenceAt(const QModelIndex& index) const {
    if (!index.isValid()) {
        return 0;
    }
    const Node* node = nodeFor(index);
    return node->parent != nullptr ? node->parent->reference : 0;
}

bool VariablesModel::isVariableRow(const QModelIndex& index) const {
    return index.isValid() && nodeFor(index)->kind == Node::Kind::Variable;
}

bool VariablesModel::isWatchRoot(const QModelIndex& index) const {
    return index.isValid() && nodeFor(index)->kind == Node::Kind::Watch;
}

void VariablesModel::updateValueAt(const QModelIndex& index, const QString& value, const QString& type) {
    if (!index.isValid()) {
        return;
    }
    Node* node = nodeFor(index);
    node->value = value;
    if (!type.isEmpty()) {
        node->type = type;
    }
    emit dataChanged(indexFor(node, 0), indexFor(node, 2));
}

QModelIndex VariablesModel::index(int row, int column, const QModelIndex& parent) const {
    const Node* node = nodeFor(parent);
    if (row < 0 || column < 0 || column >= 3 || row >= static_cast<int>(node->children.size())) {
        return {};
    }
    return createIndex(row, column, node->children[static_cast<std::size_t>(row)].get());
}

QModelIndex VariablesModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    return indexFor(nodeFor(child)->parent);
}

int VariablesModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    return static_cast<int>(nodeFor(parent)->children.size());
}

int VariablesModel::columnCount(const QModelIndex&) const { return 3; }

QVariant VariablesModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node* node = nodeFor(index);
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        switch (index.column()) {
        case 0: return node->name;
        case 1: return node->value;
        case 2: return node->type;
        default: return {};
        }
    }
    if (role == Qt::ForegroundRole && ((node->error && index.column() == 1) || node->kind == Node::Kind::LoadMore)) {
        return QBrush(node->error ? QColor(0xE0, 0x6C, 0x75) : QColor(0x58, 0x9D, 0xF6));
    }
    return {};
}

QVariant VariablesModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case 0: return m_mode == Mode::Watches ? QStringLiteral("Expression") : QStringLiteral("Name");
    case 1: return QStringLiteral("Value");
    case 2: return QStringLiteral("Type");
    default: return {};
    }
}

bool VariablesModel::hasChildren(const QModelIndex& parent) const {
    const Node* node = nodeFor(parent);
    if (parent.column() > 0) {
        return false;
    }
    return !node->children.empty() || (node->reference > 0 && !node->fetched);
}

bool VariablesModel::canFetchMore(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return false;
    }
    const Node* node = nodeFor(parent);
    return node->reference > 0 && !node->fetched && !node->fetching && m_controller.paused();
}

void VariablesModel::fetchMore(const QModelIndex& parent) {
    if (!canFetchMore(parent)) {
        return;
    }
    Node* node = nodeFor(parent);
    fetch(*node, 0, node->indexed > kPage ? kPage : 0);
}

} // namespace frost::ui
