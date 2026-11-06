// SPDX-License-Identifier: Apache-2.0
//
// ProblemsModel, ProblemsFilterModel and ProblemsView (frost/ui/toolwindows/problems.hpp).

#include "frost/ui/toolwindows/problems.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "tool_icons.hpp"
#include "frost/ui/icon_ids.hpp"
#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <string_view>

namespace frost::ui {
namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr std::array kSeverities{build::Severity::Error, build::Severity::Warning, build::Severity::Message};

std::size_t slot(build::Severity severity) noexcept { return static_cast<std::size_t>(severity); }

// "Lib" for "C:\src\Lib\Lib.vcxproj"; "" without a project.
QString projectName(std::string_view projectPath) {
    std::string_view name = io::fileName(projectPath);
    if (const std::size_t dot = name.rfind('.'); dot != std::string_view::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return qs(name);
}

// The file column: the file name of a resolved path, else the origin as printed ("LINK").
QString fileText(const build::BuildDiagnostic& diagnostic) {
    return diagnostic.path.empty() ? qs(diagnostic.diagnostic.file) : qs(io::fileName(diagnostic.path));
}

QString severityName(build::Severity severity) {
    switch (severity) {
    case build::Severity::Error: return ProblemsModel::tr("Error");
    case build::Severity::Warning: return ProblemsModel::tr("Warning");
    case build::Severity::Message: return ProblemsModel::tr("Message");
    }
    return {};
}

QString countText(std::size_t count, build::Severity severity) {
    const auto n = static_cast<qulonglong>(count);
    switch (severity) {
    case build::Severity::Error: return count == 1 ? ProblemsView::tr("1 Error") : ProblemsView::tr("%1 Errors").arg(n);
    case build::Severity::Warning:
        return count == 1 ? ProblemsView::tr("1 Warning") : ProblemsView::tr("%1 Warnings").arg(n);
    case build::Severity::Message:
        return count == 1 ? ProblemsView::tr("1 Message") : ProblemsView::tr("%1 Messages").arg(n);
    }
    return {};
}

} // namespace

// ---- ProblemsModel.

ProblemsModel::ProblemsModel(QObject* parent)
    : QAbstractTableModel(parent), m_icons(std::make_unique<detail::IconCache>()) {}

ProblemsModel::~ProblemsModel() = default;

void ProblemsModel::add(const build::BuildDiagnostic& diagnostic, DiagnosticSource source) {
    const int row = static_cast<int>(m_problems.size());
    beginInsertRows({}, row, row);
    m_problems.push_back(Problem{diagnostic, source});
    endInsertRows();
}

void ProblemsModel::setProblems(DiagnosticSource source, const std::vector<build::BuildDiagnostic>& diagnostics) {
    beginResetModel();
    std::erase_if(m_problems, [source](const Problem& problem) { return problem.source == source; });
    for (const build::BuildDiagnostic& diagnostic : diagnostics) {
        m_problems.push_back(Problem{diagnostic, source});
    }
    endResetModel();
}

void ProblemsModel::clear(DiagnosticSource source) { setProblems(source, {}); }

build::DiagnosticCounts ProblemsModel::counts() const noexcept {
    build::DiagnosticCounts counts;
    for (const Problem& problem : m_problems) {
        switch (problem.diagnostic.diagnostic.severity) {
        case build::Severity::Error: ++counts.errors; break;
        case build::Severity::Warning: ++counts.warnings; break;
        case build::Severity::Message: ++counts.messages; break;
        }
    }
    return counts;
}

std::optional<SourceLocation> ProblemsModel::locationAt(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= m_problems.size()) {
        return std::nullopt;
    }
    const build::BuildDiagnostic& d = m_problems[static_cast<std::size_t>(row)].diagnostic;
    if (!d.jumpable || d.path.empty()) {
        return std::nullopt;
    }
    return SourceLocation{d.path, d.diagnostic.line, d.diagnostic.column};
}

int ProblemsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_problems.size());
}

int ProblemsModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant ProblemsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || static_cast<std::size_t>(index.row()) >= m_problems.size()) {
        return {};
    }
    const Problem& problem = m_problems[static_cast<std::size_t>(index.row())];
    const build::BuildDiagnostic& resolved = problem.diagnostic;
    const build::Diagnostic& d = resolved.diagnostic;
    switch (role) {
    case SeverityRole: return static_cast<int>(d.severity);
    case SourceRole: return static_cast<int>(problem.source);
    case PathRole: return qs(resolved.path);
    case JumpableRole: return resolved.jumpable && !resolved.path.empty();
    default: break;
    }
    const auto column = static_cast<Column>(index.column());
    if (role == Qt::DecorationRole) {
        return column == SeverityColumn ? QVariant::fromValue(m_icons->get(detail::severityIconId(d.severity)))
                                        : QVariant();
    }
    if (role == Qt::ToolTipRole) {
        switch (column) {
        case SeverityColumn: return severityName(d.severity);
        case DescriptionColumn: return qs(d.message);
        case ProjectColumn: return qs(d.project);
        case FileColumn: return resolved.path.empty() ? qs(d.file) : qs(resolved.path);
        default: return {};
        }
    }
    if (role == Qt::AccessibleTextRole && column == SeverityColumn) {
        return severityName(d.severity);
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }
    switch (column) {
    case SeverityColumn: return role == SortRole ? QVariant(static_cast<int>(d.severity)) : QVariant(QString());
    case CodeColumn: return qs(d.code);
    case DescriptionColumn: return qs(d.message);
    case ProjectColumn: return projectName(d.project);
    case FileColumn: return fileText(resolved);
    case LineColumn:
        if (role == SortRole) {
            return d.line;
        }
        return d.line > 0 ? QVariant(d.line) : QVariant(QString());
    case SourceColumn: return problem.source == DiagnosticSource::Build ? tr("Build") : tr("Live");
    case ColumnCount: break;
    }
    return {};
}

QVariant ProblemsModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case SeverityColumn: return QString();
    case CodeColumn: return tr("Code");
    case DescriptionColumn: return tr("Description");
    case ProjectColumn: return tr("Project");
    case FileColumn: return tr("File");
    case LineColumn: return tr("Line");
    case SourceColumn: return tr("Source");
    default: return {};
    }
}

// ---- ProblemsFilterModel.

ProblemsFilterModel::ProblemsFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortRole(ProblemsModel::SortRole);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void ProblemsFilterModel::setSeverityShown(build::Severity severity, bool shown) {
    if (m_shown[slot(severity)] == shown) {
        return;
    }
    beginFilterChange();
    m_shown[slot(severity)] = shown;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ProblemsFilterModel::isSeverityShown(build::Severity severity) const noexcept { return m_shown[slot(severity)]; }

void ProblemsFilterModel::setSourceFilter(std::optional<DiagnosticSource> source) {
    if (m_sourceFilter == source) {
        return;
    }
    beginFilterChange();
    m_sourceFilter = source;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ProblemsFilterModel::setText(const QString& text) {
    if (text == m_text) {
        return;
    }
    beginFilterChange();
    m_text = text;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ProblemsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const QAbstractItemModel* source = sourceModel();
    const QModelIndex first = source->index(sourceRow, 0, sourceParent);
    if (m_sourceFilter) {
        const auto rowSource = static_cast<DiagnosticSource>(first.data(ProblemsModel::SourceRole).toInt());
        if (rowSource != *m_sourceFilter) {
            return false;
        }
    }
    const auto severity = static_cast<build::Severity>(first.data(ProblemsModel::SeverityRole).toInt());
    if (slot(severity) >= m_shown.size() || !m_shown[slot(severity)]) {
        return false;
    }
    if (m_text.isEmpty()) {
        return true;
    }
    for (const int column : {ProblemsModel::CodeColumn, ProblemsModel::DescriptionColumn, ProblemsModel::ProjectColumn,
                             ProblemsModel::FileColumn}) {
        if (source->index(sourceRow, column, sourceParent).data().toString().contains(m_text, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

// ---- ProblemsView.

ProblemsView::ProblemsView(QWidget* parent)
    : QWidget(parent), m_model(new ProblemsModel(this)), m_filter(new ProblemsFilterModel(this)),
      m_view(new QTreeView(this)), m_search(new QLineEdit(this)) {
    setObjectName(QStringLiteral("problems"));
    m_filter->setSourceModel(m_model);

    detail::IconCache icons;
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(6, 3, 6, 3);
    toolbar->setSpacing(4);

    auto* scopeGroup = new QButtonGroup(this);
    scopeGroup->setExclusive(true);
    const auto makeScopeTab = [&](const QString& label, const char* name, std::optional<DiagnosticSource> src,
                                  bool checked) {
        auto* tab = new QToolButton(this);
        tab->setObjectName(QString::fromLatin1(name));
        tab->setText(label);
        tab->setCheckable(true);
        tab->setChecked(checked);
        tab->setAutoRaise(true);
        tab->setFocusPolicy(Qt::NoFocus);
        scopeGroup->addButton(tab);
        connect(tab, &QToolButton::toggled, this, [this, src](bool on) {
            if (on) {
                m_filter->setSourceFilter(src);
            }
        });
        toolbar->addWidget(tab);
    };
    makeScopeTab(tr("All Problems"), "problemsScopeAll", std::nullopt, true);
    makeScopeTab(tr("Toolset, Environment"), "problemsScopeBuild", DiagnosticSource::Build, false);
    makeScopeTab(tr("All Solution Files"), "problemsScopeLive", DiagnosticSource::Live, false);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Plain);
    toolbar->addWidget(sep);

    static constexpr std::array<const char*, 3> kButtonNames{"errorsFilter", "warningsFilter", "messagesFilter"};
    for (const build::Severity severity : kSeverities) {
        auto* button = new QToolButton(this);
        button->setObjectName(QString::fromLatin1(kButtonNames[slot(severity)]));
        button->setCheckable(true);
        button->setChecked(true);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(icons.get(detail::severityIconId(severity)));
        connect(button, &QToolButton::toggled, this,
                [this, severity](bool checked) { m_filter->setSeverityShown(severity, checked); });
        m_buttons[slot(severity)] = button;
        toolbar->addWidget(button);
    }
    m_search->setObjectName(QStringLiteral("problemsSearch"));
    m_search->setPlaceholderText(tr("Search Problems"));
    m_search->setClearButtonEnabled(true);
    if (const QIcon searchIcon = icons.get(icons::kActionsSearch); !searchIcon.isNull()) {
        m_search->addAction(searchIcon, QLineEdit::LeadingPosition);
    }
    connect(m_search, &QLineEdit::textChanged, m_filter, &ProblemsFilterModel::setText);
    toolbar->addWidget(m_search, 1);

    m_view->setObjectName(QStringLiteral("problemsTable"));
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setAccessibleDescription(tr("Press Enter or F4 on a diagnostic to open its source."));
    m_view->setModel(m_filter);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setAllColumnsShowFocus(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->header()->setSortIndicatorClearable(true);
    m_view->header()->setSortIndicator(-1, Qt::AscendingOrder); // Arrival order until a header is clicked.
    m_view->setSortingEnabled(true);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(ProblemsModel::DescriptionColumn, QHeaderView::Stretch);
    m_view->installEventFilter(this);
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) { activate(index); });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(toolbar);
    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("problemsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_results = new QStackedWidget(this);
    m_results->addWidget(m_view);
    m_results->addWidget(m_empty);
    layout->addWidget(m_results);

    connect(m_model, &QAbstractItemModel::rowsInserted, this, &ProblemsView::updateCounts);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ProblemsView::updateCounts);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ProblemsView::updateCounts);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &ProblemsView::updateEmptyState);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &ProblemsView::updateEmptyState);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ProblemsView::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::rowsInserted, this, &ProblemsView::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::rowsRemoved, this, &ProblemsView::updateEmptyState);
    connect(m_filter, &QAbstractItemModel::modelReset, this, &ProblemsView::updateEmptyState);
    updateCounts();
    updateEmptyState();
}

