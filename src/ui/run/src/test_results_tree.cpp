// SPDX-License-Identifier: Apache-2.0
//
// TestResultsTree (test_results_tree.hpp): the Unit Tests window's results tree.

#include "frost/ui/test_results_tree.hpp"

#include "frost/run/gtest_parsers.hpp"

#include <QHeaderView>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <set>
#include <utility>

namespace frost::ui {

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

enum Column : int { NameColumn = 0, StatusColumn = 1, DurationColumn = 2 };

// The group a test is listed under: its program's file name, or a note that ctest found none.
QString programGroup(const run::DiscoveredTest& test) {
    if (test.command.empty())
        return QStringLiteral("(no program found)");
    return qs(run::gtestProgramStem(test.command.front()));
}

QString unlistedGroup() { return QStringLiteral("(not in the last test list)"); }

QString statusText(const run::TestResult& result) {
    QString text = qs(run::toString(result.outcome));
    if (!result.reason.empty() && result.reason != run::toString(result.outcome) &&
        result.outcome != run::TestOutcome::Passed)
        text += QStringLiteral(" (%1)").arg(qs(result.reason).section(QLatin1Char('\n'), 0, 0));
    return text;
}

} // namespace

TestResultsTree::TestResultsTree(QWidget* parent) : QTreeWidget(parent) {
    setObjectName(QStringLiteral("unitTestsTreeWidget"));
    setColumnCount(3);
    setHeaderLabels({QStringLiteral("Test"), QStringLiteral("Status"), QStringLiteral("Duration")});
    header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(StatusColumn, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(DurationColumn, QHeaderView::ResizeToContents);
    header()->setStretchLastSection(false);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformRowHeights(true);
    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (!item)
            return;
        const QString test = item->data(0, kTestNameRole).toString();
        if (!test.isEmpty())
            emit testActivated(test, item->data(0, kCaseNameRole).toString());
    });
}

TestResultsTree::~TestResultsTree() = default;

void TestResultsTree::setTests(std::vector<run::DiscoveredTest> tests) {
    m_tests = std::move(tests);
    m_unlisted.clear();
    m_results.clear();
    m_listedCases.clear();
    m_frameworkCases.clear();
    m_doctestCases.clear();
    m_state.clear();
    rebuild();
}

QTreeWidgetItem* TestResultsTree::groupFor(const QString& program) {
    if (const auto found = m_groups.find(program); found != m_groups.end())
        return found->second;
    auto* group = new QTreeWidgetItem(this, {program});
    m_groups.emplace(program, group);
    return group;
}

QTreeWidgetItem* TestResultsTree::addNameItem(const std::string& name, const QString& group) {
    auto* item = new QTreeWidgetItem(groupFor(group), {qs(name)});
    item->setData(0, kTestNameRole, qs(name));
    m_items[name] = item;
    return item;
}

QTreeWidgetItem* TestResultsTree::addTestItem(const run::DiscoveredTest& test) {
    QTreeWidgetItem* item = addNameItem(test.name, programGroup(test));
    QStringList tip;
    if (!test.command.empty()) {
        QStringList command;
        for (const auto& argument : test.command)
            command << qs(argument);
        tip << command.join(QLatin1Char(' '));
    }
    if (test.disabled)
        tip << QStringLiteral("DISABLED");
    if (!test.labels.empty()) {
        QStringList labels;
        for (const auto& label : test.labels)
            labels << qs(label);
        tip << QStringLiteral("Labels: %1").arg(labels.join(QStringLiteral(", ")));
    }
    item->setToolTip(NameColumn, tip.join(QLatin1Char('\n')));
    return item;
}

