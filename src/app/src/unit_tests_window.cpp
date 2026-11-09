// SPDX-License-Identifier: Apache-2.0
//
// The Unit Tests tool window (unit_tests_window.hpp): the open CMake folder's ctest setup, a
// run::TestRunner and a ui::TestResultsTree. Every test, case, outcome, duration, message and
// location it shows came from ctest's, Google Test's or doctest's output through frost::run's parsers.

#include "unit_tests_window.hpp"

#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/test_runner.hpp"
#include "frost/ui/test_results_tree.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <format>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace frost::app {

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// The most console output of one ctest run the window keeps (the tail).
constexpr qsizetype kMaxConsole = 1 << 20;

constexpr int kLocationPathRole = Qt::UserRole + 1;
constexpr int kLocationLineRole = Qt::UserRole + 2;

QString debugUnavailable() { return QStringLiteral("Debugging a single test comes with RUN-10."); }

// Where the window's tests come from, or why it has none (unit_tests_window.hpp, "Where the tests come from").
struct Source {
    std::optional<run::CTestSetup> setup{};
    QString unavailable{};
};

class UnitTestsWindow final : public QWidget {
public:
    UnitTestsWindow(QWidget* parent, UnitTestsWindowContext context)
        : QWidget(parent), m_context(std::move(context)),
          m_runner(run::TestRunner::create(*m_context.launcher, *m_context.fs)) {
        setObjectName(QStringLiteral("unitTestsToolWindow"));
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(6, 6, 6, 6);
        outer->setSpacing(6);

        auto* toolbar = new QHBoxLayout();
        const auto button = [this, toolbar](const QString& label, const char* name, const QString& tip) {
            auto* b = new QPushButton(label, this);
            b->setObjectName(QLatin1String(name));
            b->setToolTip(tip);
            toolbar->addWidget(b);
            return b;
        };
        m_runAll =
            button(QStringLiteral("Run All"), "unitTestsRunAllButton", QStringLiteral("Run every test with ctest."));
        m_runSelected = button(QStringLiteral("Run Selected"), "unitTestsRunSelectedButton",
                               QStringLiteral("Run the selected tests with ctest."));
        m_runFailed = button(QStringLiteral("Run Failed"), "unitTestsRunFailedButton",
                             QStringLiteral("Run again the tests that failed or did not run."));
        m_stop = button(QStringLiteral("Stop"), "unitTestsStopButton",
                        QStringLiteral("Stop ctest and every test process it started."));
        m_refresh = button(QStringLiteral("Refresh"), "unitTestsRefreshButton",
                           QStringLiteral("List the tests again (ctest -N)."));
        m_debug = button(QStringLiteral("Debug Selected Test"), "unitTestsDebugSelectedButton", debugUnavailable());
        m_debug->setEnabled(false);
        m_filter = new QLineEdit(this);
        m_filter->setObjectName(QStringLiteral("unitTestsFilterEdit"));
        m_filter->setPlaceholderText(QStringLiteral("Filter tests..."));
        toolbar->addWidget(m_filter, 1);
        outer->addLayout(toolbar);

        m_summary = new QLabel(this);
        m_summary->setObjectName(QStringLiteral("unitTestsSummaryLabel"));
        m_summary->setWordWrap(true);
        m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        outer->addWidget(m_summary);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        m_tree = new ui::TestResultsTree(splitter);
        m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
        splitter->addWidget(m_tree);
        auto* right = new QWidget(splitter);
        auto* rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(4);
        m_details = new QPlainTextEdit(right);
        m_details->setObjectName(QStringLiteral("unitTestsOutputView"));
        m_details->setReadOnly(true);
        m_details->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_details->setPlaceholderText(QStringLiteral("Select a test to see its command, result and output."));
        rightLayout->addWidget(m_details, 1);
        rightLayout->addWidget(new QLabel(QStringLiteral("Locations (click to open):"), right));
        m_locations = new QListWidget(right);
        m_locations->setObjectName(QStringLiteral("unitTestsLocationList"));
        m_locations->setMaximumHeight(110);
        rightLayout->addWidget(m_locations);
        splitter->addWidget(right);
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);
        outer->addWidget(splitter, 1);

