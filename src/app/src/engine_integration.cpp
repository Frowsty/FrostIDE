// SPDX-License-Identifier: Apache-2.0
// Explicit, trusted commands to a running FrostEngine editor, and the FrostEngine tool window that
// carries them (registered only for FrostEngine projects: lang::isFrostEngineProjectRoot, a
// .frostproj or .feproject file in the project folder).
#include "engine_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/engine_discovery_process.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <optional>
#include <stop_token>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::app {
namespace {
constexpr std::string_view kToolWindow = "frostEngine";
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QString discoveryDirectory() {
    QString path = qEnvironmentVariable("FROST_ENGINE_AUTOMATION_DIR");
    if (!path.isEmpty())
        return path;
    path = qEnvironmentVariable("FROST_EDITOR_CONFIG_DIR");
    if (path.isEmpty()) {
#ifdef _WIN32
        path = qEnvironmentVariable("APPDATA");
        if (path.isEmpty())
            path = QDir(qEnvironmentVariable("USERPROFILE")).filePath(QStringLiteral("AppData/Roaming"));
#else
        path = qEnvironmentVariable("XDG_CONFIG_HOME");
        if (!QDir::isAbsolutePath(path))
            path = QDir::home().filePath(QStringLiteral(".config"));
#endif
        path = QDir(path).filePath(QStringLiteral("FrostEngine/Editor"));
    }
    return QDir(path).filePath(QStringLiteral("Automation"));
}
/*
 * How long the parent waits for the helper process. The engine conversation itself is bounded by
 * lang::engineMethodTimeout inside the helper; this adds the helper's own start-up and discovery
 * time. A method without a deadline (test.run) gets no practical bound here either: only the
 * user's Cancel, a project switch or shutdown ends it (the stop token destroys the helper).
 */
std::chrono::milliseconds helperTimeout(std::string_view method) {
    using namespace std::chrono_literals;
    if (const auto engine = lang::engineMethodTimeout(method))
        return *engine + 10s;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24 * 365));
}
struct RequestState {
    std::atomic<bool> active{true};
    std::stop_source stop;
};
struct Request {
    std::string root, directory, method, executable, trustPath;
    lang::Json params;
    std::optional<text::Snapshot> source;
    io::IProcessLauncher* launcher = nullptr;
    sched::IScheduler* scheduler = nullptr;
    std::shared_ptr<io::Reactor> reactor;
    project::TrustStore* trust = nullptr;
    std::shared_ptr<RequestState> state;
};
using Completion = std::function<void(Result<lang::Json>)>;
sched::Task<void> execute(Request request, Completion completion) {
    const auto trusted = [&] { return request.state->active.load() && request.trust->isTrusted(request.root); };
    Result<lang::Json> result = fail(ErrorCode::Untrusted, "project is not trusted");
    if (trusted()) {
        if (request.source)
            request.params.set("source", request.source->text());
        const auto timeout = helperTimeout(request.method);
        auto input = lang::Json::object()
                         .set("root", request.root)
                         .set("directory", request.directory)
                         .set("trustPath", request.trustPath)
                         .set("method", request.method)
                         .set("params", std::move(request.params));
        if (trusted())
            result = co_await lang::requestEngineInHelper(*request.launcher, {*request.reactor, *request.scheduler},
                                                          request.executable, std::move(input), timeout,
                                                          request.state->stop.get_token());
    }
    completion(std::move(result));
    co_return;
}
} // namespace

struct EngineIntegration::Impl {
    EngineIntegration& owner;
    Workbench& workbench;
    QPointer<QWidget> panel;
    QPointer<QLabel> status;
    QPointer<QListWidget> output;
    std::vector<QPointer<QPushButton>> actions;
    std::shared_ptr<RequestState> current;
    std::shared_ptr<io::Reactor> reactor;
    struct Running {
        std::shared_ptr<RequestState> state;
        sched::JoinHandle<void> task;
    };
    std::vector<Running> tasks;
    bool stopped = false;
    bool registered = false;
    bool available = false;        // The open project is a FrostEngine project.
    bool stripeHidden = false;     // projectChanged hid the stripe button for a non-engine project.
    bool stripeWasVisible = false; // ...and whether it was showing before.
    std::int64_t since = 0;
    std::size_t outputBytes = 0;
    bool evicted = false;

