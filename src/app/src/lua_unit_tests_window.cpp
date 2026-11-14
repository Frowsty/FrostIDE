// SPDX-License-Identifier: Apache-2.0
// LUA-12: genuine framework discovery/results through the shared owned TestRunner lifecycle.
#include "lua_unit_tests_window.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/test_results_tree.hpp"

#include "trusted_project_launcher.hpp"
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
namespace frost::app {
namespace {
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
class LuaTestsWindow final : public QWidget {
public:
    LuaTestsWindow(QWidget* parent, LuaUnitTestsContext context) : QWidget(parent), m_context(std::move(context)) {
        setObjectName(QStringLiteral("luaUnitTestsProvider"));
        auto* layout = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        m_profile = new QComboBox(this);
        m_profile->setObjectName(QStringLiteral("luaTestProfile"));
        m_profile->addItems({QStringLiteral("Busted 2.2.0"), QStringLiteral("LuaUnit 3.5")});
        bar->addWidget(m_profile);
        auto button = [&](const QString& label, auto callback) {
            auto* b = new QPushButton(label, this);
            bar->addWidget(b);
            connect(b, &QPushButton::clicked, this, callback);
            return b;
        };
        m_refresh = button(QStringLiteral("Discover"), [this] { refresh(); });
        m_all = button(QStringLiteral("Run All"), [this] { static_cast<void>(run({}, false)); });
        m_selected = button(QStringLiteral("Run Selected"), [this] { static_cast<void>(run(selection(), false)); });
        m_failed = button(QStringLiteral("Run Failed"), [this] {
            std::vector<std::string> cases;
            if (m_source)
                if (auto* result = m_tree->result(m_source->configuration.name))
                    for (const auto& c : result->cases)
                        if (run::countsAsFailure(c.outcome))
                            cases.push_back(c.name);
            if (!cases.empty())
                static_cast<void>(run(std::move(cases), false));
        });
        m_debug = button(QStringLiteral("Debug Selected"), [this] {
            auto names = selection();
            if (names.size() == 1)
                static_cast<void>(run(std::move(names), true));
        });
        m_stop = button(QStringLiteral("Stop"), [this] {
            if (m_runner)
                m_runner->cancel();
        });
        layout->addLayout(bar);
        m_status = new QLabel(
            QStringLiteral("Select a Lua Script run configuration, choose its framework, then Discover."), this);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        m_tree = new ui::TestResultsTree(this);
        layout->addWidget(m_tree, 1);
        m_output = new QPlainTextEdit(this);
        m_output->setReadOnly(true);
        m_output->setMaximumBlockCount(10000);
        layout->addWidget(m_output);
        connect(m_profile, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
        connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
            actions();
            showDetails();
        });
        connect(m_tree, &ui::TestResultsTree::testActivated, this, [this](const QString&, const QString& name) {
            if (!m_source || !m_context.openLocation)
                return;
            for (const auto& c : m_cases)
                if (qs(c.name) == name && c.location) {
                    auto path = io::isAbsolutePath(c.location->file)
                                    ? Result<std::string>(c.location->file)
                                    : io::joinPath(workingDirectory(), c.location->file);
                    if (path)
                        m_context.openLocation(*path, c.location->line);
                }
        });
        auto* watch = new QTimer(this);
        watch->setInterval(1000);
        connect(watch, &QTimer::timeout, this, [this] {
            if (m_source && !current())
                invalidate();
            actions();
        });
        watch->start();
        actions();
    }
    ~LuaTestsWindow() override { m_runner.reset(); }
    std::vector<run::GutterTestCase> gutter(std::string_view path) const {
        std::vector<run::GutterTestCase> out;
        if (!current())
            return out;
        auto requested = io::normalizePath(path);
        if (!requested)
            return out;
        auto parent = parentTest();
        for (const auto& c : m_cases)
            if (c.location) {
                auto full = io::isAbsolutePath(c.location->file) ? Result<std::string>(c.location->file)
                                                                 : io::joinPath(workingDirectory(), c.location->file);
                if (!full)
                    continue;
                auto normalized = io::normalizePath(*full);
                if (normalized &&
                    QString::compare(qs(*normalized), qs(*requested),
                                     io::kNativePathStyle == io::PathStyle::Windows ? Qt::CaseInsensitive
                                                                                    : Qt::CaseSensitive) == 0)
                    out.push_back({parent, c.name,
                                   profile() == run::LuaTestProfile::Busted220 ? run::TestCaseFramework::Busted
                                                                               : run::TestCaseFramework::LuaUnit,
                                   c.location->line, c.name});
            }
        return out;
    }
    Result<void> gutterRun(const run::GutterTestCase& c, bool debug) {
        if (!current() || c.test != parentTest() ||
            c.framework != (profile() == run::LuaTestProfile::Busted220 ? run::TestCaseFramework::Busted
                                                                        : run::TestCaseFramework::LuaUnit))
            return fail(ErrorCode::InvalidArgument, "Lua gutter selection is stale");
        return run({c.caseName}, debug);
    }

private:
    run::LuaTestProfile profile() const {
        return m_profile->currentIndex() == 0 ? run::LuaTestProfile::Busted220 : run::LuaTestProfile::LuaUnit35;
    }
    bool current() const {
        if (!m_source || m_source->lifetime.stop_requested() || profile() != m_listedProfile || !m_context.source ||
            !m_context.trust)
            return false;
        auto now = m_context.source();
        if (!now || now->lifetime.stop_requested())
            return false;
        auto trust = m_context.trust->snapshot(m_source->trustRoot);
        return trust.trusted && trust.revision == m_source->trustRevision && now->trustRoot == m_source->trustRoot &&
               now->trustRevision == m_source->trustRevision && now->configuration == m_source->configuration;
    }
    std::string workingDirectory() const {
        auto plan = run::launchPlanFor(m_source->configuration);
        return plan ? plan->workingDirectory : std::string{};
    }
    run::DiscoveredTest parentTest() const {
        run::DiscoveredTest test;
        if (!m_source)
            return test;
        test.name = m_source->configuration.name;
        test.workingDirectory = workingDirectory();
        auto plan = run::launchPlanFor(m_source->configuration);
        if (plan) {
            test.command = {plan->program};
            test.command.insert(test.command.end(), plan->arguments.begin(), plan->arguments.end());
        }
        return test;
    }
    run::TestRunner::LuaFrameworkRequest request(std::vector<std::string> cases = {}) const {
        return {m_source->configuration, profile(), m_context.scratchDirectory, m_context.discoveryBridge,
                std::move(cases)};
    }
    void invalidate() {
        ++m_generation;
        if (m_runner)
            m_runner->cancel();
        m_source.reset();
        m_cases.clear();
        m_tree->setTests({});
        m_status->setText(QStringLiteral("Lua source, profile or trust changed. Discover again."));
        if (m_context.testsChanged)
            m_context.testsChanged();
    }
    void actions() {
        bool busy = m_runner && m_runner->busy();
        bool ready = current() && !busy;
        m_profile->setEnabled(!busy);
        m_refresh->setEnabled(!busy);
        m_all->setEnabled(ready && !m_cases.empty());
        auto names = selection();
        m_selected->setEnabled(ready && !names.empty());
        m_debug->setEnabled(ready && names.size() == 1 && static_cast<bool>(m_context.debug));
        m_failed->setEnabled(ready && !m_tree->failedTestNames().empty());
        m_stop->setEnabled(busy);
    }
    std::vector<std::string> selection() const {
        std::vector<std::string> out;
        for (const auto& [parent, name] : m_tree->selectedCases()) {
            static_cast<void>(parent);
            out.push_back(name);
        }
        return out;
    }
    void showDetails() {
        if (!m_source)
            return;
        const auto* result = m_tree->result(m_source->configuration.name);
        if (!result)
            return;
        std::string details = result->output;
        for (const auto& c : result->cases)
            if (c.outcome != run::TestOutcome::Passed)
                details += '\n' + c.name + ": " + c.reason;
        m_output->setPlainText(qs(details));
    }
    void refresh() {
        if (m_runner && m_runner->busy())
            return;
        invalidate();
        m_runner.reset();
        m_launcher.reset();
        if (!m_context.fs || !m_context.launcher || !m_context.trust || !m_context.source)
            return;
        auto source = m_context.source();
        if (!source) {
            m_status->setText(qs(source.error().message()));
            return;
        }
        const QPointer<LuaTestsWindow> owner(this);
        const auto config = source->configuration;
        if (m_context.prepare) {
            auto prepare = m_context.prepare;
            auto allowed = prepare(config);
            if (!owner)
                return;
            if (!allowed) {
                m_status->setText(qs(allowed.error().message()));
                return;
            }
        }
        if (source->lifetime.stop_requested())
            return;
        m_source = std::move(*source);
        m_listedProfile = profile();
        if (!current()) {
            invalidate();
            return;
        }
        m_launcher = std::make_unique<TrustedProjectLauncher>(
            *m_context.launcher, *m_context.trust, m_source->trustRoot, m_source->trustRevision, m_source->lifetime);
        m_runner = run::TestRunner::create(*m_launcher, *m_context.fs);
        const auto generation = m_generation;
        auto started =
            m_runner->listLuaFramework(request(), [owner, generation](Result<std::vector<run::FrameworkCase>> cases) {
                if (!owner)
                    return;
                QMetaObject::invokeMethod(
                    owner,
                    [owner, generation, cases = std::move(cases)]() mutable {
                        if (!owner || generation != owner->m_generation || !owner->current())
                            return;
                        if (!cases)
                            owner->m_status->setText(qs(cases.error().message()));
                        else {
                            owner->m_cases = std::move(*cases);
                            auto test = owner->parentTest();
                            owner->m_tree->setTests({test});
                            owner->m_tree->setFrameworkCases(test.name, owner->m_cases);
                            owner->m_status->setText(
                                QStringLiteral("Discovered %1 real framework cases").arg(owner->m_cases.size()));
                            if (owner->m_context.testsChanged)
                                owner->m_context.testsChanged();
                        }
                        owner->actions();
                    },
                    Qt::QueuedConnection);
            });
        if (!started)
            m_status->setText(qs(started.error().message()));
        actions();
    }
    Result<void> run(std::vector<std::string> names, bool debug) {
        auto failed = [this](ErrorCode code, std::string text) -> Result<void> {
            m_status->setText(qs(text));
            return fail(code, std::move(text));
        };
        if (!current() || !m_runner || m_runner->busy())
            return failed(ErrorCode::Busy, "Lua discovery is stale or a job is active");
        if (names.empty())
            for (const auto& c : m_cases)
                names.push_back(c.name);
        if (names.empty())
            return failed(ErrorCode::NotFound, "The framework listed no tests");
        for (const auto& name : names)
            if (!std::ranges::any_of(m_cases, [&](const auto& c) { return c.name == name; }))
                return failed(ErrorCode::InvalidArgument, "Selected Lua case was not discovered");
        auto config = m_source->configuration;
        const QPointer<LuaTestsWindow> owner(this);
        if (m_context.prepare) {
            auto prepare = m_context.prepare;
            auto admitted = prepare(config);
            if (!owner)
                return fail(ErrorCode::Cancelled, "Lua provider closed while saving");
            if (!admitted)
                return failed(admitted.error().code(), admitted.error().message());
        }
        if (!current())
            return failed(ErrorCode::Cancelled, "Lua configuration changed while saving");
        if (debug) {
            if (names.size() != 1 || !m_context.debug)
                return failed(ErrorCode::Unsupported, "Select one Lua case to debug");
            auto planned = run::luaFrameworkConfiguration(profile(), std::move(config), names, {}, false, {});
            if (!planned)
                return failed(planned.error().code(), planned.error().message());
            auto launch = m_context.debug;
            auto result = launch(std::move(*planned));
            if (!owner)
                return result;
            if (!result)
                m_status->setText(qs(result.error().message()));
            return result;
        }
        m_tree->markPending({config.name});
        m_output->clear();
        const auto generation = m_generation;
        run::TestRunHandlers handlers;
        handlers.onOutput = [owner, generation](std::string_view text) {
            if (!owner)
                return;
            QMetaObject::invokeMethod(
                owner,
                [owner, generation, text = std::string(text)] {
                    if (owner && generation == owner->m_generation && owner->current())
                        owner->m_output->insertPlainText(qs(text));
                },
                Qt::QueuedConnection);
        };
        handlers.onFinished = [owner, generation](const run::TestRunReport& report) {
            if (!owner)
                return;
            QMetaObject::invokeMethod(
                owner,
                [owner, generation, report]() mutable {
                    if (!owner || generation != owner->m_generation || !owner->current())
                        return;
                    for (auto& parent : report.results)
                        for (auto& c : parent.cases)
                            for (const auto& listed : owner->m_cases)
                                if (c.name == listed.name && listed.location) {
                                    if (!c.definition)
                                        c.definition = listed.location;
                                    for (auto& failure : c.failures)
                                        if (!failure.location)
                                            failure.location = listed.location;
                                }
                    owner->m_tree->finishRun(report);
                    owner->m_status->setText(report.error     ? qs(report.error->message())
                                             : report.stopped ? QStringLiteral("Stopped")
                                                              : QStringLiteral("Framework report received"));
                    owner->actions();
                },
                Qt::QueuedConnection);
        };
        auto started = m_runner->runLuaFramework(request(std::move(names)), std::move(handlers));
        if (!started)
            return failed(started.error().code(), started.error().message());
        actions();
        return {};
    }
    LuaUnitTestsContext m_context;
    std::unique_ptr<TrustedProjectLauncher> m_launcher;
    std::unique_ptr<run::TestRunner> m_runner;
    std::optional<LuaUnitTestsSource> m_source;
    run::LuaTestProfile m_listedProfile = run::LuaTestProfile::Busted220;
    std::uint64_t m_generation{};
    std::vector<run::FrameworkCase> m_cases;
    QComboBox* m_profile{};
    QPushButton *m_refresh{}, *m_all{}, *m_selected{}, *m_failed{}, *m_debug{}, *m_stop{};
    QLabel* m_status{};
    ui::TestResultsTree* m_tree{};
    QPlainTextEdit* m_output{};
};
LuaTestsWindow* provider(QWidget* widget) {
    if (auto* direct = dynamic_cast<LuaTestsWindow*>(widget))
        return direct;
    auto* tabs = qobject_cast<QTabWidget*>(widget);
    if (!tabs)
        return nullptr;
    LuaTestsWindow* found{};
    for (int i = 0; i < tabs->count(); ++i)
        if (auto* item = dynamic_cast<LuaTestsWindow*>(tabs->widget(i))) {
            if (found)
                return nullptr;
            found = item;
        }
    return found;
}
} // namespace
QWidget* createLuaUnitTestsWidget(QWidget* parent, LuaUnitTestsContext context) {
    return new LuaTestsWindow(parent, std::move(context));
}
std::vector<run::GutterTestCase> luaUnitTestsGutterCases(QWidget* widget, std::string_view path) {
    auto* p = provider(widget);
    return p ? p->gutter(path) : std::vector<run::GutterTestCase>{};
}
Result<void> runLuaGutterCase(QWidget* widget, const run::GutterTestCase& c, bool debug) {
    auto* p = provider(widget);
    if (!p)
        return fail(ErrorCode::NotFound, "No unambiguous Lua tests provider");
    return p->gutterRun(c, debug);
}
} // namespace frost::app