        connect(m_runAll, &QPushButton::clicked, this, [this] { requestRun({}); });
        connect(m_runSelected, &QPushButton::clicked, this, [this] { runSelection(); });
        connect(m_runFailed, &QPushButton::clicked, this, [this] {
            if (auto names = m_tree->failedTestNames(); !names.empty())
                requestRun(std::move(names));
        });
        connect(m_stop, &QPushButton::clicked, this, [this] { m_runner->cancel(); });
        connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
        connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
            m_tree->setFilter(text);
            updateActions();
        });
        connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
            showDetails();
            updateActions();
        });
        connect(
            m_tree, &ui::TestResultsTree::testActivated, this,
            [this](const QString& test, const QString& caseName) { open(test.toStdString(), caseName.toStdString()); });
        connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) { contextMenu(at); });
        connect(m_locations, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            if (item && m_context.openLocation)
                m_context.openLocation(item->data(kLocationPathRole).toString().toStdString(),
                                       item->data(kLocationLineRole).toInt());
        });

        // A new reply, profile or trust state lists the tests again (header comment, "Refresh").
        m_watch = new QTimer(this);
        m_watch->setInterval(1000);
        connect(m_watch, &QTimer::timeout, this, [this] {
            if (isVisible() && !m_runner->busy() && sourceKey() != m_listedKey)
                refresh();
        });
        m_watch->start();
        refresh();
    }

    ~UnitTestsWindow() override {
        // Stops the job and joins it while the posted callbacks' receiver (this) still exists.
        m_runner.reset();
    }

    UnitTestsWindow(const UnitTestsWindow&) = delete ("a QWidget");
    UnitTestsWindow& operator=(const UnitTestsWindow&) = delete ("a QWidget");