    void cancel(bool resetLog = true) {
        if (current) {
            current->active.store(false);
            current->stop.request_stop();
            current.reset();
        }
        if (resetLog)
            since = 0;
        if (resetLog && output) {
            output->clear();
            outputBytes = 0;
            evicted = false;
        }
        if (status)
            status->setText(available ? QStringLiteral("No request running")
                                      : QStringLiteral("FrostEngine automation needs a FrostEngine project "
                                                       "(a .frostproj or .feproject file in the project folder)"));
    }

    // Registers the tool window the first time a FrostEngine project opens; for any other project
    // the window is hidden, its stripe button removed and its commands disabled.
    void projectChanged() {
        if (stopped || !workbench.m_window)
            return;
        cancel();
        // The one FrostEngine project rule, shared with the LuaLS defaults (lua_service.hpp).
        available = lang::isFrostEngineProjectRoot(workbench.m_services.fs, workbench.projectRoot());
        auto& windows = workbench.m_window->toolWindows();
        if (available && !registered) {
            QWidget* content = panel;
            auto added = windows.add({std::string(kToolWindow),
                                      QStringLiteral("FrostEngine"),
                                      ui::ToolWindowSide::Bottom,
                                      std::string(ui::icons::kRunRun),
                                      {},
                                      [content] { return content; },
                                      240});
            registered = static_cast<bool>(added);
        }
        // The manager cannot unregister a window: for another project the window is hidden and its
        // stripe button too (restored as it was when a FrostEngine project opens again). The More
        // menu still lists it; it then shows the "needs a FrostEngine project" state below.
        if (registered && !available) {
            windows.hide(kToolWindow);
            if (auto* button = windows.stripeButton(kToolWindow); button && !stripeHidden) {
                stripeWasVisible = button->isVisibleTo(button->parentWidget());
                stripeHidden = true;
                button->setVisible(false);
            }
        } else if (registered && stripeHidden) {
            if (auto* button = windows.stripeButton(kToolWindow))
                button->setVisible(stripeWasVisible);
            stripeHidden = false;
        }
        for (const auto& action : actions)
            if (action)
                action->setEnabled(available);
        cancel(); // Refreshes the status text for the new availability.
    }