ProblemsView::~ProblemsView() = default;

QToolButton* ProblemsView::severityButton(build::Severity severity) const noexcept {
    return slot(severity) < m_buttons.size() ? m_buttons[slot(severity)] : nullptr;
}

void ProblemsView::setController(BuildController* controller) {
    for (const QMetaObject::Connection& connection : m_connections) {
        disconnect(connection);
    }
    m_connections.clear();
    m_controller = controller;
    if (m_controller == nullptr) {
        m_model->clear(DiagnosticSource::Build);
        return;
    }
    m_model->setProblems(DiagnosticSource::Build, m_controller->diagnostics());
    m_connections.push_back(connect(m_controller, &BuildController::diagnosticAdded, this,
                                    [this](const build::BuildDiagnostic& d) { m_model->add(d); }));
    m_connections.push_back(connect(m_controller, &BuildController::diagnosticsCleared, this,
                                    [this] { m_model->clear(DiagnosticSource::Build); }));
}

bool ProblemsView::activate(const QModelIndex& index) {
    if (!index.isValid()) {
        return false;
    }
    const QModelIndex source = index.model() == m_filter ? m_filter->mapToSource(index) : index;
    const std::optional<SourceLocation> location = m_model->locationAt(source.row());
    if (!location) {
        return false;
    }
    emit openLocation(qs(location->path), location->line, location->column);
    return true;
}

bool ProblemsView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F4) &&
            (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            activate(m_view->currentIndex());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ProblemsView::updateCounts() {
    const build::DiagnosticCounts counts = m_model->counts();
    m_buttons[slot(build::Severity::Error)]->setText(countText(counts.errors, build::Severity::Error));
    m_buttons[slot(build::Severity::Warning)]->setText(countText(counts.warnings, build::Severity::Warning));
    m_buttons[slot(build::Severity::Message)]->setText(countText(counts.messages, build::Severity::Message));
}

void ProblemsView::updateEmptyState() {
    const bool hasVisibleProblems = m_filter->rowCount() > 0;
    m_empty->setText(m_model->rowCount() == 0 ? tr("No problems reported")
                                              : tr("No problems match the current filters"));
    m_results->setCurrentWidget(hasVisibleProblems ? static_cast<QWidget*>(m_view) : m_empty);
}

} // namespace frost::ui
