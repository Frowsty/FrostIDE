// SPDX-License-Identifier: Apache-2.0
//
// The Unit Tests tool window (unit_tests_window.hpp): the open CMake folder's ctest setup, a
// run::TestRunner and a ui::TestResultsTree. Every test, case, outcome, duration, message and
// location it shows came from ctest's, Google Test's or doctest's output through frost::run's parsers.

#include "unit_tests_window.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/test_runner.hpp"
#include "frost/ui/test_results_tree.hpp"

#include "project_properties_dialog.hpp"
#include "trusted_project_launcher.hpp"
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
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
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

QString debugUnavailable() { return QStringLiteral("Select a single test or case to debug (RUN-10)."); }

class UnitTestsWindow final : public QWidget {
public:
    UnitTestsWindow(QWidget* parent, UnitTestsWindowContext context)
        : QWidget(parent), m_context(std::move(context)),
          m_runner(run::TestRunner::create(*m_context.launcher, *m_context.fs)), m_summary(new QLabel(this)),
          m_watch(new QTimer(this)) {
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
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): Preserve Qt tab order.
        m_filter = new QLineEdit(this);
        m_filter->setObjectName(QStringLiteral("unitTestsFilterEdit"));
        m_filter->setPlaceholderText(QStringLiteral("Filter tests..."));
        toolbar->addWidget(m_filter, 1);
        outer->addLayout(toolbar);

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
        connect(m_debug, &QPushButton::clicked, this, [this] { debugSelected(); });
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
        m_watch->setInterval(1000);
        connect(m_watch, &QTimer::timeout, this, [this] {
            if (unitTestsSourceKey(m_context) != m_listedKey) {
                m_runner->cancel();
                if (!m_runner->busy())
                    refresh();
            }
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

    [[nodiscard]] std::vector<run::GutterTestCase> gutterCasesForFile(std::string_view filePath,
                                                                      std::string_view content) const {
        if (!listingCurrent())
            return {};
        std::vector<run::DiscoveredTest> tests;
        std::vector<std::pair<std::string, std::vector<run::DoctestCase>>> doctestListings;
        for (const std::string& name : m_tree->testNames()) {
            if (const auto* t = m_tree->test(name))
                tests.push_back(*t);
            if (const auto* dc = m_tree->doctestCases(name))
                doctestListings.emplace_back(name, *dc);
        }
        std::vector<std::pair<std::string, std::vector<run::GTestCase>>> gtestListings(m_listedGTestCases.begin(),
                                                                                       m_listedGTestCases.end());
        auto resolved = run::resolveGutterTestCasesInFile(filePath, content, tests, doctestListings, gtestListings,
                                                 m_setup->buildDirectory);
        const auto requestedPath = io::normalizePath(filePath);
        for (const auto& test : tests) if (const auto* cases = m_tree->frameworkCases(test.name); cases && m_frameworkProfiles.contains(test.name))
            for (const auto& entry : *cases) if (entry.location && entry.location->line > 0) {
                const auto path = io::normalizePath(absolute(entry.location->file));
                if (requestedPath && path && QString::compare(qs(*path), qs(*requestedPath),
                    io::kNativePathStyle == io::PathStyle::Windows ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0)
                    resolved.push_back(run::GutterTestCase{test, entry.name,
                        m_frameworkProfiles.at(test.name) == run::FrameworkProfile::Catch2Xml3 ? run::TestCaseFramework::Catch2 : run::TestCaseFramework::BoostTest,
                        entry.location->line, entry.name});
            }
        return resolved;
    }

    [[nodiscard]] Result<void> runSingleCase(const run::GutterTestCase& tc) {
        if (!listingCurrent() || m_runner->busy())
            return fail(ErrorCode::Busy, "Unit tests runner is busy or not configured");
        if (!currentGutterCase(tc))
            return fail(ErrorCode::InvalidArgument, "The gutter test is no longer in the current test listing");
        if ((tc.framework == run::TestCaseFramework::Catch2 || tc.framework == run::TestCaseFramework::BoostTest) && !tc.caseName.empty())
            return runFramework({tc.test.name}, {{tc.test.name, tc.caseName}}, false);
        if (tc.framework == run::TestCaseFramework::Doctest && !tc.caseName.empty())
            return startDoctestRequests({run::TestRunner::DoctestRunRequest{.test = tc.test, .cases = {tc.caseName}}},
                                        {tc.test.name});
        if (tc.framework == run::TestCaseFramework::GoogleTest && !tc.caseName.empty())
            return startGTestRequests({run::TestRunner::GTestRunRequest{.test = tc.test, .cases = {tc.caseName}}},
                                      {tc.test.name});
        startRun({tc.test.name});
        return {};
    }

    [[nodiscard]] Result<void> debugSingleCase(const run::GutterTestCase& tc) {
        if (!listingCurrent() || m_runner->busy())
            return fail(ErrorCode::Busy, "Unit tests are busy or not configured");
        if (!currentGutterCase(tc))
            return fail(ErrorCode::InvalidArgument, "The gutter test is no longer in the current test listing");
        auto launch = run::buildSingleTestLaunch(*m_setup, tc.test, tc.caseName, tc.framework);
        if (!launch)
            return std::unexpected(launch.error());
        return startDebug(*launch);
    }

    [[nodiscard]] Result<void> configureFramework(const std::string& name, run::FrameworkProfile profile) {
        if (!listingCurrent() || m_runner->busy()) return fail(ErrorCode::Busy, "Tests are busy or not configured");
        const auto* test = m_tree->test(name);
        if (!test) return fail(ErrorCode::NotFound, "CTest did not discover this test");
        return listFramework(*test, profile);
    }
private:
    [[nodiscard]] bool currentGutterCase(const run::GutterTestCase& tc) const {
        const auto* test = m_tree->test(tc.test.name);
        if (!test || *test != tc.test)
            return false;
        if (tc.framework == run::TestCaseFramework::CTestWhole)
            return tc.caseName.empty();
        if (tc.framework == run::TestCaseFramework::Catch2 || tc.framework == run::TestCaseFramework::BoostTest) {
            const auto* cases = m_tree->frameworkCases(tc.test.name);
            const auto profile = m_frameworkProfiles.find(tc.test.name);
            const auto expected = tc.framework == run::TestCaseFramework::Catch2 ? run::FrameworkProfile::Catch2Xml3 : run::FrameworkProfile::BoostTest185;
            return cases && profile != m_frameworkProfiles.end() && profile->second == expected &&
                std::ranges::any_of(*cases, [&tc](const auto& item) { return item.name == tc.caseName; });
        }
        if (tc.framework == run::TestCaseFramework::Doctest) {
            const auto* cases = m_tree->doctestCases(tc.test.name);
            return cases && std::ranges::any_of(*cases, [&tc](const auto& item) { return item.name == tc.caseName; });
        }
        // CTest can register several exact cases in one positive Google Test filter. A gutter
        // action narrows that command to one of its tokens; wildcards and exclusions are never
        // evidence for an individual case identity.
        if (!tc.caseName.empty() && tc.caseName.find_first_of("*?:-") == std::string::npos &&
            tc.caseName.find('.') != std::string::npos && test->gtestFilter.find('-') == std::string::npos) {
            std::string_view remaining(test->gtestFilter);
            while (!remaining.empty()) {
                const auto separator = remaining.find(':');
                if (remaining.substr(0, separator) == tc.caseName)
                    return true;
                if (separator == std::string_view::npos)
                    break;
                remaining.remove_prefix(separator + 1);
            }
        }
        const auto listed = m_listedGTestCases.find(tc.test.name);
        return listed != m_listedGTestCases.end() &&
               std::ranges::any_of(listed->second, [&tc](const auto& item) { return item.fullName() == tc.caseName; });
    }

    [[nodiscard]] bool listingCurrent() const {
        return m_setup.has_value() && !m_listedKey.empty() && unitTestsSourceKey(m_context) == m_listedKey;
    }

    // Posts `work` to the GUI thread (the runner calls back on its own threads).
    template<typename F>
    void post(F work) {
        QMetaObject::invokeMethod(this, std::move(work), Qt::QueuedConnection);
    }

    // A job's callbacks must not publish to a new project/profile or a later trust grant.
    template<typename F>
    void postForSource(const std::string& source, std::uint64_t generation, F work) {
        post([this, source, generation, work = std::move(work)]() mutable {
            if (generation == m_listingGeneration && source == m_listedKey && source == unitTestsSourceKey(m_context))
                work();
        });
    }

    void setSummary(const QString& text) { m_summary->setText(text); }

    void notifyTestsChanged() {
        if (m_context.testsChanged)
            m_context.testsChanged();
    }

    void refresh(std::optional<std::vector<std::string>> thenRun = std::nullopt) {
        if (m_runner->busy())
            return;
        const std::string key = unitTestsSourceKey(m_context);
        if (m_listedKey != key)
            m_explicitDoctest.clear();
        m_listedKey = key;
        const std::uint64_t generation = ++m_listingGeneration;
        m_listedGTestCases.clear();
        UnitTestsSource current = unitTestsSource(m_context);
        m_setup = current.setup;
        if (!m_setup) {
            m_tree->setTests({});
            setSummary(current.unavailable);
            m_unavailable = current.unavailable;
            showDetails();
            updateActions();
            notifyTestsChanged();
            return;
        }
        m_unavailable.clear();
        // Each setup captures its trust admission. Delayed batch launches check that revision
        // through the adapter, without consulting GUI-owned CMake state on a worker thread.
        m_runner.reset();
        m_projectLauncher = std::make_unique<TrustedProjectLauncher>(*m_context.launcher, *m_context.trust,
                                                                     current.trustRoot, current.trustRevision);
        m_runner = run::TestRunner::create(*m_projectLauncher, *m_context.fs);
        setSummary(QStringLiteral("Listing the tests (ctest -N in %1)...").arg(qs(m_setup->buildDirectory)));
        const run::CTestSetup setup = *m_setup;
        auto started = m_runner->discover(setup, [this, key, generation, thenRun = std::move(thenRun)](
                                                     Result<std::vector<run::DiscoveredTest>> tests) mutable {
            postForSource(key, generation, [this, thenRun = std::move(thenRun), tests = std::move(tests)]() mutable {
                listed(std::move(tests), std::move(thenRun));
            });
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
            notifyTestsChanged();
            return;
        }
        const std::size_t count = tests->size();
        m_tree->setTests(std::move(*tests));
        m_tree->setFilter(m_filter->text());
        QString text = QStringLiteral("%1 test(s) listed by ctest in %2").arg(count).arg(qs(m_setup->buildDirectory));
        if (!m_setup->testPreset.empty())
            text += QStringLiteral(" (test preset %1)").arg(qs(m_setup->testPreset));
        setSummary(text);
        m_listedSummary = std::move(text);
        showDetails();
        updateActions();
        notifyTestsChanged();
        listDoctest(m_tree->testNames(), std::move(thenRun));
    }

    // Lists the doctest cases of `names`' doctest programs (header comment, "doctest"), then runs
    // `thenRun` when given.
    void listDoctest(const std::vector<std::string>& names, std::optional<std::vector<std::string>> thenRun) {
        if (!listingCurrent())
            return;
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
            *m_setup, std::move(doctest),
            [this, key = m_listedKey, generation = m_listingGeneration,
             thenRun](std::vector<run::TestRunner::DoctestListing> listings) {
                postForSource(key, generation, [this, thenRun, listings = std::move(listings)]() mutable {
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
        if (!listingCurrent())
            return;
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
        notifyTestsChanged();
        if (thenRun)
            startRun(std::move(*thenRun));
    }

    // Whether `name`'s cases were listed by doctest (its program is a doctest program).
    bool isDoctestTest(const std::string& name) const { return m_tree->doctestCases(name) != nullptr; }

    /*
     * Run Selected (header comment, "doctest"): a selection of only doctest tests and cases that
     * holds a case runs them with doctest directly, which gives every case's result; a selection
     * of Google Test cases runs them with --gtest_filter directly (RUN-10); any other selection
     * runs its tests with ctest.
     */
    void runSelection() {
        const std::vector<std::string> names = m_tree->selectedTestNames();
        if (names.empty())
            return;
        const auto cases = m_tree->selectedCases();
        const auto hasFramework = [this](const std::string& name) { return m_tree->frameworkCases(name) && m_frameworkProfiles.contains(name); };
        if (std::ranges::all_of(names, hasFramework)) { static_cast<void>(runFramework(names, cases)); return; }
        if (!cases.empty() && std::ranges::any_of(names, hasFramework)) {
            setSummary(QStringLiteral("Select cases from compatible direct framework providers, or select whole CTest tests.")); return;
        }
        if (!cases.empty()) {
            if (std::ranges::all_of(names, [this](const std::string& n) { return isDoctestTest(n); })) {
                runDoctest(names, cases);
                return;
            }
            if (std::ranges::none_of(names, [this](const std::string& n) { return isDoctestTest(n); })) {
                runGTest(names, cases);
                return;
            }
        }
        requestRun(names);
    }

    std::set<std::string> selectedWholeTests() const {
        std::set<std::string> wholeTests;
        for (const QTreeWidgetItem* item : m_tree->selectedItems())
            if (item->data(0, ui::TestResultsTree::kCaseNameRole).toString().isEmpty() &&
                !item->data(0, ui::TestResultsTree::kTestNameRole).toString().isEmpty())
                wholeTests.insert(item->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString());
        return wholeTests;
    }

    // Runs `names` with doctest: the whole test, or only its cases in `cases` when it has any there
    // and its own row is not selected.
    void runDoctest(const std::vector<std::string>& names,
                    const std::vector<std::pair<std::string, std::string>>& cases) {
        if (!listingCurrent() || m_runner->busy())
            return;
        const std::set<std::string> wholeTests = selectedWholeTests();
        std::vector<run::TestRunner::DoctestRunRequest> requests;
        std::vector<std::string> started;
        for (const std::string& name : names) {
            const run::DiscoveredTest* test = m_tree->test(name);
            if (!test || !isDoctestTest(name))
                continue;
            run::TestRunner::DoctestRunRequest request{.test = *test};
            if (!wholeTests.contains(name))
                for (const auto& [testName, caseName] : cases)
                    if (testName == name)
                        request.cases.push_back(caseName);
            requests.push_back(std::move(request));
            started.push_back(name);
        }
        (void)startDoctestRequests(std::move(requests), started);
    }

    Result<void> startDoctestRequests(std::vector<run::TestRunner::DoctestRunRequest> requests,
                                      const std::vector<std::string>& started) {
        if (requests.empty() || !m_setup)
            return fail(ErrorCode::InvalidArgument, "No doctest request to run");
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
        return run;
    }

    void runGTest(const std::vector<std::string>& names,
                  const std::vector<std::pair<std::string, std::string>>& cases) {
        if (!listingCurrent() || m_runner->busy())
            return;
        const std::set<std::string> wholeTests = selectedWholeTests();
        std::vector<run::TestRunner::GTestRunRequest> requests;
        std::vector<std::string> started;
        for (const std::string& name : names) {
            const run::DiscoveredTest* test = m_tree->test(name);
            if (!test || isDoctestTest(name))
                continue;
            run::TestRunner::GTestRunRequest request{.test = *test};
            if (!wholeTests.contains(name))
                for (const auto& [testName, caseName] : cases)
                    if (testName == name)
                        request.cases.push_back(caseName);
            requests.push_back(std::move(request));
            started.push_back(name);
        }
        (void)startGTestRequests(std::move(requests), started);
    }

    Result<void> startGTestRequests(std::vector<run::TestRunner::GTestRunRequest> requests,
                                    const std::vector<std::string>& started) {
        if (requests.empty() || !m_setup)
            return fail(ErrorCode::InvalidArgument, "No Google Test request to run");
        m_tree->markPending(started);
        m_console.clear();
        m_runKind = QStringLiteral("Google Test");
        auto run = m_runner->runGTest(*m_setup, std::move(requests), runHandlers());
        if (!run) {
            m_tree->finishRun({});
            setSummary(QStringLiteral("Google Test could not start: %1").arg(qs(run.error().message())));
        } else {
            setSummary(QStringLiteral("Running %1 test(s) with Google Test...").arg(started.size()));
        }
        updateActions();
        return run;
    }

    std::optional<run::SingleTestLaunch> selectedSingleTarget() const {
        if (!listingCurrent())
            return std::nullopt;
        const auto items = m_tree->selectedItems();
        if (items.size() != 1)
            return std::nullopt;
        const std::string testName =
            items.front()->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString();
        const std::string caseName =
            items.front()->data(0, ui::TestResultsTree::kCaseNameRole).toString().toStdString();
        const run::DiscoveredTest* test = testName.empty() ? nullptr : m_tree->test(testName);
        if (!test || test->command.empty())
            return std::nullopt;
        if (!caseName.empty() && m_tree->frameworkCases(testName) && m_frameworkProfiles.contains(testName)) {
            const auto framework = m_frameworkProfiles.at(testName) == run::FrameworkProfile::Catch2Xml3
                ? run::TestCaseFramework::Catch2 : run::TestCaseFramework::BoostTest;
            auto launch = run::buildSingleTestLaunch(*m_setup, *test, caseName, framework);
            return launch ? std::optional<run::SingleTestLaunch>(std::move(*launch)) : std::nullopt;
        }
        const auto framework = caseName.empty()          ? run::TestCaseFramework::CTestWhole
                               : isDoctestTest(testName) ? run::TestCaseFramework::Doctest
                                                         : run::TestCaseFramework::GoogleTest;
        auto launch = run::buildSingleTestLaunch(*m_setup, *test, caseName, framework);
        return launch ? std::optional<run::SingleTestLaunch>(std::move(*launch)) : std::nullopt;
    }

    void debugSelected() {
        if (auto launch = selectedSingleTarget())
            (void)startDebug(*launch);
    }

    Result<void> startDebug(const run::SingleTestLaunch& launch) {
        if (!listingCurrent() || m_runner->busy())
            return fail(ErrorCode::Busy, "Unit tests source changed or the runner is busy");
        if (!m_context.debugTest)
            return fail(ErrorCode::NotFound, "No debug handler is configured");
        auto started = m_context.debugTest(launch);
        if (!started)
            setSummary(QStringLiteral("Could not start debugging %1: %2")
                           .arg(qs(launch.displayName), qs(started.error().message())));
        else
            setSummary(QStringLiteral("Debugging %1...").arg(qs(launch.displayName)));
        return started;
    }

    // The callbacks of a run (ctest or doctest), posted to the GUI thread.
    run::TestRunHandlers runHandlers() {
        run::TestRunHandlers handlers;
        const std::string key = m_listedKey;
        const std::uint64_t generation = m_listingGeneration;
        handlers.onTestStarted = [this, key, generation](const std::string& name) {
            postForSource(key, generation, [this, name] { m_tree->testStarted(name); });
        };
        handlers.onTestFinished = [this, key, generation](const run::TestResult& result) {
            postForSource(key, generation, [this, result] { m_tree->applyResult(result); });
        };
        handlers.onOutput = [this, key, generation](std::string_view text) {
            postForSource(key, generation, [this, chunk = qs(text)] {
                m_console += chunk;
                if (m_console.size() > kMaxConsole)
                    m_console.remove(0, m_console.size() - kMaxConsole);
            });
        };
        handlers.onFinished = [this, key, generation](const run::TestRunReport& report) {
            postForSource(key, generation, [this, report] { finished(report); });
        };
        return handlers;
    }

    // Runs `names` (all when empty), listing the tests first when the source changed.
    void requestRun(std::vector<std::string> names) {
        if (m_runner->busy())
            return;
        if (!listingCurrent()) {
            refresh(std::move(names));
            return;
        }
        startRun(std::move(names));
    }

    void startRun(std::vector<std::string> names) {
        if (!listingCurrent() || m_runner->busy())
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
        const bool ready = listingCurrent() && !busy;
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
        enable(m_debug, ready && m_context.debugTest != nullptr && selectedSingleTarget().has_value(),
               QStringLiteral("Debug the selected test or case (RUN-10)."), debugUnavailable());
    }

    std::string absolute(const std::string& file) const {
        if (io::isAbsolutePath(file) || !m_setup)
            return io::normalizePath(file).value_or(file);
        return io::joinPath(m_setup->buildDirectory, file).value_or(file);
    }

    void open(const std::string& test, const std::string& caseName) {
        if (const auto actual = unitTestsLocationOf(test, caseName, nullptr, m_tree->result(test), nullptr); actual && m_context.openLocation) {
            m_context.openLocation(absolute(actual->file), actual->line); return;
        }
        if (const auto* cases = m_tree->frameworkCases(test))
            if (const auto found = std::ranges::find(*cases, caseName, &run::FrameworkCase::name); found != cases->end() && found->location && m_context.openLocation) {
                m_context.openLocation(absolute(found->location->file), found->location->line); return;
            }
        const auto location =
            unitTestsLocationOf(test, caseName, m_tree->test(test), m_tree->result(test), m_tree->doctestCases(test));
        if (location && m_context.openLocation)
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
        const UnitTestsSelectionDetails details = unitTestsDescribeSelection(
            testName, caseName, m_tree->test(testName), m_tree->result(testName), m_tree->doctestCases(testName));
        for (const auto& [label, loc] : details.locations)
            addLocation(label, loc);
        m_details->setPlainText(details.text);
    }

    void contextMenu(const QPoint& at) {
        QTreeWidgetItem* item = m_tree->itemAt(at);
        if (!item)
            return;
        const std::string testName = item->data(0, ui::TestResultsTree::kTestNameRole).toString().toStdString();
        QMenu menu(this);
        const bool idle = listingCurrent() && !m_runner->busy();
        QAction* run = menu.addAction(QStringLiteral("Run"));
        run->setEnabled(idle);
        QAction* debug = menu.addAction(QStringLiteral("Debug"));
        const auto singleTarget = selectedSingleTarget();
        debug->setEnabled(idle && m_context.debugTest != nullptr && singleTarget.has_value());
        const run::DiscoveredTest* test = testName.empty() ? nullptr : m_tree->test(testName);
        const auto testCopy = test ? std::optional<run::DiscoveredTest>(*test) : std::nullopt;
        const auto generation = m_listingGeneration;
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
        QAction* catch2 = menu.addAction(QStringLiteral("List cases using Catch2 3 XML profile"));
        QAction* boost = menu.addAction(QStringLiteral("List cases using Boost.Test 1.85 XML profile"));
        catch2->setEnabled(test && !test->command.empty() && idle);
        boost->setEnabled(catch2->isEnabled());
        catch2->setToolTip(QStringLiteral("Explicit profile: Catch2 3 XML listing and run schema 3; older schemas are refused."));
        boost->setToolTip(QStringLiteral("Explicit profile: Boost.Test 1.85 DOT discovery and detailed XML results."));
        QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(at));
        if (generation != m_listingGeneration || !listingCurrent())
            return;
        if ((chosen == catch2 || chosen == boost) && testCopy) {
            static_cast<void>(listFramework(*testCopy, chosen == catch2 ? run::FrameworkProfile::Catch2Xml3 : run::FrameworkProfile::BoostTest185));
        } else if (chosen == run) {
            runSelection();
        } else if (chosen == debug && singleTarget) {
            (void)startDebug(*singleTarget);
        } else if (chosen == runWithDoctest) {
            runDoctest(m_tree->selectedTestNames(), m_tree->selectedCases());
        } else if (chosen == list && testCopy) {
            listCases(*testCopy);
        } else if (chosen == listWithDoctest && testCopy) {
            m_explicitDoctest.insert(testCopy->name);
            listDoctest({testCopy->name}, std::nullopt);
        }
    }

    Result<void> listFramework(const run::DiscoveredTest& test, run::FrameworkProfile profile) {
        if (!listingCurrent() || m_runner->busy()) return fail(ErrorCode::Busy, "Tests are busy or not configured");
        const auto name = test.name;
        auto started = m_runner->listFrameworkCases(*m_setup, test, profile,
            [this, key = m_listedKey, generation = m_listingGeneration, name, profile](Result<std::vector<run::FrameworkCase>> cases) {
                postForSource(key, generation, [this, name, profile, cases = std::move(cases)]() mutable {
                    if (!cases) setSummary(qs(cases.error().message()));
                    else {
                        m_frameworkProfiles[name] = profile;
                        m_tree->setFrameworkCases(name, std::move(*cases));
                        setSummary(QStringLiteral("Listed %1 using %2. Select case rows and Run Selected or Debug.")
                            .arg(qs(name), qs(run::frameworkProfileName(profile))));
                        notifyTestsChanged();
                    }
                    updateActions();
                });
            });
        if (!started) setSummary(qs(started.error().message()));
        updateActions();
        return started;
    }
    Result<void> runFramework(const std::vector<std::string>& names, const std::vector<std::pair<std::string, std::string>>& selected, bool fromTree = true) {
        std::vector<run::TestRunner::FrameworkRunRequest> requests;
        const auto whole = fromTree ? selectedWholeTests() : std::set<std::string>{};
        for (const auto& name : names) {
            const auto* test = m_tree->test(name);
            if (!test || !m_tree->frameworkCases(name) || !m_frameworkProfiles.contains(name))
                return fail(ErrorCode::InvalidArgument, "Framework cases are no longer in the current listing");
            run::TestRunner::FrameworkRunRequest request{*test, m_frameworkProfiles.at(name), {}};
            if (!whole.contains(name)) for (const auto& [owner, value] : selected) if (owner == name) request.cases.push_back(value);
            requests.push_back(std::move(request));
        }
        m_tree->markPending(names); m_console.clear(); m_runKind = QStringLiteral("framework");
        auto started = m_runner->runFramework(*m_setup, std::move(requests), runHandlers());
        if (!started) { m_tree->finishRun({}); setSummary(qs(started.error().message())); }
        else setSummary(QStringLiteral("Running selected cases using their explicit framework profiles..."));
        updateActions();
        return started;
    }

    void listCases(const run::DiscoveredTest& test) {
        if (!listingCurrent() || m_runner->busy())
            return;
        const std::string name = test.name;
        auto started = m_runner->listGTestCases(
            *m_setup, test,
            [this, key = m_listedKey, generation = m_listingGeneration,
             name](Result<std::vector<run::GTestCase>> cases) {
                postForSource(key, generation, [this, name, cases = std::move(cases)] {
                    if (cases) {
                        m_listedGTestCases[name] = *cases;
                        m_tree->setGTestCases(name, *cases);
                        notifyTestsChanged();
                    } else {
                        setSummary(QStringLiteral("Listing the Google Test cases of %1 failed: %2")
                                       .arg(qs(name), qs(cases.error().message()).section(QLatin1Char('\n'), 0, 0)));
                    }
                    updateActions();
                });
            });
        if (!started)
            setSummary(qs(started.error().message()));
        updateActions();
    }

    UnitTestsWindowContext m_context;
    std::unique_ptr<TrustedProjectLauncher> m_projectLauncher;
    std::unique_ptr<run::TestRunner> m_runner;
    std::optional<run::CTestSetup> m_setup;
    std::string m_listedKey;
    std::uint64_t m_listingGeneration = 0;
    QString m_listedSummary;
    QString m_runKind = QStringLiteral("ctest");
    std::set<std::string> m_explicitDoctest; // Tests the user listed with "List doctest Cases".
    std::map<std::string, run::FrameworkProfile> m_frameworkProfiles;
    std::map<std::string, std::vector<run::GTestCase>> m_listedGTestCases;
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

// Resolve only a concrete CTest provider or one unambiguous provider page. Object names and
// the currently selected tab cannot establish a widget's type or select its test backend.
UnitTestsWindow* ctestProvider(QWidget* widget) {
    if (auto* direct = dynamic_cast<UnitTestsWindow*>(widget))
        return direct;
    auto* tabs = qobject_cast<QTabWidget*>(widget);
    if (!tabs)
        return nullptr;
    UnitTestsWindow* found = nullptr;
    for (int index = 0; index < tabs->count(); ++index) {
        if (auto* provider = dynamic_cast<UnitTestsWindow*>(tabs->widget(index))) {
            if (found)
                return nullptr;
            found = provider;
        }
    }
    return found;
}

} // namespace

QWidget* createUnitTestsToolWindowWidget(QWidget* parent, UnitTestsWindowContext context) {
    return new UnitTestsWindow(parent, std::move(context));
}

std::vector<run::GutterTestCase> unitTestsGutterCasesForFile(QWidget* unitTestsWidget, std::string_view filePath,
                                                             std::string_view content) {
    auto* window = ctestProvider(unitTestsWidget);
    return window ? window->gutterCasesForFile(filePath, content) : std::vector<run::GutterTestCase>{};
}

Result<void> runGutterTestCase(QWidget* unitTestsWidget, const run::GutterTestCase& testCase) {
    auto* window = ctestProvider(unitTestsWidget);
    if (!window)
        return fail(ErrorCode::NotFound, "Unit Tests window is not created");
    return window->runSingleCase(testCase);
}

Result<void> debugGutterTestCase(QWidget* unitTestsWidget, const run::GutterTestCase& testCase) {
    auto* window = ctestProvider(unitTestsWidget);
    if (!window)
        return fail(ErrorCode::NotFound, "Unit Tests window is not created");
    return window->debugSingleCase(testCase);
}

Result<void> configureTestFramework(QWidget* unitTestsWidget, const std::string& name, run::FrameworkProfile profile) {
    auto* window = ctestProvider(unitTestsWidget);
    if (!window) return fail(ErrorCode::NotFound, "No unambiguous CTest provider");
    return window->configureFramework(name, profile);
}

} // namespace frost::app
