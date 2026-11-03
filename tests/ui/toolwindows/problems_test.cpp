// SPDX-License-Identifier: Apache-2.0
//
// Tests for the Problems tool window (frost/ui/toolwindows/problems.hpp): the columns, the counts in
// the severity toggles, filtering by severity and text, sorting, live diagnostics next to the
// build's, and openLocation on double-click and Enter. Offscreen; no window is shown.
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QLineEdit>
#include <QToolButton>
#include <QTreeView>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::ui {
namespace {

using build::Severity;
using test::diagnostic;
using test::FakeBuildBackend;
using test::runPostedEvents;

// Normalised absolute paths, as the build service resolves them.
#ifdef _WIN32
#define FROST_SRC(dir, file) "C:\\src\\" dir "\\" file
#else
#define FROST_SRC(dir, file) "/src/" dir "/" file
#endif

std::vector<build::BuildDiagnostic> sample() {
    return {
        diagnostic(Severity::Error, "C2065", "'x': undeclared identifier", FROST_SRC("App", "main.cpp"), 7, 27,
                   FROST_SRC("App", "App.vcxproj")),
        diagnostic(Severity::Warning, "C4100", "'argc': unreferenced parameter", FROST_SRC("App", "main.cpp"), 3, 1,
                   FROST_SRC("App", "App.vcxproj")),
        diagnostic(Severity::Warning, "C4244", "conversion from 'double' to 'int'", FROST_SRC("Lib", "lib.cpp"), 12, 9,
                   FROST_SRC("Lib", "Lib.vcxproj")),
        diagnostic(Severity::Message, "", "Lib.vcxproj -> lib.lib", {}, 0, 0, FROST_SRC("Lib", "Lib.vcxproj")),
    };
}

QString cell(const QAbstractItemModel& model, int row, int column) {
    return model.index(row, column).data().toString();
}

TEST(ProblemsModel, ShowsTheColumnsAndCounts) {
    ProblemsModel model;
    for (const build::BuildDiagnostic& d : sample()) {
        model.add(d);
    }
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.columnCount(), ProblemsModel::ColumnCount);
    EXPECT_EQ(cell(model, 0, ProblemsModel::CodeColumn), QStringLiteral("C2065"));
    EXPECT_EQ(cell(model, 0, ProblemsModel::DescriptionColumn), QStringLiteral("'x': undeclared identifier"));
    EXPECT_EQ(cell(model, 0, ProblemsModel::ProjectColumn), QStringLiteral("App"));
    EXPECT_EQ(cell(model, 0, ProblemsModel::FileColumn), QStringLiteral("main.cpp"));
    EXPECT_EQ(cell(model, 0, ProblemsModel::LineColumn), QStringLiteral("7"));
    EXPECT_EQ(cell(model, 0, ProblemsModel::SourceColumn), QStringLiteral("Build"));
    EXPECT_EQ(cell(model, 3, ProblemsModel::LineColumn), QString()); // No location.
    EXPECT_EQ(model.headerData(ProblemsModel::DescriptionColumn, Qt::Horizontal).toString(),
              QStringLiteral("Description"));

    const build::DiagnosticCounts counts = model.counts();
    EXPECT_EQ(counts.errors, 1U);
    EXPECT_EQ(counts.warnings, 2U);
    EXPECT_EQ(counts.messages, 1U);
    EXPECT_EQ(model.locationAt(0), (SourceLocation{FROST_SRC("App", "main.cpp"), 7, 27}));
    EXPECT_FALSE(model.locationAt(3)); // The message has no place.

    // Live diagnostics (W2) sit next to the build's and are cleared on their own.
    model.setProblems(DiagnosticSource::Live,
                      {diagnostic(Severity::Error, "clang", "expected ';'", FROST_SRC("Lib", "lib.cpp"), 4, 2)});
    ASSERT_EQ(model.rowCount(), 5);
    EXPECT_EQ(cell(model, 4, ProblemsModel::SourceColumn), QStringLiteral("Live"));
    model.clear(DiagnosticSource::Build);
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.counts().errors, 1U);
}

TEST(ProblemsView, FiltersBySeverityAndTextWithCountsInTheToggles) {
    ProblemsView view;
    for (const build::BuildDiagnostic& d : sample()) {
        view.model()->add(d);
    }
    EXPECT_EQ(view.severityButton(Severity::Error)->text(), QStringLiteral("1 Error"));
    EXPECT_EQ(view.severityButton(Severity::Warning)->text(), QStringLiteral("2 Warnings"));
    EXPECT_EQ(view.severityButton(Severity::Message)->text(), QStringLiteral("1 Message"));
    EXPECT_EQ(view.view()->model()->rowCount(), 4);

    view.severityButton(Severity::Warning)->click(); // Hide warnings.
    EXPECT_FALSE(view.filterModel()->isSeverityShown(Severity::Warning));
    EXPECT_EQ(view.view()->model()->rowCount(), 2);
    // The counts stay over every row.
    EXPECT_EQ(view.severityButton(Severity::Warning)->text(), QStringLiteral("2 Warnings"));
    view.severityButton(Severity::Warning)->click();
    EXPECT_EQ(view.view()->model()->rowCount(), 4);

    view.searchField()->setText(QStringLiteral("LIB.CPP")); // The file, case ignored.
    ASSERT_EQ(view.view()->model()->rowCount(), 1);
    EXPECT_EQ(cell(*view.view()->model(), 0, ProblemsModel::CodeColumn), QStringLiteral("C4244"));
    view.searchField()->setText(QStringLiteral("undeclared"));
    ASSERT_EQ(view.view()->model()->rowCount(), 1);
    EXPECT_EQ(cell(*view.view()->model(), 0, ProblemsModel::CodeColumn), QStringLiteral("C2065"));
    view.searchField()->clear();
    EXPECT_EQ(view.view()->model()->rowCount(), 4);
}