private:
    build::CMakeService* cmake() const { return m_context.cmake ? m_context.cmake() : nullptr; }

    // Changes whenever the source would give another setup (cheap: a model snapshot).
    std::string sourceKey() const {
        build::CMakeService* service = cmake();
        if (!service)
            return m_context.solutionOpen && m_context.solutionOpen() ? "solution" : "none";
        const auto model = service->model();
        const auto profile = service->activeProfile();
        const bool trusted = m_context.trust && m_context.trust->isTrusted(service->sourceDirectory());
        return std::format("cmake|{}|{}|{}|{}", model ? model->replyIndex : std::string(),
                           profile ? profile->name : std::string(), trusted, service->profileGeneration());
    }

    Source source() const {
        Source out;
        build::CMakeService* service = cmake();
        if (!service) {
            out.unavailable =
                m_context.solutionOpen && m_context.solutionOpen()
                    ? QStringLiteral("Tests are listed by ctest, from a configured CMake build directory. This "
                                     "Visual Studio solution has none, so no test is listed.")
                    : QStringLiteral("Open a CMake folder: the Unit Tests window lists the tests ctest finds in "
                                     "its configured build directory.");
            return out;
        }
        const auto model = service->model();
        const auto profile = service->activeProfile();
        if (!model || !profile) {
            out.unavailable = QStringLiteral("The CMake folder is not configured yet. Its tests are listed once a "
                                             "configure has written its build directory.");
            return out;
        }
        if (!m_context.trust || !m_context.trust->isTrusted(service->sourceDirectory())) {
            out.unavailable = QStringLiteral("Trust this folder to list and run its tests: ctest runs the "
                                             "project's test scripts and test programs.");
            return out;
        }
        const project::CMakeTestPreset* preset = nullptr;
        auto presets = project::loadCMakePresets(*m_context.fs, service->sourceDirectory());
        if (presets && presets->has_value())
            preset = run::testPresetFor(**presets, *profile);
        auto setup = run::ctestSetupFor(*model, *profile, preset);
        if (!setup) {
            out.unavailable = QStringLiteral("ctest is unavailable: %1").arg(qs(setup.error().message()));
            return out;
        }
        out.setup = std::move(*setup);
        return out;
    }

    // Posts `work` to the GUI thread (the runner calls back on its own threads).
    template<typename F>
    void post(F work) {
        QMetaObject::invokeMethod(this, std::move(work), Qt::QueuedConnection);
    }

    void setSummary(const QString& text) { m_summary->setText(text); }

    void refresh(std::optional<std::vector<std::string>> thenRun = std::nullopt) {
        if (m_runner->busy())
            return;
        m_listedKey = sourceKey();
        Source current = source();
        m_setup = current.setup;
        if (!m_setup) {
            m_tree->setTests({});
            setSummary(current.unavailable);
            m_unavailable = current.unavailable;
            showDetails();
            updateActions();
            return;
        }
        m_unavailable.clear();
        setSummary(QStringLiteral("Listing the tests (ctest -N in %1)...").arg(qs(m_setup->buildDirectory)));
        const run::CTestSetup setup = *m_setup;
        auto started = m_runner->discover(setup, [this, thenRun](Result<std::vector<run::DiscoveredTest>> tests) {
            post([this, thenRun, tests = std::move(tests)]() mutable { listed(std::move(tests), thenRun); });
        });
        if (!started)
            setSummary(QStringLiteral("ctest could not list the tests: %1").arg(qs(started.error().message())));
        updateActions();
    }

    void listed(Result<std::vector<run::DiscoveredTest>> tests, std::optional<std::vector<std::string>> thenRun) {
        if (!tests) {
            m_tree->setTests({});
            const QString message = qs(tests.error().message());
            setSummary(
                QStringLiteral("ctest could not list the tests: %1").arg(message.section(QLatin1Char('\n'), 0, 0)));
            m_details->setPlainText(message);
            updateActions();
            return;
        }
        const std::size_t count = tests->size();
        m_tree->setTests(std::move(*tests));
        m_tree->setFilter(m_filter->text());
        QString text = QStringLiteral("%1 test(s) listed by ctest in %2").arg(count).arg(qs(m_setup->buildDirectory));
        if (!m_setup->testPreset.empty())
            text += QStringLiteral(" (test preset %1)").arg(qs(m_setup->testPreset));
        setSummary(text);
        m_listedSummary = text;
        showDetails();
        updateActions();
        listDoctest(m_tree->testNames(), std::move(thenRun));
    }

    // Lists the doctest cases of `names`' doctest programs (header comment, "doctest"), then runs
    // `thenRun` when given.
    void listDoctest(const std::vector<std::string>& names, std::optional<std::vector<std::string>> thenRun) {
        std::vector<run::DiscoveredTest> doctest;
        for (const std::string& name : names)
            if (const run::DiscoveredTest* test = m_tree->test(name);
                test && !test->command.empty() && io::isAbsolutePath(test->command.front()) &&
                (run::isDoctestCommand(test->command) || m_explicitDoctest.contains(name)))
                doctest.push_back(*test);
        if (doctest.empty() || !m_setup) {
            if (thenRun)
                startRun(std::move(*thenRun));
            return;
        }
        setSummary(m_listedSummary +
                   QStringLiteral("; listing the doctest cases of %1 test(s)...").arg(doctest.size()));
        auto started = m_runner->listDoctestCases(
            *m_setup, std::move(doctest), [this, thenRun](std::vector<run::TestRunner::DoctestListing> listings) {
                post([this, thenRun, listings = std::move(listings)]() mutable {
                    doctestListed(std::move(listings), std::move(thenRun));
                });
            });
        if (!started)
            setSummary(m_listedSummary +
                       QStringLiteral("; the doctest cases were not listed: %1").arg(qs(started.error().message())));
        updateActions();
    }

    void doctestListed(std::vector<run::TestRunner::DoctestListing> listings,
                       std::optional<std::vector<std::string>> thenRun) {
        std::size_t cases = 0, tests = 0;
        QStringList failures;
        std::vector<std::pair<std::string, std::vector<run::DoctestCase>>> listed;
        for (run::TestRunner::DoctestListing& listing : listings) {
            if (!listing.cases) {
                failures << QStringLiteral("%1: %2").arg(qs(listing.test), qs(listing.cases.error().message()));
                continue;
            }
            ++tests;
            cases += listing.cases->size();
            listed.emplace_back(std::move(listing.test), std::move(*listing.cases));
        }
        m_tree->setDoctestCases(std::move(listed)); // One rebuild, under the filter setTests kept.
        QString text = m_listedSummary + QStringLiteral("; %1 doctest case(s) in %2 test(s)").arg(cases).arg(tests);
        if (!failures.isEmpty()) {
            text += QStringLiteral("; listing failed for %1 test(s)").arg(failures.size());
            m_details->setPlainText(failures.join(QLatin1Char('\n')));
        }
        setSummary(text);
        updateActions();
        if (thenRun)
            startRun(std::move(*thenRun));
    }

    // Whether `name`'s cases were listed by doctest (its program is a doctest program).
    bool isDoctestTest(const std::string& name) const { return m_tree->doctestCases(name) != nullptr; }

    /*
     * Run Selected (header comment, "doctest"): a selection of only doctest tests and cases that
     * holds a case runs them with doctest directly, which gives every case's result; any other
     * selection runs its tests with ctest.
     */
    void runSelection() {
        const std::vector<std::string> names = m_tree->selectedTestNames();
        if (names.empty())
            return;
        const auto cases = m_tree->selectedCases();
        if (!cases.empty() && std::ranges::all_of(names, [this](const std::string& n) { return isDoctestTest(n); })) {
            runDoctest(names, cases);
            return;
        }
        requestRun(names);
    }

    // Runs `names` with doctest: the whole test, or only its cases in `cases` when it has any there
    // and its own row is not selected.
    void runDoctest(const std::vector<std::string>& names,
                    const std::vector<std::pair<std::string, std::string>>& cases) {
        if (!m_setup || m_runner->busy() || sourceKey() != m_listedKey)
            return;
        std::set<std::string> wholeTests;
        for (const QTreeWidgetItem* item : m_tree->selectedItems())
            if (item->data(0, ui::TestResultsTree::kCaseNameRole).toString().isEmpty() &&
                !item->data(0, ui::TestResultsTree::kTestNameRole).toString().isEmpty())
                wholeTests.insert(item->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString());
        std::vector<run::TestRunner::DoctestRunRequest> requests;
        std::vector<std::string> started;
        for (const std::string& name : names) {
            const run::DiscoveredTest* test = m_tree->test(name);
            if (!test || !isDoctestTest(name))
                continue;
            run::TestRunner::DoctestRunRequest request;
            request.test = *test;
            if (!wholeTests.contains(name))
                for (const auto& [testName, caseName] : cases)
                    if (testName == name)
                        request.cases.push_back(caseName);
            requests.push_back(std::move(request));
            started.push_back(name);
        }
        if (requests.empty())
            return;
        m_tree->markPending(started);
        m_console.clear();
        m_runKind = QStringLiteral("doctest");
        auto run = m_runner->runDoctest(*m_setup, std::move(requests), runHandlers());
        if (!run) {
            m_tree->finishRun({});
            setSummary(QStringLiteral("doctest could not start: %1").arg(qs(run.error().message())));
        } else {
            setSummary(QStringLiteral("Running %1 test(s) with doctest...").arg(started.size()));
        }
        updateActions();
    }

    // The callbacks of a run (ctest or doctest), posted to the GUI thread.
    run::TestRunHandlers runHandlers() {
        run::TestRunHandlers handlers;
        handlers.onTestStarted = [this](const std::string& name) { post([this, name] { m_tree->testStarted(name); }); };
        handlers.onTestFinished = [this](const run::TestResult& result) {
            post([this, result] { m_tree->applyResult(result); });
        };
        handlers.onOutput = [this](std::string_view text) {
            post([this, chunk = qs(text)] {
                m_console += chunk;
                if (m_console.size() > kMaxConsole)
                    m_console.remove(0, m_console.size() - kMaxConsole);
            });
        };
        handlers.onFinished = [this](const run::TestRunReport& report) { post([this, report] { finished(report); }); };
        return handlers;
    }

    // Runs `names` (all when empty), listing the tests first when the source changed.
    void requestRun(std::vector<std::string> names) {
        if (m_runner->busy())
            return;
        if (!m_setup || sourceKey() != m_listedKey) {
            refresh(std::move(names));
            return;
        }
        startRun(std::move(names));
    }

    void startRun(std::vector<std::string> names) {
        if (!m_setup || m_runner->busy())
            return;
        std::vector<run::DiscoveredTest> tests;
        for (const std::string& name : m_tree->testNames())
            if (const run::DiscoveredTest* test = m_tree->test(name))
                tests.push_back(*test);
        const std::size_t count = names.empty() ? tests.size() : names.size();
        m_tree->markPending(names.empty() ? m_tree->testNames() : names);
        m_console.clear();
        m_runKind = QStringLiteral("ctest");
        auto started = m_runner->run(*m_setup, std::move(tests), std::move(names), runHandlers());
        if (!started) {
            m_tree->finishRun({});
            setSummary(QStringLiteral("ctest could not start: %1").arg(qs(started.error().message())));
        } else {
            setSummary(QStringLiteral("Running %1 test(s) with ctest...").arg(count));
        }
        updateActions();
    }

    void finished(const run::TestRunReport& report) {
        m_lastCommand = qs(report.commandLine);
        m_tree->finishRun(report);
        if (report.error) {
            const QString message = qs(report.error->message());
            setSummary(QStringLiteral("%1 failed: %2").arg(m_runKind, message.section(QLatin1Char('\n'), 0, 0)));
            m_details->setPlainText(message);
            m_locations->clear();
            updateActions();
            return;
        }
        std::size_t passed = 0, failed = 0, skipped = 0, disabled = 0;
        for (const run::TestResult& result : report.results) {
            switch (result.outcome) {
            case run::TestOutcome::Passed: ++passed; break;
            case run::TestOutcome::Failed:
            case run::TestOutcome::NotRun: ++failed; break;
            case run::TestOutcome::Skipped: ++skipped; break;
            case run::TestOutcome::Disabled: ++disabled; break;
            }
        }
        QString text = report.stopped ? QStringLiteral("Stopped after %1 test(s): ").arg(report.results.size())
                                      : QStringLiteral("Ran %1 test(s): ").arg(report.results.size());
        text += QStringLiteral("%1 passed, %2 failed, %3 skipped, %4 disabled.")
                    .arg(passed)
                    .arg(failed)
                    .arg(skipped)
                    .arg(disabled);
        setSummary(text);
        showDetails();
        updateActions();
    }

    void updateActions() {
        const bool busy = m_runner->busy();
        const bool ready = m_setup.has_value() && !busy;
        const QString why = m_unavailable.isEmpty() ? QStringLiteral("A test job is running.") : m_unavailable;
        const auto enable = [&](QPushButton* b, bool on, const QString& tip, const QString& off) {
            b->setEnabled(on);
            b->setToolTip(on ? tip : off);
        };
        enable(m_runAll, ready, QStringLiteral("Run every test with ctest."), why);
        enable(m_runSelected, ready && !m_tree->selectedTestNames().empty(),
               QStringLiteral("Run the selected tests with ctest."),
               ready ? QStringLiteral("Select the tests to run.") : why);
        enable(m_runFailed, ready && !m_tree->failedTestNames().empty(),
               QStringLiteral("Run again the tests that failed or did not run."),
               ready ? QStringLiteral("No test failed in the last run.") : why);
        enable(m_stop, busy, QStringLiteral("Stop ctest and every test process it started."),
               QStringLiteral("Nothing is running."));
        enable(m_refresh, !busy, QStringLiteral("List the tests again (ctest -N)."),
               QStringLiteral("A test job is running."));
    }

    // The location to open for a test or case (header comment), relative paths against the build directory.
    std::optional<run::TestLocation> locationOf(const std::string& testName, const std::string& caseName) const {
        const run::DiscoveredTest* test = m_tree->test(testName);
        const run::TestResult* result = m_tree->result(testName);
        if (!caseName.empty()) {
            if (result) {
                for (const run::TestResult& testCase : result->cases) {
                    if (testCase.name != caseName)
                        continue;
                    for (const run::TestFailure& failure : testCase.failures)
                        if (failure.location)
                            return failure.location;
                    if (testCase.definition)
                        return testCase.definition;
                }
            }
            if (const auto* listed = m_tree->doctestCases(testName)) {
                const auto found = std::ranges::find(*listed, caseName, &run::DoctestCase::name);
                if (found != listed->end() && found->location)
                    return found->location;
            }
            return std::nullopt;
        }
        if (result)
            for (const run::TestFailure& failure : result->failures)
                if (failure.location)
                    return failure.location;
        if (test && test->definition)
            return test->definition;
        if (test && test->declaration)
            return test->declaration;
        return std::nullopt;
    }

    std::string absolute(const std::string& file) const {
        if (io::isAbsolutePath(file) || !m_setup)
            return io::normalizePath(file).value_or(file);
        return io::joinPath(m_setup->buildDirectory, file).value_or(file);
    }

    void open(const std::string& test, const std::string& caseName) {
        if (const auto location = locationOf(test, caseName); location && m_context.openLocation)
            m_context.openLocation(absolute(location->file), location->line);
    }

    void addLocation(const QString& label, const run::TestLocation& location) {
        const std::string path = absolute(location.file);
        auto* item =
            new QListWidgetItem(QStringLiteral("%1: %2:%3").arg(label, qs(path)).arg(location.line), m_locations);
        item->setData(kLocationPathRole, qs(path));
        item->setData(kLocationLineRole, location.line);
    }

    void showDetails() {
        m_locations->clear();
        const QList<QTreeWidgetItem*> selection = m_tree->selectedItems();
        QTreeWidgetItem* item = selection.isEmpty() ? nullptr : selection.front();
        const std::string testName =
            item ? item->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString() : std::string();
        if (testName.empty()) {
            // No test selected: the last run's ctest console, or why there are no tests.
            if (!m_console.isEmpty())
                m_details->setPlainText(m_lastCommand + QLatin1Char('\n') + m_console);
            else
                m_details->setPlainText(m_unavailable);
            return;
        }
        const std::string caseName = item->data(0, ui::TestResultsTree::kCaseNameRole).toString().toStdString();
        const run::DiscoveredTest* test = m_tree->test(testName);
        const run::TestResult* result = m_tree->result(testName);
        QStringList lines;
        const auto describe = [&](const run::TestResult& r) {
            lines << QStringLiteral("Status: %1").arg(qs(run::toString(r.outcome)));
            if (!r.reason.empty())
                lines << QStringLiteral("Reason: %1").arg(qs(r.reason));
            if (r.duration)
                lines << QStringLiteral("Duration: %1").arg(qs(run::formatTestDuration(r.duration)));
            if (r.interim)
                lines << QStringLiteral("(from ctest's progress; its report has not been read yet)");
            for (const run::TestFailure& failure : r.failures) {
                lines << QString() << QStringLiteral("Failure:") << qs(failure.message);
                if (failure.location)
                    addLocation(QStringLiteral("Failure"), *failure.location);
            }
        };
        if (!caseName.empty()) {
            const auto* doctestCases = m_tree->doctestCases(testName);
            lines << QStringLiteral("%1 case: %2")
                         .arg(doctestCases ? QStringLiteral("doctest") : QStringLiteral("Google Test"), qs(caseName))
                  << QStringLiteral("Runs in: %1").arg(qs(testName));
            if (doctestCases) {
                const auto listed = std::ranges::find(*doctestCases, caseName, &run::DoctestCase::name);
                if (listed != doctestCases->end() && !listed->suite.empty())
                    lines << QStringLiteral("Test suite: %1").arg(qs(listed->suite));
                if (listed != doctestCases->end() && listed->location)
                    addLocation(QStringLiteral("Defined"), *listed->location);
            }
            const run::TestResult* caseResult = nullptr;
            if (result)
                for (const run::TestResult& c : result->cases)
                    if (c.name == caseName)
                        caseResult = &c;
            if (caseResult) {
                describe(*caseResult);
                if (caseResult->definition)
                    addLocation(QStringLiteral("Defined"), *caseResult->definition);
            } else {
                lines << QStringLiteral("Status: no result yet");
            }
            m_details->setPlainText(lines.join(QLatin1Char('\n')));
            return;
        }
        lines << QStringLiteral("Test: %1").arg(qs(testName));
        if (test) {
            if (!test->command.empty()) {
                run::LaunchPlan plan;
                plan.program = test->command.front();
                plan.arguments.assign(test->command.begin() + 1, test->command.end());
                lines << QStringLiteral("Command: %1").arg(qs(run::displayCommandLine(plan)));
            } else {
                lines << QStringLiteral("Command: none (ctest found no program for this test)");
            }
            if (!test->workingDirectory.empty())
                lines << QStringLiteral("Working directory: %1").arg(qs(test->workingDirectory));
            if (!test->labels.empty()) {
                QStringList labels;
                for (const auto& label : test->labels)
                    labels << qs(label);
                lines << QStringLiteral("Labels: %1").arg(labels.join(QStringLiteral(", ")));
            }
            if (test->disabled)
                lines << QStringLiteral("DISABLED");
            if (test->definition)
                addLocation(QStringLiteral("Defined"), *test->definition);
            if (test->declaration)
                addLocation(QStringLiteral("add_test"), *test->declaration);
        }
        if (result) {
            describe(*result);
            if (!result->output.empty())
                lines << QString() << QStringLiteral("Output:") << qs(result->output);
        } else {
            lines << QStringLiteral("Status: no result yet");
        }
        m_details->setPlainText(lines.join(QLatin1Char('\n')));
    }

    void contextMenu(const QPoint& at) {
        QTreeWidgetItem* item = m_tree->itemAt(at);
        if (!item)
            return;
        const std::string testName = item->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString();
        QMenu menu(this);
        QAction* run = menu.addAction(QStringLiteral("Run"));
        run->setEnabled(m_setup.has_value() && !m_runner->busy());
        const run::DiscoveredTest* test = testName.empty() ? nullptr : m_tree->test(testName);
        const bool idle = m_setup.has_value() && !m_runner->busy();
        QAction* runWithDoctest = menu.addAction(QStringLiteral("Run with doctest (Every Case's Result)"));
        const std::vector<std::string> selected = m_tree->selectedTestNames();
        runWithDoctest->setEnabled(idle && !selected.empty() &&
                                   std::ranges::all_of(selected, [this](const auto& n) { return isDoctestTest(n); }));
        QAction* list = menu.addAction(QStringLiteral("List Google Test Cases"));
        list->setEnabled(test && !test->command.empty() && test->gtestFilter.empty() && idle);
        list->setToolTip(QStringLiteral("Run the test's program with --gtest_list_tests."));
        QAction* listWithDoctest = menu.addAction(QStringLiteral("List doctest Cases"));
        listWithDoctest->setEnabled(test && !test->command.empty() && idle);
        listWithDoctest->setToolTip(QStringLiteral("Run the test's program with doctest's --list-test-cases."));
        QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(at));
        if (chosen == run) {
            if (auto names = m_tree->selectedTestNames(); !names.empty())
                requestRun(std::move(names));
        } else if (chosen == runWithDoctest) {
            runDoctest(m_tree->selectedTestNames(), m_tree->selectedCases());
        } else if (chosen == list && test) {
            listCases(*test);
        } else if (chosen == listWithDoctest && test) {
            m_explicitDoctest.insert(test->name);
            listDoctest({test->name}, std::nullopt);
        }
    }

    void listCases(const run::DiscoveredTest& test) {
        if (!m_setup)
            return;
        const std::string name = test.name;
        auto started =
            m_runner->listGTestCases(*m_setup, test, [this, name](Result<std::vector<run::GTestCase>> cases) {
                post([this, name, cases = std::move(cases)] {
                    if (cases)
                        m_tree->setGTestCases(name, *cases);
                    else
                        setSummary(QStringLiteral("Listing the Google Test cases of %1 failed: %2")
                                       .arg(qs(name), qs(cases.error().message()).section(QLatin1Char('\n'), 0, 0)));
                    updateActions();
                });
            });
        if (!started)
            setSummary(qs(started.error().message()));
        updateActions();
    }

    UnitTestsWindowContext m_context;
    std::unique_ptr<run::TestRunner> m_runner;
    std::optional<run::CTestSetup> m_setup;
    std::string m_listedKey;
    QString m_listedSummary;
    QString m_runKind = QStringLiteral("ctest");
    std::set<std::string> m_explicitDoctest; // Tests the user listed with "List doctest Cases".
    QString m_unavailable;
    QString m_console;
    QString m_lastCommand;
    ui::TestResultsTree* m_tree = nullptr;
    QPushButton* m_runAll = nullptr;
    QPushButton* m_runSelected = nullptr;
    QPushButton* m_runFailed = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_debug = nullptr;
    QLineEdit* m_filter = nullptr;
    QLabel* m_summary = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QListWidget* m_locations = nullptr;
    QTimer* m_watch = nullptr;
};

} // namespace

QWidget* createUnitTestsToolWindowWidget(QWidget* parent, UnitTestsWindowContext context) {
    return new UnitTestsWindow(parent, std::move(context));
}

} // namespace frost::app
