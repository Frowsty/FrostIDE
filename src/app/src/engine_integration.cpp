// SPDX-License-Identifier: Apache-2.0
// Explicit, trusted commands to a running FrostEngine editor.
#include "engine_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/engine_discovery_process.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
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
        auto input = lang::Json::object()
                         .set("root", request.root)
                         .set("directory", request.directory)
                         .set("trustPath", request.trustPath)
                         .set("method", request.method)
                         .set("params", std::move(request.params));
        if (trusted())
            result = co_await lang::requestEngineInHelper(*request.launcher, {*request.reactor, *request.scheduler},
                                                          request.executable, std::move(input),
                                                          std::chrono::seconds(20), request.state->stop.get_token());
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
    std::shared_ptr<RequestState> current;
    std::shared_ptr<io::Reactor> reactor;
    struct Running {
        std::shared_ptr<RequestState> state;
        sched::JoinHandle<void> task;
    };
    std::vector<Running> tasks;
    bool stopped = false;
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
            status->setText(QStringLiteral("No request running"));
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
    void run(std::string method, std::string location = {}, int line = 0) {
        if (stopped || workbench.m_shutDown)
            return;
        std::erase_if(tasks, [](Running& running) { return running.task.ready(); });
        if (current) {
            status->setText(QStringLiteral("A FrostEngine request is already running"));
            return;
        }
        const std::string root = workbench.projectRoot();
        if (root.empty() || !workbench.m_services.trust.isTrusted(root)) {
            status->setText(QStringLiteral("Trust the project before using FrostEngine automation"));
            return;
        }
        const QString directory = discoveryDirectory();
        if (!io::isAbsolutePath(root) || !QDir::isAbsolutePath(directory)) {
            status->setText(QStringLiteral("Cannot locate the project or automation directory"));
            return;
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
                return;
            }
            const QString relative = QDir(qs(root)).relativeFilePath(qs(view->path()));
            if (!relative.startsWith(QStringLiteral("Assets/")) || relative.split('/').contains(QStringLiteral(".."))) {
                status->setText(QStringLiteral("The script must be inside this project's Assets folder"));
                return;
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
                return;
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
        status->setText(QStringLiteral("Contacting FrostEngine…"));
        QPointer<EngineIntegration> receiver(&owner);
        auto completion = [receiver, state, root, method, line](Result<lang::Json> result) mutable {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [receiver, state, root, method, line, result = std::move(result)]() mutable {
                    if (!receiver || !state->active.load())
                        return;
                    auto& impl = *receiver->m_impl;
                    if (impl.current != state || impl.workbench.projectRoot() != root)
                        return;
                    impl.current.reset();
                    if (!result) {
                        const bool uncertain = result.error().code() == ErrorCode::Timeout ||
                                               result.error().code() == ErrorCode::Cancelled;
                        impl.status->setText(
                            uncertain
                                ? QStringLiteral("Request timed out or was cancelled; the engine may still be working")
                                : qs(result.error().message()));
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
    }
};

EngineIntegration::EngineIntegration(Workbench& workbench)
    : QObject(&workbench), m_impl(std::make_unique<Impl>(*this, workbench)) {
    auto* panel = new QWidget(workbench.m_window);
    m_impl->panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout();
    for (const auto& [title, method] : {std::pair{"Play", "play.start"},
                                        {"Stop", "play.stop"},
                                        {"State", "play.state"},
                                        {"Validate Script", "script.validate"},
                                        {"Run Tests", "test.run"},
                                        {"Refresh Log", "log.get"},
                                        {"Focus Editor", "editor.focus"}}) {
        auto* button = new QPushButton(QString::fromLatin1(title), panel);
        row->addWidget(button);
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
    m_impl->status = new QLabel(QStringLiteral("No request running"), panel);
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
    auto added = workbench.m_window->toolWindows().add({"frostEngine",
                                                        QStringLiteral("FrostEngine"),
                                                        ui::ToolWindowSide::Bottom,
                                                        {},
                                                        {},
                                                        [panel] { return panel; },
                                                        240});
    if (!added)
        delete panel;
}
EngineIntegration::~EngineIntegration() { shutdown(); }
void EngineIntegration::cancel() { m_impl->cancel(); }
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
