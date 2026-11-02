// SPDX-License-Identifier: Apache-2.0
//
// The Problems tool window, FrostIDE's error list (W1, ADR 0039): ProblemsModel, a table of
// diagnostics with a source flag so W2 can add clangd's live diagnostics next to the build's;
// ProblemsFilterModel, the severity toggles and the text filter; and ProblemsView, the widget with
// Visual Studio-style toggle buttons that show the counts.
//
/*
 * Columns: severity (icon), code, description, project, file, line, source ("Build" or "Live").
 * Sorting is by column through SortRole (severity rank, line number, otherwise the text, case
 * ignored); without a sort column the rows keep arrival order. The counts in the toggle buttons
 * are over every row, whatever the filters show. Double-click or Enter on a jumpable row emits
 * openLocation(path, line, column). Grouping by file is not offered yet (TODO(M4)).
 *
 * GUI thread only. Adding a row is O(1) amortised; filtering and sorting are Qt's (O(n log n)).
 */
#pragma once

#include "frost/build/build_diagnostics.hpp"
#include "frost/ui/toolwindows/build_output.hpp"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QString>
#include <QWidget>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QLineEdit;
class QToolButton;
class QTreeView;

namespace frost::ui {

namespace detail {
class IconCache;
} // namespace detail

class BuildController;

// Where a row came from: the last build, or (W2) a language server's live analysis.
enum class DiagnosticSource : std::uint8_t { Build, Live };

struct Problem {
    build::BuildDiagnostic diagnostic;
    DiagnosticSource source = DiagnosticSource::Build;
};

class ProblemsModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column : int {
        SeverityColumn,
        CodeColumn,
        DescriptionColumn,
        ProjectColumn,
        FileColumn,
        LineColumn,
        SourceColumn,
        ColumnCount
    };
    enum Role : int {
        SortRole = Qt::UserRole + 1, // The value the proxy sorts by (header comment).
        SeverityRole,                // int: build::Severity.
        SourceRole,                  // int: DiagnosticSource.
        PathRole,                    // QString: the absolute path, or "".
        JumpableRole,                // bool.
    };

    explicit ProblemsModel(QObject* parent = nullptr);
    ~ProblemsModel() override;

    void add(const build::BuildDiagnostic& diagnostic, DiagnosticSource source = DiagnosticSource::Build);
    // Replaces every row of `source` with `diagnostics` (in that order, after the other sources' rows).
    void setProblems(DiagnosticSource source, const std::vector<build::BuildDiagnostic>& diagnostics);
    // Removes every row of `source`.
    void clear(DiagnosticSource source);

    [[nodiscard]] const std::vector<Problem>& problems() const noexcept { return m_problems; }
    [[nodiscard]] build::DiagnosticCounts counts() const noexcept;
    // Where row `row` jumps to; nullopt when it is not jumpable or out of range.
    [[nodiscard]] std::optional<SourceLocation> locationAt(int row) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

private:
    std::vector<Problem> m_problems;
    std::unique_ptr<detail::IconCache> m_icons;
};

// The severity toggles and the text filter over a ProblemsModel (all severities shown by default).
class ProblemsFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit ProblemsFilterModel(QObject* parent = nullptr);

    void setSeverityShown(build::Severity severity, bool shown);
    [[nodiscard]] bool isSeverityShown(build::Severity severity) const noexcept;
    // Rows whose code, description, project or file contains `text` (case ignored); "" shows all.
    void setText(const QString& text);
    [[nodiscard]] const QString& text() const noexcept { return m_text; }

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    std::array<bool, 3> m_shown{true, true, true}; // By build::Severity.
    QString m_text;
};

class ProblemsView final : public QWidget {
    Q_OBJECT

public:
    explicit ProblemsView(QWidget* parent = nullptr);
    ~ProblemsView() override;

    // Shows the build diagnostics of `controller` (not owned; nullptr detaches) and follows them.
    void setController(BuildController* controller);

    // Emits openLocation() for `index` (an index of view()->model()) when it is jumpable.
    bool activate(const QModelIndex& index);

    [[nodiscard]] ProblemsModel* model() const noexcept { return m_model; }
    [[nodiscard]] ProblemsFilterModel* filterModel() const noexcept { return m_filter; }
    [[nodiscard]] QTreeView* view() const noexcept { return m_view; }
    [[nodiscard]] QToolButton* severityButton(build::Severity severity) const noexcept;
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_search; }

signals:
    void openLocation(const QString& path, int line, int column);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateCounts();

    ProblemsModel* m_model = nullptr;
    ProblemsFilterModel* m_filter = nullptr;
    QTreeView* m_view = nullptr;
    QLineEdit* m_search = nullptr;
    std::array<QToolButton*, 3> m_buttons{}; // By build::Severity.
    BuildController* m_controller = nullptr;
    std::vector<QMetaObject::Connection> m_connections;
};

} // namespace frost::ui
