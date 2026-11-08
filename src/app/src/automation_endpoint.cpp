// SPDX-License-Identifier: Apache-2.0
//
// The automation endpoint (automation_endpoint.hpp; FND-7a, ADR 0080). Everything but the
// "not compiled in" answers is inside FROST_AUTOMATION_ENDPOINT, so a build without it (Release, or
// FROST_ENABLE_AUTOMATION_ENDPOINT off) holds none of this code and none of its strings.

#include "automation_endpoint.hpp"

#include "frost/app/workbench.hpp"

#ifdef FROST_AUTOMATION_ENDPOINT
#include "frost/app/document_editor.hpp"
#include "frost/build/cmake_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include <QAbstractProxyModel>
#include <QCoreApplication>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace frost::app {

#ifndef FROST_AUTOMATION_ENDPOINT

struct AutomationEndpoint::Impl {
    std::string empty;
};

bool AutomationEndpoint::available() noexcept { return false; }

Result<std::unique_ptr<AutomationEndpoint>> AutomationEndpoint::start(Workbench&, const std::string&) {
    return fail(ErrorCode::Unsupported, "this build has no automation endpoint (Debug test builds only)");
}

const std::string& AutomationEndpoint::serverName() const noexcept { return m_impl->empty; }
const std::string& AutomationEndpoint::token() const noexcept { return m_impl->empty; }

#else

namespace {

using rpc::Json;
using namespace std::chrono_literals;

// The protocol marker: in the connection file and the "hello" result. The Release probe
// (tests/tools/test_automation_release_probe.py) looks for these bytes in the built image.
constexpr std::string_view kProtocol = "frost-automation-endpoint/1";

// The longest wait a "wait" request may ask for; the journeys' configure of a whole engine fits.
constexpr std::int64_t kMaxWaitMs = 60LL * 60 * 1000;

// 32 bytes from the system's CSPRNG as 64 lower-case hex digits.
std::string randomToken() {
    std::array<quint32, 8> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    std::string out;
    for (quint32 word : words)
        out += std::format("{:08x}", word);
    return out;
}

// Constant-time comparison, so the reply time says nothing about how much of a token matched.
bool sameToken(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

Json errorJson(const Error& error) {
    return Json::object().set("code", std::string(toString(error.code()))).set("message", std::string(error.message()));
}

Json diagnosticJson(const build::BuildDiagnostic& item, std::string_view source) {
    return Json::object()
        .set("severity", std::string(build::toString(item.diagnostic.severity)))
        .set("code", item.diagnostic.code)
        .set("message", item.diagnostic.message)
        .set("file", item.path.empty() ? item.diagnostic.file : item.path)
        .set("line", item.diagnostic.line)
        .set("column", item.diagnostic.column)
        .set("jumpable", item.jumpable)
        .set("source", std::string(source));
}

} // namespace

struct AutomationEndpoint::Impl {
    Workbench& workbench;
    std::string connectionFile;
    std::string token;
    std::string serverName;
    std::unique_ptr<QLocalServer> server;
    struct Client {
        QPointer<QLocalSocket> socket;
        std::string buffer;
        bool authenticated = false;
    };
    std::vector<std::unique_ptr<Client>> clients;

    explicit Impl(Workbench& wb) : workbench(wb) {}

    void accept() {
        while (QLocalSocket* socket = server->nextPendingConnection()) {
            auto client = std::make_unique<Client>();
            client->socket = socket;
            Client* raw = client.get();
            clients.push_back(std::move(client));
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, raw] { read(*raw); });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    void send(Client& client, const Json& message) {
        if (client.socket.isNull())
            return;
        std::string line = message.dump();
        line += '\n';
        client.socket->write(line.data(), static_cast<qint64>(line.size()));
        client.socket->flush();
    }

    void reply(Client& client, const Json& id, Result<Json> result) {
        Json message = Json::object().set("id", id);
        if (result)
            message.set("result", std::move(*result));
        else
            message.set("error", errorJson(result.error()));
        send(client, message);
    }

    void read(Client& client) {
        if (client.socket.isNull())
            return;
        const QByteArray bytes = client.socket->readAll();
        client.buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        for (std::size_t end = client.buffer.find('\n'); end != std::string::npos; end = client.buffer.find('\n')) {
            const std::string line = client.buffer.substr(0, end);
            client.buffer.erase(0, end + 1);
            if (line.empty())
                continue;
            handle(client, line);
            if (client.socket.isNull() || client.socket->state() != QLocalSocket::ConnectedState)
                return;
        }
    }