void TestResultsTree::rebuild() {
    clear();
    m_items.clear();
    m_groups.clear();
    const QString filter = m_filter;
    const auto matches = [&filter](const run::DiscoveredTest& test, const run::TestResult* result,
                                   const std::vector<run::GTestCase>* listed,
                                   const std::vector<run::DoctestCase>* doctest, const std::vector<run::FrameworkCase>* framework) {
        if (filter.isEmpty())
            return true;
        if (qs(test.name).contains(filter, Qt::CaseInsensitive) ||
            programGroup(test).contains(filter, Qt::CaseInsensitive))
            return true;
        if (result && std::ranges::any_of(result->cases, [&](const run::TestResult& c) {
                return qs(c.name).contains(filter, Qt::CaseInsensitive);
            }))
            return true;
        if (framework && std::ranges::any_of(*framework, [&](const auto& c) { return qs(c.name).contains(filter, Qt::CaseInsensitive); })) return true;
        if (doctest && std::ranges::any_of(*doctest, [&](const run::DoctestCase& c) {
                return qs(c.name).contains(filter, Qt::CaseInsensitive);
            }))
            return true;
        return listed && std::ranges::any_of(*listed, [&](const run::GTestCase& c) {
                   return qs(c.fullName()).contains(filter, Qt::CaseInsensitive);
               });
    };
    for (const run::DiscoveredTest& test : m_tests) {
        const auto result = m_results.find(test.name);
        const auto listed = m_listedCases.find(test.name);
        const auto doctest = m_doctestCases.find(test.name);
        const auto framework = m_frameworkCases.find(test.name);
        if (!matches(test, result == m_results.end() ? nullptr : &result->second,
                     listed == m_listedCases.end() ? nullptr : &listed->second,
                     doctest == m_doctestCases.end() ? nullptr : &doctest->second,
                     framework == m_frameworkCases.end() ? nullptr : &framework->second))
            continue;
        QTreeWidgetItem* item = addTestItem(test);
        if (doctest != m_doctestCases.end()) {
            std::set<std::string> shown; // doctest allows one name in several files; one row per name.
            for (const run::DoctestCase& testCase : doctest->second) {
                if (!shown.insert(testCase.name).second)
                    continue;
                auto* child = new QTreeWidgetItem(item, {qs(testCase.name)});
                child->setData(0, kTestNameRole, qs(test.name));
                child->setData(0, kCaseNameRole, qs(testCase.name));
                QStringList tip;
                if (!testCase.suite.empty())
                    tip << QStringLiteral("Test suite: %1").arg(qs(testCase.suite));
                if (testCase.location)
                    tip << QStringLiteral("%1:%2").arg(qs(testCase.location->file)).arg(testCase.location->line);
                child->setToolTip(NameColumn, tip.join(QLatin1Char('\n')));
            }
        }
        if (framework != m_frameworkCases.end()) for (const auto& entry : framework->second) {
            auto* child = new QTreeWidgetItem(item, {qs(entry.name)});
            child->setData(0, kTestNameRole, qs(test.name));
            child->setData(0, kCaseNameRole, qs(entry.name));
            QString tip;
            if (entry.location) tip = QStringLiteral("%1:%2").arg(qs(entry.location->file)).arg(entry.location->line);
            if (entry.disabled) tip += QStringLiteral(" (disabled by default)");
            child->setToolTip(NameColumn, tip);
        }
        if (listed != m_listedCases.end()) {
            for (const run::GTestCase& testCase : listed->second) {
                auto* child = new QTreeWidgetItem(item, {qs(testCase.fullName())});
                child->setData(0, kTestNameRole, qs(test.name));
                child->setData(0, kCaseNameRole, qs(testCase.fullName()));
                if (!testCase.typeParam.empty() || !testCase.valueParam.empty())
                    child->setToolTip(NameColumn,
                                      qs(testCase.typeParam.empty() ? testCase.valueParam : testCase.typeParam));
            }
        }
        if (result != m_results.end())
            showResult(item, result->second);
        else if (const auto state = m_state.find(test.name); state != m_state.end())
            item->setText(StatusColumn, state->second);
    }
    for (const std::string& name : m_unlisted) {
        if (!filter.isEmpty() && !qs(name).contains(filter, Qt::CaseInsensitive))
            continue;
        QTreeWidgetItem* item = addNameItem(name, unlistedGroup());
        if (const auto result = m_results.find(name); result != m_results.end())
            showResult(item, result->second);
    }
    expandAll();
}