TEST(ProblemsView, SortsByColumn) {
    ProblemsView view;
    for (const build::BuildDiagnostic& d : sample()) {
        view.model()->add(d);
    }
    QAbstractItemModel& shown = *view.view()->model();
    view.view()->sortByColumn(ProblemsModel::LineColumn, Qt::AscendingOrder);
    EXPECT_EQ(cell(shown, 0, ProblemsModel::CodeColumn), QString()); // Line 0 first.
    EXPECT_EQ(cell(shown, 1, ProblemsModel::CodeColumn), QStringLiteral("C4100"));
    EXPECT_EQ(cell(shown, 3, ProblemsModel::CodeColumn), QStringLiteral("C4244"));
    view.view()->sortByColumn(ProblemsModel::SeverityColumn, Qt::AscendingOrder);
    EXPECT_EQ(shown.index(0, 0).data(ProblemsModel::SeverityRole).toInt(), static_cast<int>(Severity::Error));
    EXPECT_EQ(shown.index(3, 0).data(ProblemsModel::SeverityRole).toInt(), static_cast<int>(Severity::Message));
    view.view()->sortByColumn(ProblemsModel::FileColumn, Qt::DescendingOrder);
    EXPECT_EQ(cell(shown, 0, ProblemsModel::FileColumn), QStringLiteral("main.cpp"));
}

TEST(ProblemsView, DoubleClickAndEnterOpenTheLocation) {
    ProblemsView view;
    for (const build::BuildDiagnostic& d : sample()) {
        view.model()->add(d);
    }
    test::SignalLog jumps(&view, &ProblemsView::openLocation);
    QAbstractItemModel& shown = *view.view()->model();
    emit view.view()->doubleClicked(shown.index(2, ProblemsModel::DescriptionColumn));
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(0).toString().toStdString(), FROST_SRC("Lib", "lib.cpp"));
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 12);
    EXPECT_EQ(jumps.at(0).at(2).toInt(), 9);

    view.view()->setCurrentIndex(shown.index(0, 0));
    test::pressKey(view.view(), Qt::Key_Return);
    ASSERT_EQ(jumps.count(), 2);
    EXPECT_EQ(jumps.at(1).at(1).toInt(), 7);

    // Through the filter: the shown row, not the model row.
    view.searchField()->setText(QStringLiteral("argc"));
    emit view.view()->doubleClicked(shown.index(0, 0));
    ASSERT_EQ(jumps.count(), 3);
    EXPECT_EQ(jumps.at(2).at(1).toInt(), 3);
    // The message row has no location.
    view.searchField()->setText(QStringLiteral("lib.lib"));
    EXPECT_FALSE(view.activate(shown.index(0, 0)));
    EXPECT_EQ(jumps.count(), 3);
}

TEST(ProblemsView, F4OpensTheSelectedFilteredDiagnostic) {
    ProblemsView view;
    for (const auto& diagnostic : sample())
        view.model()->add(diagnostic);
    test::SignalLog jumps(&view, &ProblemsView::openLocation);
    view.searchField()->setText(QStringLiteral("argc"));
    auto* shown = view.view()->model();
    ASSERT_EQ(shown->rowCount(), 1);
    view.view()->setCurrentIndex(shown->index(0, ProblemsModel::DescriptionColumn));
    test::pressKey(view.view(), Qt::Key_F4, {}, Qt::ControlModifier);
    EXPECT_EQ(jumps.count(), 0);
    test::pressKey(view.view(), Qt::Key_F4);
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 3);
    view.searchField()->setText(QStringLiteral("lib.lib"));
    view.view()->setCurrentIndex(shown->index(0, 0));
    test::pressKey(view.view(), Qt::Key_F4);
    EXPECT_EQ(jumps.count(), 1); // A linker diagnostic has no source location.
}

TEST(ProblemsView, FollowsTheControllersBuildDiagnostics) {
    FakeBuildBackend backend;
    backend.diagnosticList = {sample()[0]};
    BuildController controller;
    controller.setBackend(&backend);
    ProblemsView view;
    view.setController(&controller);
    EXPECT_EQ(view.model()->rowCount(), 1); // The diagnostics so far.

    const build::BuildServiceCallbacks callbacks = controller.serviceCallbacks();
    callbacks.onDiagnostic(sample()[1]);
    callbacks.onDiagnostic(sample()[2]);
    runPostedEvents();
    EXPECT_EQ(view.model()->rowCount(), 3);
    EXPECT_EQ(view.severityButton(Severity::Warning)->text(), QStringLiteral("2 Warnings"));
    callbacks.onDiagnosticsCleared();
    runPostedEvents();
    EXPECT_EQ(view.model()->rowCount(), 0);
    EXPECT_EQ(view.severityButton(Severity::Error)->text(), QStringLiteral("0 Errors"));
}

} // namespace
} // namespace frost::ui
