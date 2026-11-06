// SPDX-License-Identifier: Apache-2.0
// Explicit, trusted commands to a running FrostEngine editor.
#include "engine_integration.hpp"

#include "frost/app/workbench.hpp"
#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/io/async_process.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/engine_automation.hpp"
#include "frost/lang/engine_discovery_process.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "cmake_integration.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
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
using PreparedLua = Result<debug::LuaDebugPlayConnection>;
using PreparedLuaCompletion = std::function<void(std::shared_ptr<PreparedLua>)>;
sched::Task<void> prepareLua(std::uint16_t port, std::string token, std::string root, project::TrustStore* trust,
                             std::shared_ptr<RequestState> state, PreparedLuaCompletion completion) {
    PreparedLua result = fail(ErrorCode::Cancelled, "Lua debug start was cancelled");
    if (state->active.load() && trust->isTrusted(root)) {
        result = debug::connectLuaDebugPlayEndpoint(
            port, token, std::chrono::steady_clock::now() + std::chrono::seconds(5), state->stop.get_token());
    }
    completion(std::make_shared<PreparedLua>(std::move(result)));
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
    std::string luaPreparingRoot;
    std::string luaPreparingEndpoint;
    std::uint64_t luaReservation = 0;
    struct LuaPending {
        std::shared_ptr<RequestState> request;
        std::string root;
        std::string endpointId;
        std::uint64_t sessionGeneration = 0;
        std::uint64_t connectionGeneration = 0;
        std::uint64_t operationId = 0;
        bool playRequested = false;
        bool playSettled = false;
        bool stopRequested = false;
        bool stopCompleted = false;
        bool cleanupPending = false;
        QMetaObject::Connection ready;
        QMetaObject::Connection terminated;
    };
    std::shared_ptr<LuaPending> luaPending;
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

    void releaseLua(std::uint64_t token) {
        if (token == 0 || luaReservation != token)
            return;
        workbench.m_buildService->releaseExternalLaunch(token);
        luaReservation = 0;
        luaPreparingRoot.clear();
        luaPreparingEndpoint.clear();
        QPointer<EngineIntegration> receiver(&owner);
        QMetaObject::invokeMethod(
            &owner,
            [receiver] {
                if (receiver && receiver->m_impl->workbench.m_cmake)
                    receiver->m_impl->workbench.m_cmake->service().admissionAvailable();
                if (receiver && receiver->m_impl->workbench.m_window)
                    receiver->m_impl->workbench.m_window->refreshActions();
            },
            Qt::QueuedConnection);
    }

    void cancel(bool resetLog = true) {
        const auto reservation = luaReservation;
        const std::string preparingRoot = luaPreparingRoot;
        const std::string preparingEndpoint = luaPreparingEndpoint;
        luaPreparingRoot.clear();
        auto pending = std::move(luaPending);
        if (pending) {
            QObject::disconnect(pending->ready);
            QObject::disconnect(pending->terminated);
            if (workbench.m_debug && workbench.m_debug->sessionGeneration() == pending->sessionGeneration)
                workbench.m_debug->stop();
        }
        if (current) {
            current->active.store(false);
            current->stop.request_stop();
            current.reset();
        }
        releaseLua(reservation);
        if (!preparingEndpoint.empty())
            stopLuaEndpoint(preparingRoot, preparingEndpoint);
        if (pending)
            stopLuaEndpoint(pending->root, pending->endpointId);
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
    bool run(std::string method, std::string location = {}, int line = 0,
             lang::Json suppliedParams = lang::Json::object(), Completion done = {}) {
        if (stopped || workbench.m_shutDown)
            return false;
        std::erase_if(tasks, [](Running& running) { return running.task.ready(); });
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
        lang::Json params = std::move(suppliedParams);
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
        status->setText(QStringLiteral("Contacting FrostEngine..."));
        QPointer<EngineIntegration> receiver(&owner);
        auto completion = [receiver, state, root, method, line,
                           done = std::move(done)](Result<lang::Json> result) mutable {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [receiver, state, root, method, line, done = std::move(done), result = std::move(result)]() mutable {
                    if (!receiver)
                        return;
                    auto& impl = *receiver->m_impl;
                    if (!state->active.load() || impl.current != state || impl.workbench.projectRoot() != root) {
                        // A successful start can arrive after cancellation. Clean up only the endpoint
                        // named by that response; never disturb a successor request or reservation.
                        if (method == "script.debug.start" && result) {
                            const auto* endpoint = result->find("result");
                            if (endpoint && endpoint->isObject()) {
                                const std::string endpointId(endpoint->stringAt("endpointId"));
                                if (!endpointId.empty())
                                    impl.stopLuaEndpoint(root, endpointId);
                            }
                        }
                        return;
                    }
                    impl.current.reset();
                    QTimer::singleShot(0, receiver, [receiver] {
                        if (receiver)
                            receiver->m_impl->drainLuaCleanup();
                    });
                    if (done) {
                        done(std::move(result));
                        return;
                    }
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
        return true;
    }

    void stopLuaEndpoint(std::string root, std::string endpointId) {
        const QString directory = discoveryDirectory();
        if (stopped || workbench.m_shutDown || !reactor || workbench.projectRoot() != root ||
            !workbench.m_services.trust.isTrusted(root) || !QDir::isAbsolutePath(directory))
            return;
        auto state = std::make_shared<RequestState>();
        Request request{root,
                        utf8(directory),
                        "script.debug.stop",
                        workbench.m_options.engineDiscoveryHelperPath.empty()
                            ? utf8(QCoreApplication::applicationFilePath())
                            : workbench.m_options.engineDiscoveryHelperPath,
                        workbench.m_services.trust.storePath(),
                        lang::Json::object().set("endpointId", endpointId),
                        std::nullopt,
                        &workbench.m_services.launcher,
                        workbench.m_pool.get(),
                        reactor,
                        &workbench.m_services.trust,
                        state};
        tasks.push_back({state, workbench.m_pool->spawn(sched::Lane::Background,
                                                        execute(std::move(request), [](Result<lang::Json>) {}))});
    }

    void drainLuaCleanup() {
        auto pending = luaPending;
        if (!pending || !pending->cleanupPending || current || stopped || workbench.m_shutDown ||
            workbench.projectRoot() != pending->root || !workbench.m_services.trust.isTrusted(pending->root))
            return;
        if (!run("script.debug.stop", {}, 0, lang::Json::object().set("endpointId", pending->endpointId)) ||
            luaPending != pending)
            return;
        QObject::disconnect(pending->ready);
        QObject::disconnect(pending->terminated);
        luaPending.reset();
    }

    void startLuaDebug() {
        if (stopped || current || luaPending || luaReservation || !workbench.canStartLuaDebug()) {
            status->setText(QStringLiteral("Stop the current request or debug session first"));
            return;
        }
        auto acquired = workbench.m_buildService->reserveExternalLaunch();
        if (!acquired) {
            status->setText(qs(acquired.error().message()));
            return;
        }
        const std::string root = workbench.projectRoot();
        const std::uint64_t reservation = *acquired;
        luaReservation = reservation;
        luaPreparingRoot = root;
        QPointer<EngineIntegration> receiver(&owner);
        if (workbench.m_window)
            workbench.m_window->refreshActions();
        if (!receiver)
            return;
        auto& afterRefresh = *receiver->m_impl;
        if (afterRefresh.stopped || afterRefresh.workbench.m_shutDown || afterRefresh.luaReservation != reservation ||
            afterRefresh.current || afterRefresh.luaPending || afterRefresh.workbench.projectRoot() != root ||
            !afterRefresh.workbench.m_services.trust.isTrusted(root) || !afterRefresh.workbench.canStartLuaDebug()) {
            afterRefresh.releaseLua(reservation);
            return;
        }
        afterRefresh.run(
            "script.debug.start", {}, 0, lang::Json::object(),
            [receiver, root, reservation](Result<lang::Json> result) mutable {
                if (!receiver)
                    return;
                auto& impl = *receiver->m_impl;
                if (impl.luaReservation != reservation)
                    return;
                if (!result) {
                    impl.releaseLua(reservation);
                    impl.status->setText(qs(result.error().message()));
                    return;
                }
                const lang::Json* endpoint = result->find("result");
                if (!endpoint || !endpoint->isObject()) {
                    impl.releaseLua(reservation);
                    impl.status->setText(QStringLiteral("Invalid Lua debug endpoint"));
                    return;
                }
                const auto port = endpoint->integerAt("port");
                const std::string token(endpoint->stringAt("token"));
                const std::string endpointId(endpoint->stringAt("endpointId"));
                const std::string projectFile(endpoint->stringAt("projectFile"));
                if (!port || *port <= 0 || *port > 65535 || token.empty() || endpointId.empty() ||
                    projectFile.empty()) {
                    impl.releaseLua(reservation);
                    impl.status->setText(QStringLiteral("Invalid Lua debug endpoint"));
                    return;
                }
                if (impl.workbench.projectRoot() != root || !impl.workbench.m_services.trust.isTrusted(root)) {
                    impl.releaseLua(reservation);
                    impl.stopLuaEndpoint(root, endpointId);
                    return;
                }
                auto state = std::make_shared<RequestState>();
                impl.current = state;
                impl.luaPreparingRoot = root;
                impl.luaPreparingEndpoint = endpointId;
                impl.status->setText(QStringLiteral("Connecting Lua debugger..."));
                auto complete = [receiver, state, root, endpointId, projectFile,
                                 reservation](std::shared_ptr<PreparedLua> connection) {
                    QMetaObject::invokeMethod(
                        QCoreApplication::instance(),
                        [receiver, state, root, endpointId, projectFile, reservation, connection] {
                            if (!receiver)
                                return;
                            auto& impl = *receiver->m_impl;
                            if (impl.luaReservation != reservation) {
                                if (*connection)
                                    (**connection).stream.reset();
                                return;
                            }
                            if (impl.current != state || !state->active.load() ||
                                impl.workbench.projectRoot() != root ||
                                !impl.workbench.m_services.trust.isTrusted(root)) {
                                if (impl.current == state)
                                    impl.current.reset();
                                if (*connection)
                                    (**connection).stream.reset();
                                impl.releaseLua(reservation);
                                impl.stopLuaEndpoint(root, endpointId);
                                return;
                            }
                            if (!*connection) {
                                impl.current.reset();
                                impl.releaseLua(reservation);
                                impl.status->setText(qs(connection->error().message()));
                                impl.stopLuaEndpoint(root, endpointId);
                                return;
                            }
                            debug::DebugTarget target{
                                .name = "FrostEngine Lua", .program = projectFile, .trustRoots = {root}};
                            const std::uint64_t expectedSession = impl.workbench.m_debug->sessionGeneration() + 1;
                            auto attached = impl.workbench.m_debug->attachTarget(
                                target,
                                debug::luaDebugAdapterFactory(std::move((**connection).stream),
                                                              (**connection).nextWireRequest,
                                                              (**connection).operationId),
                                rpc::Json::object(), [receiver, reservation] {
                                    return receiver && receiver->m_impl->luaReservation == reservation &&
                                           receiver->m_impl->workbench.canStartLuaDebug();
                                });
                            if (!receiver)
                                return;
                            auto& liveImpl = *receiver->m_impl;
                            if (liveImpl.luaReservation != reservation) {
                                if (attached && liveImpl.workbench.m_debug->sessionGeneration() == expectedSession)
                                    liveImpl.workbench.m_debug->stop();
                                return;
                            }
                            if (!attached) {
                                liveImpl.current.reset();
                                liveImpl.releaseLua(reservation);
                                liveImpl.status->setText(qs(attached.error().message()));
                                liveImpl.stopLuaEndpoint(root, endpointId);
                                return;
                            }
                            // attachTarget emits stateChanged synchronously. Cancellation or a project change
                            // may therefore have occurred before it returned and before readiness is subscribed.
                            if (liveImpl.luaReservation != reservation || liveImpl.current != state ||
                                !state->active.load() || liveImpl.workbench.projectRoot() != root ||
                                !liveImpl.workbench.m_services.trust.isTrusted(root) ||
                                liveImpl.workbench.m_debug->sessionGeneration() != expectedSession) {
                                if (liveImpl.workbench.m_debug->sessionGeneration() == expectedSession)
                                    liveImpl.workbench.m_debug->stop();
                                if (liveImpl.current == state)
                                    liveImpl.current.reset();
                                liveImpl.releaseLua(reservation);
                                liveImpl.stopLuaEndpoint(root, endpointId);
                                return;
                            }
                            auto pending = std::make_shared<LuaPending>();
                            pending->request = state;
                            pending->root = root;
                            pending->endpointId = endpointId;
                            pending->sessionGeneration = expectedSession;
                            pending->connectionGeneration = (**connection).connectionGeneration;
                            pending->operationId = (**connection).operationId;
                            liveImpl.luaPending = pending;
                            liveImpl.releaseLua(reservation); // Admission/UI notifications are queued.
                            if (!receiver)
                                return;
                            auto& readyImpl = *receiver->m_impl;
                            if (readyImpl.luaPending != pending || !pending->request->active.load() ||
                                readyImpl.workbench.projectRoot() != pending->root ||
                                !readyImpl.workbench.m_services.trust.isTrusted(pending->root) ||
                                !readyImpl.workbench.m_debug ||
                                readyImpl.workbench.m_debug->sessionGeneration() != pending->sessionGeneration) {
                                if (readyImpl.luaPending == pending)
                                    readyImpl.cancel(false); // Only the matching generation may be stopped.
                                return;
                            }
                            auto ready = [receiver, pending] {
                                if (!receiver)
                                    return;
                                auto& ownerImpl = *receiver->m_impl;
                                if (ownerImpl.luaPending != pending)
                                    return;
                                const bool sameSession =
                                    ownerImpl.workbench.m_debug &&
                                    ownerImpl.workbench.m_debug->sessionGeneration() == pending->sessionGeneration;
                                if (!sameSession) {
                                    if (pending->playRequested && !pending->playSettled)
                                        return;
                                    if (pending->stopRequested && !pending->stopCompleted) {
                                        ownerImpl.status->setText(QStringLiteral("Lua play stop outcome is unknown"));
                                        return;
                                    }
                                    if (ownerImpl.current == pending->request)
                                        ownerImpl.current.reset();
                                    pending->cleanupPending = true;
                                    ownerImpl.drainLuaCleanup();
                                    return;
                                }
                                if (!pending->request->active.load() ||
                                    ownerImpl.workbench.projectRoot() != pending->root ||
                                    !ownerImpl.workbench.m_services.trust.isTrusted(pending->root)) {
                                    ownerImpl.cancel(false);
                                    return;
                                }
                                if (pending->playRequested ||
                                    ownerImpl.workbench.m_debug->state() != debug::DebugState::Running)
                                    return;
                                pending->playRequested = true;
                                if (ownerImpl.current == pending->request)
                                    ownerImpl.current.reset();
                                ownerImpl.run(
                                    "script.debug.play", {}, 0,
                                    lang::Json::object()
                                        .set("endpointId", pending->endpointId)
                                        .set("connectionGeneration", pending->connectionGeneration)
                                        .set("operationId", pending->operationId),
                                    [receiver, pending](Result<lang::Json> result) {
                                        if (!receiver || receiver->m_impl->luaPending != pending)
                                            return;
                                        auto& impl = *receiver->m_impl;
                                        pending->playSettled = true;
                                        const bool sameSession =
                                            impl.workbench.m_debug &&
                                            impl.workbench.m_debug->sessionGeneration() == pending->sessionGeneration;
                                        if (!sameSession) {
                                            if (pending->stopRequested && !pending->stopCompleted) {
                                                impl.status->setText(
                                                    QStringLiteral("Lua play stop outcome is unknown"));
                                                return;
                                            }
                                            pending->cleanupPending = true;
                                            impl.drainLuaCleanup();
                                            return;
                                        }
                                        if (!result) {
                                            const auto code = result.error().code();
                                            const bool refused = code == ErrorCode::PermissionDenied ||
                                                                 code == ErrorCode::Untrusted ||
                                                                 code == ErrorCode::InvalidArgument;
                                            if (refused) {
                                                impl.cancel(false);
                                                impl.status->setText(qs(result.error().message()));
                                            } else {
                                                impl.status->setText(QStringLiteral(
                                                    "Play request outcome is unknown; check FrostEngine state"));
                                            }
                                        } else if (!pending->stopRequested) {
                                            impl.status->setText(QStringLiteral("FrostEngine play started"));
                                        }
                                    });
                            };
                            pending->ready = QObject::connect(readyImpl.workbench.m_debug.get(),
                                                              &ui::DebugController::stateChanged, receiver, ready);
                            pending->terminated = QObject::connect(
                                readyImpl.workbench.m_debug.get(), &ui::DebugController::adapterTerminated, receiver,
                                [receiver, pending](std::uint64_t generation) {
                                    if (receiver && receiver->m_impl->luaPending == pending &&
                                        generation == pending->sessionGeneration && pending->stopRequested)
                                        pending->stopCompleted = true;
                                });
                            readyImpl.status->setText(QStringLiteral("Lua debugger attached; starting play..."));
                            ready(); // Covers a synchronous Running or Ended transition before subscription.
                        },
                        Qt::QueuedConnection);
                };
                impl.tasks.push_back(
                    {state, impl.workbench.m_pool->spawn(sched::Lane::Background,
                                                         prepareLua(static_cast<std::uint16_t>(*port), token, root,
                                                                    &impl.workbench.m_services.trust, state,
                                                                    std::move(complete)))});
            });
        if (receiver && !receiver->m_impl->current)
            receiver->m_impl->releaseLua(reservation);
    }

    void stopPlay() {
        auto pending = luaPending;
        if (pending && pending->cleanupPending) {
            drainLuaCleanup();
            return;
        }
        if (!pending) {
            run("play.stop");
            return;
        }
        if (pending->stopRequested || !workbench.m_debug ||
            workbench.m_debug->sessionGeneration() != pending->sessionGeneration || !workbench.m_debug->session()) {
            status->setText(QStringLiteral("Lua play stop is already pending or its outcome is unknown"));
            return;
        }
        pending->stopRequested = true;
        QPointer<EngineIntegration> receiver(&owner);
        workbench.m_debug->session()->stopPlay([receiver, pending](Result<rpc::Json> result) mutable {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [receiver, pending, result = std::move(result)]() mutable {
                    if (!receiver || receiver->m_impl->luaPending != pending)
                        return;
                    auto& impl = *receiver->m_impl;
                    if (!result) {
                        // A transport failure can follow acceptance; only an explicit adapter
                        // refusal permits retrying without first reconciling the owned operation.
                        if (result.error().code() == ErrorCode::Busy ||
                            result.error().code() == ErrorCode::PermissionDenied ||
                            result.error().code() == ErrorCode::InvalidArgument) {
                            pending->stopRequested = false;
                            impl.status->setText(QStringLiteral("Lua play stop was refused"));
                        } else {
                            impl.status->setText(QStringLiteral("Lua play stop outcome is unknown"));
                        }
                        return;
                    }
                    if (!pending->stopCompleted &&
                        (!impl.workbench.m_debug ||
                         impl.workbench.m_debug->sessionGeneration() != pending->sessionGeneration)) {
                        impl.status->setText(QStringLiteral("Lua play stop outcome is unknown"));
                        return;
                    }
                    impl.status->setText(QStringLiteral("Stopping Lua play..."));
                },
                Qt::QueuedConnection);
        });
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
    for (const auto& [title, method] : {std::pair{"Play", "play.start"},
                                        {"Stop", "play.stop"},
                                        {"State", "play.state"},
                                        {"Validate Script", "script.validate"},
                                        {"Run Tests", "test.run"},
                                        {"Refresh Log", "log.get"},
                                        {"Focus Editor", "editor.focus"}}) {
        auto* button = new QPushButton(QString::fromLatin1(title), panel);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, method] {
            if (std::string_view(method) == "play.stop")
                m_impl->stopPlay();
            else
                m_impl->run(method);
        });
    }
    auto* luaDebugButton = new QPushButton(QStringLiteral("Debug Lua (experimental)"), panel);
    row->addWidget(luaDebugButton);
    connect(luaDebugButton, &QPushButton::clicked, this, [this] { m_impl->startLuaDebug(); });
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
                                                        std::string(ui::icons::kRunRun),
                                                        {},
                                                        [panel] { return panel; },
                                                        240});
    if (!added)
        delete panel;
}
EngineIntegration::~EngineIntegration() { shutdown(); }
void EngineIntegration::cancel() { m_impl->cancel(); }
bool EngineIntegration::luaReservationPending() const noexcept { return m_impl->luaReservation != 0; }
void EngineIntegration::trustChanged() {
    if ((m_impl->luaPending && !m_impl->workbench.m_services.trust.isTrusted(m_impl->luaPending->root)) ||
        (!m_impl->luaPreparingRoot.empty() && !m_impl->workbench.m_services.trust.isTrusted(m_impl->luaPreparingRoot)))
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