void TestResultsTree::showCases(QTreeWidgetItem* item, const run::TestResult& result) {
    const QString test = item->data(0, kTestNameRole).toString();
    for (const run::TestResult& testCase : result.cases) {
        QTreeWidgetItem* child = nullptr;
        for (int i = 0; i < item->childCount() && !child; ++i)
            if (item->child(i)->data(0, kCaseNameRole).toString() == qs(testCase.name))
                child = item->child(i);
        if (!child) {
            child = new QTreeWidgetItem(item, {qs(testCase.name)});
            child->setData(0, kTestNameRole, test);
            child->setData(0, kCaseNameRole, qs(testCase.name));
        }
        child->setText(StatusColumn, statusText(testCase));
        child->setText(DurationColumn, qs(run::formatTestDuration(testCase.duration)));
    }
    if (!result.cases.empty())
        item->setExpanded(true);
}

void TestResultsTree::showResult(QTreeWidgetItem* item, const run::TestResult& result) {
    item->setText(StatusColumn, statusText(result));
    item->setText(DurationColumn, qs(run::formatTestDuration(result.duration)));
    showCases(item, result);
}

void TestResultsTree::setFrameworkCases(const std::string& test, std::vector<run::FrameworkCase> cases) {
    m_listedCases.erase(test); m_doctestCases.erase(test);
    m_frameworkCases[test] = std::move(cases);
    rebuild();
}
const std::vector<run::FrameworkCase>* TestResultsTree::frameworkCases(const std::string& test) const {
    const auto found = m_frameworkCases.find(test);
    return found == m_frameworkCases.end() ? nullptr : &found->second;
}
void TestResultsTree::setGTestCases(const std::string& test, const std::vector<run::GTestCase>& cases) {
    m_frameworkCases.erase(test);
    m_listedCases[test] = cases;
    rebuild();
}

void TestResultsTree::setDoctestCases(std::vector<std::pair<std::string, std::vector<run::DoctestCase>>> listings) {
    for (auto& [test, cases] : listings) {
        m_frameworkCases.erase(test);
        m_doctestCases[test] = std::move(cases);
    }
    rebuild();
}

const std::vector<run::DoctestCase>* TestResultsTree::doctestCases(const std::string& test) const {
    const auto found = m_doctestCases.find(test);
    return found == m_doctestCases.end() ? nullptr : &found->second;
}

void TestResultsTree::markPending(const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        m_state[name] = QStringLiteral("Pending");
        // A pending rerun invalidates the previous verdict. If this run stops or fails before it
        // produces a new result, showing that old verdict would make it look current.
        m_results.erase(name);
        if (auto* item = itemFor(name)) {
            item->setText(StatusColumn, m_state[name]);
            item->setText(DurationColumn, QString());
            for (int i = 0; i < item->childCount(); ++i) {
                item->child(i)->setText(StatusColumn, QString());
                item->child(i)->setText(DurationColumn, QString());
            }
        }
    }
}

void TestResultsTree::testStarted(const std::string& name) {
    m_state[name] = QStringLiteral("Running");
    if (auto* item = itemFor(name))
        item->setText(StatusColumn, m_state[name]);
}

void TestResultsTree::applyResult(const run::TestResult& result) {
    m_state.erase(result.name);
    m_results.insert_or_assign(result.name, result);
    QTreeWidgetItem* item = itemFor(result.name);
    if (!item && !test(result.name) && !std::ranges::contains(m_unlisted, result.name)) {
        // ctest's report names a test the last discovery did not list.
        m_unlisted.push_back(result.name);
        if (m_filter.isEmpty() || qs(result.name).contains(m_filter, Qt::CaseInsensitive))
            item = addNameItem(result.name, unlistedGroup());
    }
    if (item)
        showResult(item, result);
}