    QListWidgetItem* append(std::string_view text) {
        const QString shown = qs(text.substr(0, 8192));
        const auto size = static_cast<std::size_t>(shown.toUtf8().size());
        while (output->count() && (output->count() >= 10000 || outputBytes + size > 4 * 1024 * 1024)) {
            auto* oldest = output->takeItem(0);
            outputBytes -= static_cast<std::size_t>(oldest->text().toUtf8().size());
            delete oldest;
            evicted = true;
        }
        outputBytes += size;
        return new QListWidgetItem(shown, output);
    }
    void showItems(const lang::Json& value, const lang::Json& locations) {
        if (value.isArray()) {
            for (const auto& item : value.items()) {
                if (const auto* text = item.asString())
                    append(*text);
                else
                    showItems(item, locations);
            }
            return;
        }
        if (!value.isObject())
            return;
        const auto message = value.stringAt("message");
        if (!message.empty()) {
            auto* item = append(message);
            std::string_view path = value.stringAt("path");
            auto line = value.integerAt("line");
            if (const auto* source = value.find("source")) {
                path = source->stringAt("path");
                line = source->integerAt("line");
            }
            const auto resolved = locations.stringAt(path);
            if (line && *line > 0 && *line <= INT_MAX && !resolved.empty()) {
                item->setData(Qt::UserRole, qs(path));
                item->setData(Qt::UserRole + 1, static_cast<int>(*line));
                item->setData(Qt::UserRole + 2, qs(workbench.projectRoot()));
            }
        }
        for (const auto& child : value.items())
            showItems(child, locations);
    }
    bool run(std::string method, std::string location = {}, int line = 0) {
        if (stopped || workbench.m_shutDown)
            return false;
        std::erase_if(tasks, [](Running& running) { return running.task.ready(); });
        if (!available) {
            cancel(false);
            return false;
        }
        if (current) {
            status->setText(QStringLiteral("A FrostEngine request is already running"));
            return false;
        }
        const std::string root = workbench.projectRoot();
        if (root.empty() || !workbench.m_services.trust.isTrusted(root)) {
            status->setText(QStringLiteral("Trust the project before using FrostEngine automation"));
            return false;
        }
        const QString directory = discoveryDirectory();
        if (!io::isAbsolutePath(root) || !QDir::isAbsolutePath(directory)) {
            status->setText(QStringLiteral("Cannot locate the project or automation directory"));
            return false;
        }
        lang::Json params = lang::Json::object();
        std::optional<text::Snapshot> source;
        if (method == "log.get")
            params.set("since", since);
        if (method == "navigation.resolve")
            params.set("path", std::move(location));
        if (method == "script.validate") {
            auto* view = workbench.currentView();
            if (!view || !qs(view->path()).endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) {
                status->setText(QStringLiteral("Open a Lua script to validate"));
                return false;
            }
            const QString relative = QDir(qs(root)).relativeFilePath(qs(view->path()));
            if (!relative.startsWith(QStringLiteral("Assets/")) || relative.split('/').contains(QStringLiteral(".."))) {
                status->setText(QStringLiteral("The script must be inside this project's Assets folder"));
                return false;
            }
            params.set("path", utf8(relative));
            source = view->document().document().text();
        }
#ifdef _WIN32
        if (method == "editor.focus")
            static_cast<void>(AllowSetForegroundWindow(ASFW_ANY));
#endif
        if (!reactor) {
            auto created = io::Reactor::create(io::ReactorOptions{1, "frost-engine-io"});
            if (!created) {
                status->setText(QStringLiteral("Cannot start the FrostEngine I/O service"));
                return false;
            }
            reactor = std::shared_ptr<io::Reactor>(std::move(*created));
        }
        current = std::make_shared<RequestState>();
        const auto state = current;
        Request request{root,
                        utf8(directory),
                        method,
                        workbench.m_options.engineDiscoveryHelperPath.empty()
                            ? utf8(QCoreApplication::applicationFilePath())
                            : workbench.m_options.engineDiscoveryHelperPath,
                        workbench.m_services.trust.storePath(),
                        std::move(params),
                        std::move(source),
                        &workbench.m_services.launcher,
                        workbench.m_pool.get(),
                        reactor,
                        &workbench.m_services.trust,
                        state};
        status->setText(method == "test.run" ? QStringLiteral("Running FrostEngine tests... (Cancel Request stops "
                                                              "waiting)")
                                             : QStringLiteral("Contacting FrostEngine..."));
        QPointer<EngineIntegration> receiver(&owner);
        auto completion = [receiver, state, root, method, line](Result<lang::Json> result) mutable {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [receiver, state, root, method, line, result = std::move(result)]() mutable {
                    if (!receiver)
                        return;
                    auto& impl = *receiver->m_impl;
                    if (!state->active.load() || impl.current != state || impl.workbench.projectRoot() != root)
                        return;
                    impl.current.reset();
                    if (!result) {
                        const auto code = result.error().code();
                        if (code == ErrorCode::Timeout)
                            impl.status->setText(
                                QStringLiteral("FrostEngine did not answer in time; the engine may still be working"));
                        else if (code == ErrorCode::Cancelled)
                            impl.status->setText(
                                QStringLiteral("Request was cancelled; the engine may still be working"));
                        else // The engine's own code and (token-redacted) message, or the local reason.
                            impl.status->setText(qs(result.error().message()));
                        return;
                    }
                    auto* payload = result->find("result");
                    const auto* locations = result->find("locations");
                    if (!payload || !payload->isObject() || !locations || !locations->isObject()) {
                        impl.status->setText(QStringLiteral("Invalid FrostEngine response"));
                        return;
                    }
                    if (method == "navigation.resolve") {
                        const auto path = payload->stringAt("path");
                        if (!path.empty())
                            static_cast<void>(impl.workbench.openPath(path, {line, 1}));
                        return;
                    }
                    impl.status->setText(method == "editor.focus" && payload->boolAt("focused") == false
                                             ? QStringLiteral("The editor could not take focus")
                                             : QStringLiteral("FrostEngine request complete"));
                    if (method == "log.get")
                        impl.since = payload->integerAt("nextSince").value_or(impl.since);
                    else {
                        impl.output->clear();
                        impl.outputBytes = 0;
                        impl.evicted = false;
                    }
                    impl.showItems(*payload, *locations);
                    if (method == "play.state" || method == "play.start" || method == "play.stop")
                        impl.append(payload->stringAt("state"));
                    if (method == "script.validate")
                        impl.status->setText(payload->boolAt("valid") == true
                                                 ? QStringLiteral("Script is valid")
                                                 : QStringLiteral("Script validation failed"));
                    if (method == "log.get" && payload->boolAt("truncated") == true)
                        impl.status->setText(QStringLiteral("More log entries are available; refresh again"));
                    if (impl.evicted) {
                        impl.status->setText(
                            method == "log.get" && payload->boolAt("truncated") == true
                                ? QStringLiteral("Older output was removed; more log entries are available")
                                : QStringLiteral("Older output was removed to keep the display bounded"));
                    }
                },
                Qt::QueuedConnection);
        };
        tasks.push_back({state, workbench.m_pool->spawn(sched::Lane::Background,
                                                        execute(std::move(request), std::move(completion)))});
        return true;
    }
};

