// SPDX-License-Identifier: Apache-2.0
//
// TestResultsTree (RUN-7, PLAN.md Appendix C R2): the Unit Tests window's minimal results tree. It
// lists the tests ctest discovered, grouped by their program, and shows for each the outcome and
// duration the test runner parsed from ctest's, Google Test's and doctest's own output (frost/run/
// test_model.hpp), with a Google Test program's or a doctest program's cases below it (RUN-8). It only displays: it
// never decides an outcome. A test without a result shows an empty status (nothing ran, so nothing is claimed);
// "Pending" and "Running" are the window's own state while a run goes on, not results.
//
/*
 * Columns: the name, the status (run::toString of the outcome, "Pending", "Running" or ""), the
 * duration (run::formatTestDuration). Items carry the test name (kTestNameRole) and, for a case,
 * its full name (kCaseNameRole); groups carry neither.
 *
 * Results. applyResult takes an interim result (a progress line) or a final one; finishRun applies
 * the report's results, which replace the interim ones, and clears Pending/Running from tests that
 * got none (a stopped run). A result for a test that is not listed adds its name under "(not in
 * the last test list)" (ctest's report names a test the last discovery did not see). setTests
 * drops every result.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/run/doctest_parsers.hpp"
#include "frost/run/test_model.hpp"
#include "frost/run/test_runner.hpp"

#include <QString>
#include <QTreeWidget>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui {

class TestResultsTree final : public QTreeWidget {
    Q_OBJECT

public:
    static constexpr int kTestNameRole = Qt::UserRole + 1;
    static constexpr int kCaseNameRole = Qt::UserRole + 2;

    explicit TestResultsTree(QWidget* parent = nullptr);
    ~TestResultsTree() override;
    TestResultsTree(const TestResultsTree&) = delete ("a QWidget");
    TestResultsTree& operator=(const TestResultsTree&) = delete ("a QWidget");

    // Lists `tests` (discovery order within each program group), without results.
    void setTests(std::vector<run::DiscoveredTest> tests);
    // Lists the cases `--gtest_list_tests` gave for `test` below it (without results).
    void setGTestCases(const std::string& test, const std::vector<run::GTestCase>& cases);
    /*
     * Lists the cases doctest's --list-test-cases gave for each test of `listings` (test name ->
     * cases) below it, without results; a later result's cases are shown on these rows (by name).
     * Rebuilds the tree once for all of them: O(rows). FrostEngine lists ~650 suites with ~8500 cases.
     */
    void setDoctestCases(std::vector<std::pair<std::string, std::vector<run::DoctestCase>>> listings);
    // The doctest cases listed for `test`, or null when none were.
    [[nodiscard]] const std::vector<run::DoctestCase>* doctestCases(const std::string& test) const;
    // Marks `names` Pending: a run of them was started.
    void markPending(const std::vector<std::string>& names);
    // ctest started `name`.
    void testStarted(const std::string& name);
    // Shows `result` (interim or final) for its test, and its Google Test cases below it.
    void applyResult(const run::TestResult& result);
    // The run ended: its results replace the interim ones (header comment).
    void finishRun(const run::TestRunReport& report);
    // Shows only tests whose name, program or case names contain `text` (ASCII case ignored).
    void setFilter(const QString& text);

    // Every listed test, in the tree's order.
    [[nodiscard]] std::vector<std::string> testNames() const;
    // The tests under the selected items (a group stands for its tests, a case for its test).
    [[nodiscard]] std::vector<std::string> selectedTestNames() const;
    // The selected case rows as (test, case) pairs, in the tree's order; test and group rows are not
    // in it.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> selectedCases() const;
    // The tests whose last result counts as a failure (run::countsAsFailure).
    [[nodiscard]] std::vector<std::string> failedTestNames() const;
    // The discovered test, or null.
    [[nodiscard]] const run::DiscoveredTest* test(const std::string& name) const;
    // The test's last result, or null when it has none.
    [[nodiscard]] const run::TestResult* result(const std::string& name) const;
    // The item of a test (or of one of its cases), or null; for tests.
    [[nodiscard]] QTreeWidgetItem* itemFor(const std::string& name, const std::string& caseName = {}) const;

    // The number of tests with a result of each outcome, and without one.
    struct Counts {
        std::size_t tests = 0;
        std::size_t passed = 0;
        std::size_t failed = 0; // Failed and NotRun (countsAsFailure).
        std::size_t skipped = 0;
        std::size_t disabled = 0;
        std::size_t withoutResult = 0;
    };
    [[nodiscard]] Counts counts() const;

signals:
    // A test or case was double-clicked or Enter was pressed on it.
    void testActivated(const QString& test, const QString& caseName);

private:
    void rebuild();
    void showResult(QTreeWidgetItem* item, const run::TestResult& result);
    void showCases(QTreeWidgetItem* item, const run::TestResult& result);
    QTreeWidgetItem* addTestItem(const run::DiscoveredTest& test);
    QTreeWidgetItem* addNameItem(const std::string& name, const QString& group);
    QTreeWidgetItem* groupFor(const QString& program);

    std::vector<run::DiscoveredTest> m_tests;
    std::vector<std::string> m_unlisted; // Named by a report, not by the last discovery.
    std::map<std::string, run::TestResult> m_results;
    std::map<std::string, std::vector<run::GTestCase>> m_listedCases;
    std::map<std::string, std::vector<run::DoctestCase>> m_doctestCases;
    std::map<std::string, QString> m_state; // "Pending" / "Running" while a run goes on.
    std::map<std::string, QTreeWidgetItem*> m_items;
    std::map<QString, QTreeWidgetItem*> m_groups;
    QString m_filter;
};

} // namespace frost::ui
