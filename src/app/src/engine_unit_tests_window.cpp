// SPDX-License-Identifier: Apache-2.0
// FER-10: GUI view over engine-owned scene discovery and runtime-only case results.
#include "engine_unit_tests_window.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <mutex>
#include <thread>

namespace frost::app {
namespace {
using namespace engine;
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
std::string key(const EngineUnitTestsSource& s) {
    return rpc::Json::array({s.options.program, s.options.project, s.options.expectedProjectRoot,
                             s.options.expectedProjectFile.value_or(""), s.options.folder, rpc::Json(s.options.frames),
                             rpc::Json(s.revision)})
        .dump();
}
QString measured(const std::optional<std::chrono::microseconds>& d) {
    return d ? QString::number(static_cast<double>(d->count()) / 1000.0, 'f', 3) + QStringLiteral(" ms")
             : QString::fromUtf8("—");
}
constexpr int kId = Qt::UserRole, kCase = Qt::UserRole + 1, kPath = Qt::UserRole + 2, kLine = Qt::UserRole + 3;
// Serializes enqueue against GUI destruction. No filesystem or worker wait occurs under the lock;
// QObject removes already queued deliveries when destroyed. Workers never dereference the widget.
struct EngineTestsRelay {
    std::mutex mutex;
    QObject* target = nullptr;
    void post(std::function<void()> callback) {
        const std::lock_guard lock(mutex);
        if (target)
            QMetaObject::invokeMethod(target, std::move(callback), Qt::QueuedConnection);
    }
};
std::function<bool()> workerAdmission(const EngineUnitTestsSource& source) {
    return [logical = source.admitted, physical = source.validatePhysical] {
        return logical && logical() && (!physical || physical().has_value()) && logical();
    };
}
class EngineTestsWidget final : public QWidget {
public:
    EngineTestsWidget(QWidget* parent, EngineUnitTestsWindowContext context)
        : QWidget(parent), m_context(std::move(context)) {
        m_relay->target = this;
        setObjectName(QStringLiteral("engineUnitTestsProvider"));
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(
            new QLabel(QStringLiteral("FrostEngine scene tests — Test.Case results appear after a run."), this));
        auto* bar = new QHBoxLayout;
        layout->addLayout(bar);
        const auto button = [&](const char* name, const QString& label) {
            auto* b = new QPushButton(label, this);
            b->setObjectName(QLatin1String(name));
            bar->addWidget(b);
            return b;
        };
        m_all = button("engineTestsRunAll", QStringLiteral("Run All Scenes"));
        m_selected = button("engineTestsRunSelected", QStringLiteral("Run Selected Scene"));
        m_failed = button("engineTestsRunFailed", QStringLiteral("Run Failed Scenes"));
        m_stop = button("engineTestsStop", QStringLiteral("Stop"));
        m_refresh = button("engineTestsRefresh", QStringLiteral("Refresh"));
        m_debug = button("engineTestsDebug", QStringLiteral("Debug Selected Scene"));
        m_summary = new QLabel(this);
        m_summary->setObjectName(QStringLiteral("engineTestsSummary"));
        m_summary->setWordWrap(true);
        layout->addWidget(m_summary);
        m_tree = new QTreeWidget(this);
        m_tree->setObjectName(QStringLiteral("engineTestsTree"));
        m_tree->setHeaderLabels({QStringLiteral("Scene / runtime case"), QStringLiteral("Actual outcome"),
                                 QStringLiteral("Measured duration")});
        m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(m_tree, 1);
        m_details = new QPlainTextEdit(this);
        m_details->setObjectName(QStringLiteral("engineTestsDetails"));
        m_details->setReadOnly(true);
        m_details->setMaximumHeight(180);
        layout->addWidget(m_details);
        connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
        connect(m_all, &QPushButton::clicked, this, [this] { run({}); });
        connect(m_selected, &QPushButton::clicked, this, [this] {
            if (auto* i = selectedScene())
                run({i->data(0, kId).toString().toStdString()});
        });
        connect(m_failed, &QPushButton::clicked, this, [this] {
            auto ids = failedEngineSceneTests(m_listing, m_results);
            if (!ids.empty())
                run(std::move(ids));
        });
        connect(m_stop, &QPushButton::clicked, this, [this] {
            m_worker.request_stop();
            m_summary->setText(QStringLiteral("Stopping the owned engine test process…"));
        });
        connect(m_debug, &QPushButton::clicked, this, [this] {
            if (!m_context.debugScene)
                return;
            auto* i = selectedScene();
            if (!i)
                return;
            auto source = current();
            if (!source)
                return;
            if (key(*source) != m_key) {
                refresh();
                return;
            }
            for (const auto& t : m_listing.tests)
                if (t.id == i->data(0, kId).toString().toStdString()) {
                    m_context.debugScene(source->options, t);
                    break;
                }
        });
        connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
            details();
            actions();
        });
        connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) { open(item); });
        auto* watch = new QTimer(this);
        watch->setInterval(1000);
        connect(watch, &QTimer::timeout, this, [this] {
            auto source = current();
            const auto next = source ? key(*source) : std::string{};
            if (next != m_key) {
                ++m_generation;
                m_key = next;
                m_worker.request_stop();
                m_listing = {};
                m_results.clear();
                render();
                m_summary->setText(source ? QStringLiteral("Project source changed; refreshing scene tests…")
                                          : qs(source.error().message()));
                if (!m_busy && source)
                    refresh();
            }
        });
        watch->start();
        refresh();
    }
    ~EngineTestsWidget() override {
        {
            const std::lock_guard lock(m_relay->mutex);
            m_relay->target = nullptr;
        }
        retireWorker();
    }