EngineIntegration::EngineIntegration(Workbench& workbench)
    : QObject(&workbench), m_impl(std::make_unique<Impl>(*this, workbench)) {
    auto* panel = new QWidget(workbench.m_window);
    panel->setObjectName(QStringLiteral("frostEnginePanel"));
    // The tool window reparents this child lazily; keep it off the menu bar until then.
    panel->hide();
    m_impl->panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout();
    // No "Debug Lua" action: the engine's Lua debug endpoint is on no branch; FER-8 owns it.
    for (const auto& [title, method] : {std::pair{"Play", "play.start"},
                                        {"Stop", "play.stop"},
                                        {"State", "play.state"},
                                        {"Validate Script", "script.validate"},
                                        {"Run Tests", "test.run"},
                                        {"Refresh Log", "log.get"},
                                        {"Focus Editor", "editor.focus"}}) {
        auto* button = new QPushButton(QString::fromLatin1(title), panel);
        row->addWidget(button);
        m_impl->actions.emplace_back(button);
        connect(button, &QPushButton::clicked, this, [this, method] { m_impl->run(method); });
    }
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel Request"), panel);
    row->addWidget(cancelButton);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (m_impl->current) {
            m_impl->cancel(false);
            m_impl->status->setText(QStringLiteral("Request cancelled locally; the engine may still be working"));
        }
    });
    layout->addLayout(row);
    m_impl->status = new QLabel(panel);
    layout->addWidget(m_impl->status);
    m_impl->output = new QListWidget(panel);
    layout->addWidget(m_impl->output);
    connect(m_impl->output, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item->data(Qt::UserRole + 2).toString() != qs(m_impl->workbench.projectRoot()))
            return;
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            m_impl->run("navigation.resolve", utf8(path), item->data(Qt::UserRole + 1).toInt());
    });
    // projectChanged is emitted once the new root is set (or cleared on close).
    connect(&workbench, &Workbench::projectChanged, this, [this](const QString&) { m_impl->projectChanged(); });
    m_impl->projectChanged();
}
EngineIntegration::~EngineIntegration() {
    shutdown();
    // Never registered: the panel is still this integration's child of the window.
    if (!m_impl->registered)
        delete m_impl->panel.data();
}
void EngineIntegration::cancel() { m_impl->cancel(); }
void EngineIntegration::trustChanged() {
    if (m_impl->current && !m_impl->workbench.m_services.trust.isTrusted(m_impl->workbench.projectRoot()))
        m_impl->cancel(false);
}
void EngineIntegration::shutdown() {
    if (m_impl->stopped)
        return;
    m_impl->stopped = true;
    m_impl->cancel();
    for (auto& running : m_impl->tasks) {
        running.state->active.store(false);
        running.state->stop.request_stop();
    }
    if (m_impl->reactor)
        m_impl->reactor->shutdown();
    for (auto& running : m_impl->tasks)
        running.task.join();
    m_impl->tasks.clear();
    m_impl->reactor.reset();
}
} // namespace frost::app