void TestResultsTree::finishRun(const run::TestRunReport& report) {
    for (const run::TestResult& result : report.results)
        applyResult(result);
    // Tests that were pending or running and got no result keep the result they had before, or none.
    for (const auto& [name, state] : m_state) {
        if (auto* item = itemFor(name)) {
            const auto result = m_results.find(name);
            if (result != m_results.end()) {
                showResult(item, result->second);
            } else {
                item->setText(StatusColumn, QString());
                item->setText(DurationColumn, QString());
            }
        }
    }
    m_state.clear();
}

void TestResultsTree::setFilter(const QString& text) {
    m_filter = text.trimmed();
    rebuild();
}

std::vector<std::string> TestResultsTree::testNames() const {
    std::vector<std::string> out;
    for (const auto& test : m_tests)
        out.push_back(test.name);
    out.insert(out.end(), m_unlisted.begin(), m_unlisted.end());
    return out;
}

std::vector<std::string> TestResultsTree::selectedTestNames() const {
    std::set<std::string> chosen;
    for (const QTreeWidgetItem* item : selectedItems()) {
        const QString test = item->data(0, kTestNameRole).toString();
        if (!test.isEmpty()) {
            chosen.insert(test.toStdString());
            continue;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            const QString child = item->child(i)->data(0, kTestNameRole).toString();
            if (!child.isEmpty())
                chosen.insert(child.toStdString());
        }
    }
    std::vector<std::string> out;
    for (const auto& name : testNames())
        if (chosen.contains(name))
            out.push_back(name);
    return out;
}

std::vector<std::pair<std::string, std::string>> TestResultsTree::selectedCases() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (QTreeWidgetItemIterator it(const_cast<TestResultsTree*>(this)); *it; ++it) {
        const QTreeWidgetItem* item = *it;
        const QString caseName = item->data(0, kCaseNameRole).toString();
        if (item->isSelected() && !caseName.isEmpty())
            out.emplace_back(item->data(0, kTestNameRole).toString().toStdString(), caseName.toStdString());
    }
    return out;
}

std::vector<std::string> TestResultsTree::failedTestNames() const {
    std::vector<std::string> out;
    for (const auto& name : testNames()) {
        const auto result = m_results.find(name);
        if (result != m_results.end() && run::countsAsFailure(result->second.outcome))
            out.push_back(name);
    }
    return out;
}

const run::DiscoveredTest* TestResultsTree::test(const std::string& name) const {
    const auto found = std::ranges::find(m_tests, name, &run::DiscoveredTest::name);
    return found == m_tests.end() ? nullptr : &*found;
}

const run::TestResult* TestResultsTree::result(const std::string& name) const {
    const auto found = m_results.find(name);
    return found == m_results.end() ? nullptr : &found->second;
}

QTreeWidgetItem* TestResultsTree::itemFor(const std::string& name, const std::string& caseName) const {
    const auto found = m_items.find(name);
    if (found == m_items.end())
        return nullptr;
    if (caseName.empty())
        return found->second;
    for (int i = 0; i < found->second->childCount(); ++i)
        if (found->second->child(i)->data(0, kCaseNameRole).toString() == qs(caseName))
            return found->second->child(i);
    return nullptr;
}

TestResultsTree::Counts TestResultsTree::counts() const {
    Counts out;
    const std::vector<std::string> names = testNames();
    out.tests = names.size();
    for (const auto& name : names) {
        const auto result = m_results.find(name);
        if (result == m_results.end()) {
            ++out.withoutResult;
            continue;
        }
        switch (result->second.outcome) {
        case run::TestOutcome::Passed: ++out.passed; break;
        case run::TestOutcome::Failed:
        case run::TestOutcome::NotRun: ++out.failed; break;
        case run::TestOutcome::Skipped: ++out.skipped; break;
        case run::TestOutcome::Disabled: ++out.disabled; break;
        }
    }
    return out;
}

} // namespace frost::ui