    void handle(Client& client, const std::string& line) {
        auto request = rpc::parseJson(line);
        if (!request || !request->isObject()) {
            reply(client, Json(), fail(ErrorCode::InvalidArgument, "a request is one JSON object per line"));
            return;
        }
        const Json id = request->find("id") != nullptr ? *request->find("id") : Json();
        const std::string method(request->stringAt("method"));
        const Json noParams = Json::object();
        const Json* found = request->find("params");
        const Json& params = found != nullptr && found->isObject() ? *found : noParams;
        if (!client.authenticated) {
            if (method != "hello" || !sameToken(params.stringAt("token"), token)) {
                reply(client, id, fail(ErrorCode::PermissionDenied, "the first request must be hello with the token"));
                client.socket->disconnectFromServer();
                return;
            }
            client.authenticated = true;
            reply(client, id,
                  Json::object()
                      .set("protocol", std::string(kProtocol))
                      .set("pid", static_cast<std::int64_t>(QCoreApplication::applicationPid())));
            return;
        }
        if (method == "wait") {
            wait(client, id, params);
            return;
        }
        reply(client, id, dispatch(method, params));
        if (method == "quit")
            QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::quit(); });
    }

    // --- The methods -----------------------------------------------------------------------------

    /*
     * kMethods (params -> result):
     *   hello {token} -> {protocol, pid}            the first request (header comment)
     *   state {} -> {project, cmake, clangd, editor} what the workbench shows now (stateJson)
     *   open {path, line?, column?} -> {}           Workbench::openPath (a folder opens as the project)
     *   trust {root} -> {}                          the trust dialog's Trust button: the build
     *                                               controller's trust(root), which the CMake service
     *                                               follows; nothing is trusted otherwise
     *   cmake.profiles {} -> {profiles: [...], active, presetsError}
     *   cmake.select {name} -> {}                   CMakeService::setActiveProfile(name)
     *   cmake.configure {} -> {}                    CMakeService::configure (Reload CMake Project)
     *   cmake.targets {} -> {targets: [{name, type, sources}]}   the File API model
     *   cmake.build {action: build|rebuild|clean, target?} -> {} CMakeService::build
     *   wait {for: cmake|clangd, timeoutMs} -> state   replies when the CMake service is idle (no
     *                                               job, no reply read) or clangd runs, or at the
     *                                               timeout with {"timedOut": true}
     *   problems {} -> {problems: [...]}            the Problems model's rows, in its order
     *   problems.activate {index} -> {}             Problems' own activation (opens the location)
     *   editor {} -> {path, line, column} | {}      the current editor and its caret
     *   navigate.definition {} -> {}                Go to Declaration or Definition at the caret
     *   run.configurations {} -> {configurations: [{id, name, type, target, program, arguments,
     *                               workingDirectory, environment, generated}], selected}
     *                                               the toolbar combo's run configurations (RUN-2)
     *   run.select {id} -> {}                       chooses a configuration, as the combo does
     *   run.edit {id, name?, arguments?: [...], workingDirectory?, environment?: [{name, value?}]}
     *                               -> {}           the Run/Debug Configurations dialog's OK with
     *                                               these edits (persisted in the workspace store)
     *   run.start {} -> {}                          clicks the toolbar's Run button (refused when
     *                                               it is disabled, with its tooltip)
     *   run.stop {} -> {}                           clicks the toolbar's Stop button
     *   run.state {} -> {running, tabs: [{key, title, active, current, text}],
     *                    runs: [{configuration, processId, commandLine}]}
     *                                               the Run tool window's tabs (the console text,
     *                                               its last 64 KiB) and the RunManager's runs
     *   quit {} -> {}                               QCoreApplication::quit after the reply
     */
    Result<Json> dispatch(const std::string& method, const Json& params) {
        if (method == "state")
            return stateJson();
        if (method == "open") {
            const std::string path(params.stringAt("path"));
            FileLocation at{static_cast<int>(params.integerAt("line").value_or(0)),
                            static_cast<int>(params.integerAt("column").value_or(0))};
            if (auto opened = workbench.openPath(path, at); !opened)
                return std::unexpected(opened.error());
            return Json::object();
        }
        if (method == "trust") {
            if (auto trusted = workbench.build().trust(params.stringAt("root")); !trusted)
                return std::unexpected(trusted.error());
            return Json::object();
        }
        if (method == "editor")
            return editorJson();
        if (method == "navigate.definition") {
            if (workbench.language() == nullptr || workbench.currentView() == nullptr)
                return fail(ErrorCode::InvalidArgument, "no editor to navigate from");
            workbench.language()->gotoDeclaration();
            return Json::object();
        }
        if (method == "problems")
            return problemsJson();
        if (method == "problems.activate")
            return activateProblem(params.integerAt("index").value_or(-1));
        if (method == "quit")
            return Json::object();
        if (method.starts_with("cmake."))
            return cmake(method, params);
        if (method.starts_with("run."))
            return runMethod(method, params);
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    // --- Run (RUN-2/RUN-3) -------------------------------------------------------------------

    [[nodiscard]] static Json configurationJson(const run::RunConfiguration& configuration, bool generated) {
        Json arguments = Json::array();
        for (const std::string& argument : configuration.arguments)
            arguments.push(Json(argument));
        Json environment = Json::array();
        for (const run::EnvironmentEntry& entry : configuration.environment)
            environment.push(
                Json::object().set("name", entry.name).set("value", entry.value ? Json(*entry.value) : Json()));
        return Json::object()
            .set("id", configuration.id)
            .set("name", configuration.name)
            .set("type", std::string(run::toString(configuration.type)))
            .set("target", configuration.target)
            .set("program", configuration.program)
            .set("arguments", std::move(arguments))
            .set("workingDirectory", configuration.workingDirectory)
            .set("environment", std::move(environment))
            .set("generated", generated);
    }

    Result<Json> runMethod(const std::string& method, const Json& params) {
        ui::RunController* controller = workbench.runController();
        ui::ShellWindow* window = workbench.window();
        if (controller == nullptr || window == nullptr)
            return fail(ErrorCode::InvalidArgument, "the workbench has no run controller or window");
        if (method == "run.configurations") {
            Json list = Json::array();
            for (const run::RunConfiguration& configuration : controller->configurations().all())
                list.push(configurationJson(configuration, controller->configurations().isGenerated(configuration.id)));
            const run::RunConfiguration* selected = controller->selectedConfiguration();
            return Json::object()
                .set("configurations", std::move(list))
                .set("selected", selected ? Json(selected->id) : Json());
        }
        if (method == "run.select") {
            if (auto selected = controller->selectConfiguration(std::string(params.stringAt("id"))); !selected)
                return std::unexpected(selected.error());
            return Json::object();
        }
        if (method == "run.edit") {
            const std::string id(params.stringAt("id"));
            std::vector<run::RunConfiguration> edited = controller->configurations().all();
            const auto it = std::ranges::find(edited, id, &run::RunConfiguration::id);
            if (it == edited.end())
                return fail(ErrorCode::NotFound, std::format("no run configuration '{}'", id));
            if (const Json* name = params.find("name"); name != nullptr && name->isString())
                it->name = *name->asString();
            if (const Json* arguments = params.find("arguments"); arguments != nullptr && arguments->isArray()) {
                it->arguments.clear();
                for (const Json& argument : arguments->items())
                    if (argument.isString())
                        it->arguments.push_back(*argument.asString());
            }
            if (const Json* cwd = params.find("workingDirectory"); cwd != nullptr && cwd->isString())
                it->workingDirectory = *cwd->asString();
            if (const Json* environment = params.find("environment");
                environment != nullptr && environment->isArray()) {
                it->environment.clear();
                for (const Json& entry : environment->items()) {
                    run::EnvironmentEntry variable;
                    variable.name = std::string(entry.stringAt("name"));
                    if (const Json* value = entry.find("value"); value != nullptr && value->isString())
                        variable.value = *value->asString();
                    it->environment.push_back(std::move(variable));
                }
            }
            if (auto replaced = controller->replaceConfigurations(edited); !replaced)
                return std::unexpected(replaced.error());
            return Json::object();
        }
        if (method == "run.start" || method == "run.stop") {
            QToolButton* button =
                window->mainToolbar().button(method == "run.start" ? ui::MainToolbar::kRun : ui::MainToolbar::kStop);
            if (button == nullptr)
                return fail(ErrorCode::NotFound, "the toolbar has no such button");
            if (!button->isEnabled())
                return fail(ErrorCode::Busy,
                            std::format("the toolbar button is disabled: {}", button->toolTip().toStdString()));
            button->click();
            return Json::object();
        }
        if (method == "run.state") {
            Json tabs = Json::array();
            if (ui::RunToolWindow* runs = workbench.runToolWindow()) {
                const ui::RunConsole* current = runs->currentConsole();
                for (ui::RunConsole* console : runs->consoles()) {
                    // The console's last 64 KiB: enough to see the output stream; never the whole log.
                    constexpr qsizetype kTail = 64 * 1024;
                    const QString text = console->text();
                    tabs.push(Json::object()
                                  .set("key", runs->keyOf(console).toStdString())
                                  .set("title", console->title()->text().toStdString())
                                  .set("active", console->active())
                                  .set("current", console == current)
                                  .set("text", (text.size() > kTail ? text.right(kTail) : text).toStdString()));
                }
            }
            Json active = Json::array();
            if (run::RunManager* manager = workbench.runManager())
                for (const run::RunStatus& status : manager->activeRuns())
                    active.push(Json::object()
                                    .set("configuration", status.configuration.id)
                                    .set("processId", static_cast<std::int64_t>(status.processId))
                                    .set("commandLine", status.commandLine));
            return Json::object()
                .set("running", controller->running())
                .set("tabs", std::move(tabs))
                .set("runs", std::move(active));
        }
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    Result<Json> cmake(const std::string& method, const Json& params) {
        build::CMakeService* service = workbench.cmakeService();
        if (service == nullptr)
            return fail(ErrorCode::InvalidArgument, "no CMake project is open");
        if (method == "cmake.profiles") {
            Json list = Json::array();
            for (const project::CMakeProfile& profile : service->profiles())
                list.push(Json::object()
                              .set("name", profile.name)
                              .set("displayName", profile.displayName)
                              .set("configurePreset", profile.configurePreset)
                              .set("buildPreset", profile.buildPreset)
                              .set("buildDirectory", profile.buildDirectory)
                              .set("usable", profile.usable())
                              .set("problem", profile.problem));
            const auto active = service->activeProfile();
            const auto presetsError = service->presetsError();
            return Json::object()
                .set("profiles", std::move(list))
                .set("active", active ? Json(active->name) : Json())
                .set("presetsError", presetsError ? Json(*presetsError) : Json());
        }
        if (method == "cmake.select") {
            if (auto selected = service->setActiveProfile(params.stringAt("name")); !selected)
                return std::unexpected(selected.error());
            return Json::object();
        }
        if (method == "cmake.configure") {
            if (auto started = service->configure(); !started)
                return std::unexpected(started.error());
            return Json::object();
        }
        if (method == "cmake.targets") {
            const auto model = service->model();
            Json list = Json::array();
            if (model) {
                for (const project::CMakeTarget& target : model->targets) {
                    Json sources = Json::array();
                    for (const project::CMakeSource& source : target.sources)
                        sources.push(Json(source.path));
                    list.push(Json::object()
                                  .set("name", target.name)
                                  .set("type", std::string(project::toString(target.type)))
                                  .set("sources", std::move(sources)));
                }
            }
            return Json::object().set("available", model != nullptr).set("targets", std::move(list));
        }
        if (method == "cmake.build") {
            const std::string_view action = params.stringAt("action", "build");
            build::BuildAction kind = build::BuildAction::Build;
            if (action == "rebuild")
                kind = build::BuildAction::Rebuild;
            else if (action == "clean")
                kind = build::BuildAction::Clean;
            else if (action != "build")
                return fail(ErrorCode::InvalidArgument, std::format("unknown build action '{}'", action));
            std::optional<std::size_t> index;
            if (const std::string_view name = params.stringAt("target"); !name.empty()) {
                const auto model = service->model();
                if (!model)
                    return fail(ErrorCode::InvalidArgument, "no CMake model yet (configure first)");
                for (std::size_t i = 0; i < model->targets.size() && !index; ++i)
                    if (model->targets[i].name == name)
                        index = i;
                if (!index)
                    return fail(ErrorCode::NotFound, std::format("no target '{}' in the CMake model", name));
            }
            if (auto started = service->build(kind, index); !started)
                return std::unexpected(started.error());
            return Json::object();
        }
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    [[nodiscard]] bool cmakeIdle() {
        build::CMakeService* service = workbench.cmakeService();
        if (service == nullptr)
            return true;
        const build::BuildState state = service->state();
        return !service->modelLoading() && (state == build::BuildState::Idle || state == build::BuildState::NeedsTrust);
    }

    [[nodiscard]] bool clangdRunning() {
        lang::ClangdService* clangd = workbench.clangd();
        return clangd != nullptr && clangd->state() == lang::ClangdState::Running;
    }

    // Replies once the condition holds, checked on a 50 ms timer on the GUI thread (the event loop
    // keeps running meanwhile), or at the timeout.
    void wait(Client& client, const Json& id, const Json& params) {
        const std::string what(params.stringAt("for", "cmake"));
        if (what != "cmake" && what != "clangd") {
            reply(client, id, fail(ErrorCode::InvalidArgument, std::format("cannot wait for '{}'", what)));
            return;
        }
        const std::int64_t timeoutMs =
            std::clamp<std::int64_t>(params.integerAt("timeoutMs").value_or(60000), 0, kMaxWaitMs);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        auto* timer = new QTimer(client.socket.data());
        timer->setInterval(50);
        QPointer<QLocalSocket> socket = client.socket;
        Client* raw = &client;
        const auto check = [this, timer, socket, raw, id, what, deadline] {
            if (socket.isNull()) {
                timer->deleteLater();
                return;
            }
            const bool reached = what == "cmake" ? cmakeIdle() : clangdRunning();
            const bool timedOut = std::chrono::steady_clock::now() >= deadline;
            if (!reached && !timedOut)
                return;
            timer->stop();
            timer->deleteLater();
            Json state = stateJson();
            state.set("timedOut", !reached);
            reply(*raw, id, std::move(state));
        };
        QObject::connect(timer, &QTimer::timeout, timer, check);
        timer->start();
    }

    Json editorJson() {
        const DocumentEditorView* view = workbench.currentView();
        if (view == nullptr)
            return Json::object().set("available", false);
        const ui::EditorStatus status = view->status();
        return Json::object()
            .set("available", true)
            .set("path", view->path())
            .set("line", status.line)
            .set("column", status.column)
            .set("dirty", status.dirty);
    }

    Json problemsJson() {
        Json list = Json::array();
        if (const ui::ProblemsView* view = workbench.problems(); view != nullptr && view->model() != nullptr) {
            for (const ui::Problem& problem : view->model()->problems())
                list.push(diagnosticJson(problem.diagnostic,
                                         problem.source == ui::DiagnosticSource::Build ? "build" : "live"));
        }
        return Json::object().set("problems", std::move(list));
    }

    Result<Json> activateProblem(std::int64_t index) {
        ui::ProblemsView* view = workbench.problems();
        if (view == nullptr || view->model() == nullptr)
            return fail(ErrorCode::InvalidArgument, "no Problems view");
        if (index < 0 || std::cmp_greater_equal(index, view->model()->problems().size()))
            return fail(ErrorCode::OutOfRange, std::format("no problem {}", index));
        // The view shows the model through its filter proxy; activate the row the user would click.
        QModelIndex source = view->model()->index(static_cast<int>(index), 0);
        QModelIndex shown = source;
        if (auto* proxy = qobject_cast<QAbstractProxyModel*>(view->view()->model()); proxy != nullptr)
            shown = proxy->mapFromSource(source);
        if (!shown.isValid())
            return fail(ErrorCode::InvalidArgument, std::format("problem {} is filtered out of the view", index));
        if (!view->activate(shown))
            return fail(ErrorCode::InvalidArgument, std::format("problem {} has no location to open", index));
        return Json::object();
    }

    Json stateJson() {
        Json cmakeState = Json::object().set("available", false);
        if (build::CMakeService* service = workbench.cmakeService(); service != nullptr) {
            const auto active = service->activeProfile();
            const auto configured = service->lastConfigure();
            const auto built = service->lastOutcome();
            const build::DiagnosticCounts counts = service->diagnosticCounts();
            cmakeState = Json::object()
                             .set("available", true)
                             .set("sourceDirectory", service->sourceDirectory())
                             .set("state", std::string(build::toString(service->state())))
                             .set("modelLoading", service->modelLoading())
                             .set("modelLoaded", service->model() != nullptr)
                             .set("targetsCurrent", workbench.cmakeTargetsCurrent())
                             .set("activeProfile", active ? Json(active->name) : Json())
                             .set("buildDirectory", service->activeBuildDirectory())
                             .set("errors", static_cast<std::int64_t>(counts.errors))
                             .set("warnings", static_cast<std::int64_t>(counts.warnings));
            if (configured)
                cmakeState.set("lastConfigure", Json::object()
                                                    .set("profile", configured->profile)
                                                    .set("succeeded", configured->succeeded())
                                                    .set("exitCode", configured->result.exitCode)
                                                    .set("modelLoaded", configured->modelLoaded));
            if (built)
                cmakeState.set("lastBuild", Json::object()
                                                .set("action", std::string(build::toString(built->request.action)))
                                                .set("succeeded", built->succeeded())
                                                .set("exitCode", built->result.exitCode)
                                                .set("errors", static_cast<std::int64_t>(built->counts.errors)));
        }
        Json clangdState = Json::object().set("available", false);
        if (lang::ClangdService* clangd = workbench.clangd(); clangd != nullptr) {
            const auto database = clangd->compileCommands();
            const auto pid = clangd->serverPid();
            clangdState = Json::object()
                              .set("available", true)
                              .set("state", std::string(lang::toString(clangd->state())))
                              .set("pid", pid ? Json(static_cast<std::int64_t>(*pid)) : Json())
                              .set("restarts", static_cast<std::int64_t>(clangd->restarts()))
                              .set("compileCommandsDirectory", database ? Json(database->directory) : Json())
                              .set("compileCommandsSource",
                                   database ? Json(std::string(lang::toString(database->source))) : Json());
        }
        return Json::object()
            .set("project", workbench.projectRoot())
            .set("cmake", std::move(cmakeState))
            .set("clangd", std::move(clangdState))
            .set("editor", editorJson());
    }
};

bool AutomationEndpoint::available() noexcept { return true; }

Result<std::unique_ptr<AutomationEndpoint>> AutomationEndpoint::start(Workbench& workbench,
                                                                      const std::string& connectionFile) {
    if (!io::isAbsolutePath(connectionFile))
        return fail(ErrorCode::InvalidArgument,
                    std::format("the connection file '{}' is not absolute", connectionFile));
    auto impl = std::make_unique<Impl>(workbench);
    impl->connectionFile = connectionFile;
    impl->token = randomToken();
    impl->server = std::make_unique<QLocalServer>();
    impl->server->setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = QStringLiteral("frost-automation-%1-%2")
                             .arg(QCoreApplication::applicationPid())
                             .arg(QString::fromStdString(randomToken().substr(0, 16)));
    if (!impl->server->listen(name))
        return fail(ErrorCode::IoError, std::format("cannot listen on {}: {}", name.toStdString(),
                                                    impl->server->errorString().toStdString()));
    impl->serverName = impl->server->fullServerName().toStdString();
    Impl* raw = impl.get();
    QObject::connect(impl->server.get(), &QLocalServer::newConnection, impl->server.get(), [raw] { raw->accept(); });
    const Json file = Json::object()
                          .set("protocol", std::string(kProtocol))
                          .set("server", impl->serverName)
                          .set("token", impl->token)
                          .set("pid", static_cast<std::int64_t>(QCoreApplication::applicationPid()));
    if (auto written = io::realFileSystem().writeFileAtomic(connectionFile, file.dump() + "\n"); !written)
        return std::unexpected(written.error());
    return std::unique_ptr<AutomationEndpoint>(new AutomationEndpoint(std::move(impl)));
}

const std::string& AutomationEndpoint::serverName() const noexcept { return m_impl->serverName; }
const std::string& AutomationEndpoint::token() const noexcept { return m_impl->token; }

#endif

AutomationEndpoint::AutomationEndpoint(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

AutomationEndpoint::~AutomationEndpoint() = default;

} // namespace frost::app