private:
    void retireWorker() {
        m_worker.request_stop();
        if (m_worker.joinable())
            m_context.retirements->retire(std::move(m_worker));
    }
    Result<EngineUnitTestsSource> current() const {
        if (!m_context.fs || !m_context.launcher || !m_context.source || !m_context.retirements)
            return fail(ErrorCode::NotFound, "FrostEngine test provider services unavailable");
        auto source = m_context.source();
        if (!source)
            return source;
        if (!source->admitted || !source->admitted())
            return fail(ErrorCode::Untrusted, "Trust the current engine test project before listing or running tests");
        return source;
    }
    QTreeWidgetItem* selectedScene() const {
        auto* i = m_tree->currentItem();
        return i && !i->data(0, kCase).toBool() ? i : nullptr;
    }
    void actions() {
        const bool ready = !m_busy && !m_listing.tests.empty() && !m_key.empty();
        m_all->setEnabled(ready);
        m_selected->setEnabled(ready && selectedScene());
        m_failed->setEnabled(ready && !failedEngineSceneTests(m_listing, m_results).empty());
        m_stop->setEnabled(m_busy);
        m_refresh->setEnabled(!m_busy);
        m_debug->setEnabled(ready && selectedScene() && static_cast<bool>(m_context.debugScene));
        const bool child = m_tree->currentItem() && m_tree->currentItem()->data(0, kCase).toBool();
        const auto unavailable =
            child ? QStringLiteral("The engine selects scenes, not individual runtime Test.Case entries.")
                  : QStringLiteral("Select an engine scene test.");
        m_selected->setToolTip(unavailable);
        m_debug->setToolTip(m_context.debugScene
                                ? unavailable
                                : QStringLiteral("Engine test debugging is unavailable for this source."));
    }
    void refresh() {
        if (m_busy)
            return;
        auto source = current();
        if (!source) {
            m_summary->setText(qs(source.error().message()));
            m_key.clear();
            m_listing = {};
            m_results.clear();
            render();
            return;
        }
        m_key = key(*source);
        const auto generation = ++m_generation;
        m_busy = true;
        actions();
        m_summary->setText(QStringLiteral("Listing actual engine scene tests…"));
        retireWorker();
        auto* fs = m_context.fs;
        auto* launcher = m_context.launcher;
        const auto relay = m_relay;
        m_worker =
            std::jthread([this, relay, fs, launcher, source = std::move(*source), generation](std::stop_token stop) {
                const auto physical = source.validatePhysical ? source.validatePhysical() : Result<void>{};
                auto result =
                    physical ? discoverEngineSceneTests(*fs, *launcher, source.options, workerAdmission(source), stop)
                             : Result<EngineTestListing>(std::unexpected(physical.error()));
                relay->post([this, result = std::move(result), generation]() mutable {
                    m_busy = false;
                    const auto now = current();
                    if (generation != m_generation || !now || key(*now) != m_key) {
                        actions();
                        if (now)
                            refresh();
                        return;
                    }
                    if (result) {
                        m_listing = std::move(*result);
                        m_results.clear();
                        m_summary->setText(
                            QStringLiteral("%1 scene tests listed by FrostEngine.").arg(m_listing.tests.size()));
                    } else {
                        m_listing = {};
                        m_results.clear();
                        m_summary->setText(qs(result.error().message()));
                    }
                    render();
                });
            });
    }
    void run(std::vector<std::string> ids) {
        if (m_busy)
            return;
        auto source = current();
        if (!source) {
            m_summary->setText(qs(source.error().message()));
            return;
        }
        if (key(*source) != m_key) {
            refresh();
            return;
        }
        m_busy = true;
        actions();
        const auto generation = m_generation;
        m_summary->setText(QStringLiteral("Running actual engine scene tests…"));
        retireWorker();
        auto* fs = m_context.fs;
        auto* launcher = m_context.launcher;
        auto listing = m_listing;
        // Forget only selected old outcomes. Completed unselected results remain real previous reports.
        if (ids.empty())
            m_results.clear();
        else
            std::erase_if(m_results, [&](const auto& r) { return std::ranges::find(ids, r.scene) != ids.end(); });
        render();
        const auto relay = m_relay;
        m_worker = std::jthread([this, relay, fs, launcher, source = std::move(*source), listing = std::move(listing),
                                 ids = std::move(ids), generation](std::stop_token stop) {
            EngineTestExecution result;
            const auto physical = source.validatePhysical ? source.validatePhysical() : Result<void>{};
            if (physical)
                result =
                    runEngineSceneTests(*fs, *launcher, source.options, listing, ids, workerAdmission(source), stop);
            else
                result.error = physical.error();
            relay->post([this, result = std::move(result), generation]() mutable {
                m_busy = false;
                const auto now = current();
                if (generation != m_generation || !now || key(*now) != m_key) {
                    actions();
                    if (now)
                        refresh();
                    return;
                }
                for (auto& r : result.results) {
                    std::erase_if(m_results, [&](const auto& previous) { return previous.scene == r.scene; });
                    m_results.push_back(std::move(r));
                }
                if (result.error)
                    m_summary->setText((result.cancelled ? QStringLiteral("Cancelled. ") : QString{}) +
                                       qs(result.error->message()));
                else
                    m_summary->setText(
                        QStringLiteral("%1 complete actual scene reports retained.").arg(m_results.size()));
                render();
            });
        });
    }
    void render() {
        m_tree->clear();
        for (const auto& t : m_listing.tests) {
            auto* i = new QTreeWidgetItem(m_tree, {qs(t.scene), QStringLiteral("Not run"), measured({})});
            i->setData(0, kId, qs(t.id));
            for (const auto& r : m_results)
                if (r.scene == t.scene) {
                    i->setText(1, qs(toString(r.outcome)));
                    i->setText(2, measured(r.duration));
                    for (const auto& c : r.cases) {
                        auto* child = new QTreeWidgetItem(i, {qs(c.peer.empty() ? c.name : c.peer + ": " + c.name),
                                                              qs(toString(c.outcome)), measured(c.duration)});
                        child->setData(0, kId, qs(t.id));
                        child->setToolTip(0, qs(c.skipReason));
                        child->setData(0, kCase, true);
                        if (c.definition) {
                            child->setData(0, kPath, qs(c.definition->path));
                            child->setData(0, kLine, QVariant::fromValue<qulonglong>(c.definition->line));
                        }
                    }
                    for (const auto& f : r.failures) {
                        auto* child =
                            new QTreeWidgetItem(i, {qs(f.message), QStringLiteral("Reported failure"), measured({})});
                        child->setData(0, kId, qs(t.id));
                        child->setData(0, kCase, true);
                        if (f.location) {
                            child->setData(0, kPath, qs(f.location->path));
                            child->setData(0, kLine, QVariant::fromValue<qulonglong>(f.location->line));
                        }
                    }
                }
        }
        actions();
        details();
    }
    void details() {
        auto* i = m_tree->currentItem();
        if (!i) {
            m_details->clear();
            return;
        }
        QStringList lines;
        if (i->data(0, kCase).toBool())
            lines << QStringLiteral(
                "Runtime entry. Individual case selection is unavailable; select its scene explicitly to rerun.");
        const auto id = i->data(0, kId).toString().toStdString();
        for (const auto& r : m_results)
            if (r.scene == id) {
                if (!r.exitName.empty())
                    lines << QStringLiteral("Exit: ") + qs(r.exitName);
                if (!r.exitReason.empty())
                    lines << QStringLiteral("Reason: ") + qs(r.exitReason);
                for (const auto& log : r.logTail)
                    lines << qs("[" + log.level + "] " + log.logger + ": " + log.message);
                for (const auto& line : r.report)
                    lines << qs(line);
                for (const auto& f : r.failures)
                    lines << qs(f.peer + " " + f.caseName + ": " + f.message + " " + f.rawLocation);
            }
        m_details->setPlainText(lines.join('\n'));
    }
    void open(QTreeWidgetItem* i) {
        if (m_busy || !i || !m_context.openLocation)
            return;
        auto source = current();
        if (!source || key(*source) != m_key)
            return;
        const auto line = i->data(0, kLine).toULongLong();
        const auto path = i->data(0, kPath).toString().toStdString();
        if (path.empty() || line == 0 || line > static_cast<qulonglong>(std::numeric_limits<int>::max()))
            return;
        m_busy = true;
        actions();
        const auto generation = m_generation;
        auto* fs = m_context.fs;
        retireWorker();
        const auto relay = m_relay;
        m_worker = std::jthread([this, relay, fs, source = std::move(*source),
                                 location = EngineTestLocation{path, line}, generation](std::stop_token stop) {
            const auto physical = source.validatePhysical ? source.validatePhysical() : Result<void>{};
            auto resolved = physical ? resolveEngineTestLocation(*fs, source.options.expectedProjectRoot, location)
                                     : Result<std::string>(std::unexpected(physical.error()));
            if (stop.stop_requested() || !workerAdmission(source)())
                resolved = fail(ErrorCode::Cancelled, "engine test source changed during navigation");
            relay->post([this, resolved = std::move(resolved), generation, line]() mutable {
                m_busy = false;
                const auto now = current();
                if (generation == m_generation && now && key(*now) == m_key) {
                    if (resolved)
                        m_context.openLocation(*resolved, static_cast<int>(line));
                    else
                        m_summary->setText(qs(resolved.error().message()));
                }
                actions();
            });
        });
    }
    EngineUnitTestsWindowContext m_context;
    EngineTestListing m_listing;
    std::vector<EngineSceneTestResult> m_results;
    std::string m_key;
    std::uint64_t m_generation = 0;
    bool m_busy = false;
    std::jthread m_worker;
    std::shared_ptr<EngineTestsRelay> m_relay = std::make_shared<EngineTestsRelay>();
    QPushButton *m_all = nullptr, *m_selected = nullptr, *m_failed = nullptr, *m_stop = nullptr, *m_refresh = nullptr,
                *m_debug = nullptr;
    QLabel* m_summary = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPlainTextEdit* m_details = nullptr;
};
} // namespace
QWidget* createEngineUnitTestsProviderWidget(QWidget* parent, EngineUnitTestsWindowContext context) {
    return new EngineTestsWidget(parent, std::move(context));
}
} // namespace frost::app
