// SPDX-License-Identifier: Apache-2.0
// Engine errors retain their own source and exact positions alongside build and language results.
#include "frost/ui/toolwindows/problems.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QToolButton>
#include <QTreeView>
#include <gtest/gtest.h>

namespace frost::ui {
namespace {
TEST(EngineProblems, KeepsReportedCoordinatesAndClearsOnlyTheEngineSource) {
    ProblemsView view;
#ifdef _WIN32
    const std::string path = "C:\\project\\Assets\\Player.lua";
#else
    const std::string path = "/project/Assets/Player.lua";
#endif
    view.model()->add(test::diagnostic(build::Severity::Error, "lua", "runtime callback failed", path, 47, 3),
                      DiagnosticSource::Engine);
    view.model()->add(test::diagnostic(build::Severity::Warning, "unused", "unused local", path, 4, 8),
                      DiagnosticSource::Live);
    view.model()->add(test::diagnostic(build::Severity::Error, "build", "compile failed", path, 9, 1));
    EXPECT_EQ(view.model()->index(0, ProblemsModel::SourceColumn).data().toString(), QStringLiteral("Engine"));
    EXPECT_EQ(view.model()->locationAt(0), (SourceLocation{path, 47, 3}));
    view.filterModel()->setSourceFilter(DiagnosticSource::Engine);
    ASSERT_EQ(view.view()->model()->rowCount(), 1);
    test::SignalLog jumps(&view, &ProblemsView::openLocation);
    EXPECT_TRUE(view.activate(view.view()->model()->index(0, 0)));
    ASSERT_EQ(jumps.count(), 1);
    EXPECT_EQ(jumps.at(0).at(0).toString().toStdString(), path);
    EXPECT_EQ(jumps.at(0).at(1).toInt(), 47);
    EXPECT_EQ(jumps.at(0).at(2).toInt(), 3);
    view.model()->clear(DiagnosticSource::Build);
    EXPECT_EQ(view.model()->rowCount(), 2);
    EXPECT_EQ(view.view()->model()->rowCount(), 1);
    view.model()->clear(DiagnosticSource::Engine);
    EXPECT_EQ(view.model()->rowCount(), 1);
    EXPECT_EQ(view.model()->problems().front().source, DiagnosticSource::Live);
}

TEST(EngineProblems, InspectionAndEngineScopesRemainDistinct) {
    ProblemsView view;
    view.model()->add(test::diagnostic(build::Severity::Warning, "inspection", "actual inspection message", "", 0, 0),
                      DiagnosticSource::Inspection);
    view.model()->add(test::diagnostic(build::Severity::Error, "engine", "actual engine message", "", 0, 0),
                      DiagnosticSource::Engine);
    auto* inspection = view.findChild<QToolButton*>(QStringLiteral("problemsScopeInspection"));
    auto* engine = view.findChild<QToolButton*>(QStringLiteral("problemsScopeEngine"));
    ASSERT_NE(inspection, nullptr);
    ASSERT_NE(engine, nullptr);
    inspection->click();
    ASSERT_EQ(view.view()->model()->rowCount(), 1);
    EXPECT_EQ(view.view()->model()->index(0, ProblemsModel::SourceColumn).data().toString(),
              QStringLiteral("Inspection"));
    engine->click();
    ASSERT_EQ(view.view()->model()->rowCount(), 1);
    EXPECT_EQ(view.view()->model()->index(0, ProblemsModel::SourceColumn).data().toString(), QStringLiteral("Engine"));
    view.model()->clear(DiagnosticSource::Inspection);
    ASSERT_EQ(view.model()->rowCount(), 1);
    EXPECT_EQ(view.model()->problems().front().source, DiagnosticSource::Engine);
    EXPECT_EQ(view.view()->model()->rowCount(), 1);
    view.model()->clear(DiagnosticSource::Engine);
    EXPECT_EQ(view.model()->rowCount(), 0);
}

TEST(EngineProblems, DoesNotInventPositionsForAnUnlocatedEngineError) {
    ProblemsModel model;
    model.add(test::diagnostic(build::Severity::Error, "engine", "request failed", "", 0, 0), DiagnosticSource::Engine);
    EXPECT_FALSE(model.locationAt(0));
    EXPECT_FALSE(model.index(0, 0).data(ProblemsModel::JumpableRole).toBool());
    EXPECT_EQ(model.index(0, ProblemsModel::LineColumn).data().toString(), QString());
}
} // namespace
} // namespace frost::ui
