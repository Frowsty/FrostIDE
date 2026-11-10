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
#include "frost/build/run_service.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"

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
#include <memory>
#include <optional>
#include <set>
#include <string>
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

// How long debug.locals waits for lldb-dap's scopes and variables replies (a stopped STL frame
// answers in well under a second; the limit only ends a request the adapter dropped).
constexpr std::int64_t kLocalsTimeoutMs = 30000;

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
        // The origin as the tool printed it (relative origins are resolved into "file").
        .set("origin", item.diagnostic.file)
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
        if (method == "debug.locals") {
            debugLocals(client, id);
            return;
        }
        if (method == "debug.variables") {
            debugVariables(client, id, params);
            return;
        }
        if (method == "lua.complete" || method == "lua.completions" || method == "editor.complete") {
            luaComplete(client, id, params);
            return;
        }
        if (method == "lua.inspect") {
            luaInspect(client, id, params);
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
     *   state {} -> {project, cmake, clangd, lua, editor} what the workbench shows now (stateJson)
     *   open {path, line?, column?} -> {}           Workbench::openPath (a folder opens as the project)
     *   trust {root} -> {}                          the trust dialog's Trust button: the build
     *                                               controller's trust(root), which the CMake service
     *                                               follows; nothing is trusted otherwise
     *   cmake.profiles {} -> {profiles: [...], active, presetsError}
     *   cmake.select {name} -> {}                   CMakeService::setActiveProfile(name)
     *   cmake.configure {} -> {}                    CMakeService::configure (Reload CMake Project)
     *   cmake.targets {} -> {targets: [{name, type, sources}]}   the File API model
     *   cmake.build {action: build|rebuild|clean, target?} -> {} CMakeService::build
     *   wait {for: cmake|clangd|lua, timeoutMs} -> state   replies when the CMake service is idle (no
     *                                               job, no reply read), clangd runs, or LuaLS runs,
     *                                               or at the timeout with {"timedOut": true}
     *   problems {} -> {problems: [...]}            the Problems model's rows, in its order: severity,
     *                                               code, message, file (resolved), origin (as
     *                                               printed), line, column, jumpable, source
     *   problems.activate {index} -> {}             Problems' own activation (opens the location)
     *   editor {} -> {path, line, column} | {}      the current editor and its caret
     *   navigate.definition {} -> {}                Go to Declaration or Definition at the caret
     *   navigate.cppBinding {} -> {}                Go to C++ Binding at the caret (FEC-10b)
     *   lua.state {} -> {available, state, pid, restarts, configurationChangesSent, runtimeVersion}
     *   lua.complete {line?, column?, timeoutMs?} -> {items, labels, timedOut}
     *   lua.inspect {directory?} -> {files: [{file, diagnostics: [{line, column, code, message}]}]}
     *   lua.setRuntime {version} -> {}              changes lua.runtimeVersion (live didChangeConfiguration)
     *   run.configurations {} -> {configurations: [{id, name, type, target, program, arguments,
     *                               workingDirectory, environment, generated}], selected}
     *                                               the toolbar combo's run configurations (RUN-2)
     *   run.select {id} -> {}                       chooses a configuration, as the combo does
     *   run.edit {id, name?, arguments?: [...], workingDirectory?, environment?: [{name, value?}]}
     *                               -> {}           the Run/Debug Configurations dialog's OK with
     *                                               these edits (persisted in the workspace store)
     *   run.start {} -> {}                          clicks the toolbar's Run button (refused when
     *                                               it is disabled, with its tooltip)
     *   run.debug {} -> {}                          clicks the toolbar's Debug button (F5: builds the
     *                                               startup project, then debugs it under lldb-dap)
     *   run.stop {} -> {}                           clicks the toolbar's Stop button
     *   run.state {} -> {running, tabs: [{key, title, active, current, text}],
     *                    runs: [{configuration, processId, commandLine}]}
     *                                               the Run tool window's tabs (the console text,
     *                                               its last 64 KiB) and the RunManager's runs
     *   solution {} -> {available, path, format: sln|slnx, configurations, active, state, building,
     *                    trusted, untrusted (the build gate's folders not trusted), banner,
     *                    shownSolution, projects: [{name, path, kind, loaded,
     *                    evaluation: {source, configuration, platform, itemCount}, files}], startup,
     *                    lastBuild: {action, project, file, configuration, succeeded, exitCode,
     *                    errors, warnings, error}}  the open MSVC solution (FND-7b): the build
     *                                               controller's state and the Solution Explorer's
     *                                               own model (its projects and file paths)
     *   solution.configuration {name} -> {}         the toolbar configuration combo's choice
     *                                               ("Release|x64")
     *   solution.build {action: build|rebuild|clean, project?} -> {}   Build, Rebuild or Clean of
     *                                               the solution or of one project
     *   solution.compile {path} -> {}               Build > Compile (Ctrl+F7) of one source file
     *   solution.startup {project} -> {}            Set as Startup Project
     *   debug.state {} -> {available, status, session, state, processId?, stop?: {reason,
     *                      description, exceptionFilter, exceptionType, threadId, frames: [{name,
     *                      path, line, module}]}, outcome?: {exitCode, error, buildFailed, stopped},
     *                      breakpoints: [{path, line, enabled, status, message}], console?}  (the
     *                      console: the last 8000 characters of the Debug console)
     *                                               the Debug tool window's view of the session
     *   debug.breakpoint {path, line} -> {}         a line breakpoint (1-based), as a gutter click adds
     *   debug.exception {filter, enabled?, types?: [...]} -> {}   the Exceptions list's check box
     *                                               and its type list (DBG-2b)
     *   debug.continue {} -> {}                     Resume (F5 while paused)
     *   debug.stop {} -> {}                         Stop
     *   debug.locals {} -> {variables: [{name, value, type, reference, children: [{name, value,
     *                       type, reference}]}]}    the selected frame's Locals, one level expanded,
     *                                               from lldb-dap's replies (asynchronous)
     *   debug.variables {reference} -> {variables: [{name, value, type, reference}]}   the children
     *                                               of a row with a reference > 0, as expanding it
     *                                               asks lldb-dap (asynchronous)
     *   editor.type {text} -> {}                    typing at the caret of the current editor
     *   editor.undo {} -> {}                        Edit > Undo in the current editor
     *   editor.save {} -> {}                        File > Save of the current editor
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
        if (method == "navigate.cppBinding") {
            if (auto bound = workbench.gotoCppBinding(); !bound)
                return std::unexpected(bound.error());
            return Json::object();
        }
        if (method == "lua.state")
            return luaStateJson();
        if (method == "lua.setRuntime") {
            if (auto updated = workbench.setLuaRuntimeVersion(params.stringAt("version")); !updated)
                return std::unexpected(updated.error());
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
        if (method == "solution" || method.starts_with("solution."))
            return solutionMethod(method, params);
        if (method.starts_with("debug."))
            return debugMethod(method, params);
        if (method == "editor.type" || method == "editor.undo" || method == "editor.save")
            return editorMethod(method, params);
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    // --- MSVC solutions (FND-7b, j2_msvc) -----------------------------------------------------

    [[nodiscard]] static Result<build::BuildAction> buildActionOf(std::string_view action) {
        if (action == "build")
            return build::BuildAction::Build;
        if (action == "rebuild")
            return build::BuildAction::Rebuild;
        if (action == "clean")
            return build::BuildAction::Clean;
        return fail(ErrorCode::InvalidArgument, std::format("unknown build action '{}'", action));
    }

    // The project of `model` named `name` (its solution display name).
    [[nodiscard]] static std::optional<project::ProjectId> projectNamed(const project::SolutionModel& model,
                                                                        std::string_view name) {
        const auto projects = model.projects();
        for (std::size_t i = 0; i < projects.size(); ++i)
            if (projects[i].name == name)
                return project::ProjectId{static_cast<std::uint32_t>(i)};
        return std::nullopt;
    }

    Json solutionJson() {
        ui::BuildController& controller = workbench.build();
        const auto solution = controller.solution();
        if (!solution)
            return Json::object().set("available", false);
        const std::vector<project::SolutionConfiguration>& listed = controller.configurations();
        Json configurations = Json::array();
        for (const project::SolutionConfiguration& configuration : listed)
            configurations.push(Json(configuration.name()));
        const auto active = controller.activeConfigurationIndex();
        // The folders the build gate would ask to trust (trust_store.hpp: buildTrustRoots), as the
        // trust store answers now; the gate itself only runs when a build or a run starts.
        Json untrusted = Json::array();
        for (const std::string& root : project::buildTrustRoots(*solution))
            if (!workbench.trustStore().isTrusted(root))
                untrusted.push(Json(root));
        // The projects and their files as the Solution Explorer shows them (its own model).
        Json projects = Json::array();
        const ui::SolutionExplorer* explorer = workbench.solutionExplorer();
        const auto shown = explorer != nullptr ? explorer->solution() : nullptr;
        if (shown) {
            const auto infos = shown->projects();
            for (std::size_t i = 0; i < infos.size(); ++i) {
                const project::ProjectInfo& info = infos[i];
                Json files = Json::array();
                for (const std::string& path :
                     shown->projectFilePaths(project::ProjectId{static_cast<std::uint32_t>(i)}))
                    files.push(Json(path));
                Json evaluation;
                if (info.evaluation)
                    evaluation = Json::object()
                                     .set("source", std::string(project::toString(info.evaluation->source)))
                                     .set("configuration", info.evaluation->configuration)
                                     .set("platform", info.evaluation->platform)
                                     .set("itemCount", static_cast<std::int64_t>(info.evaluation->itemCount));
                projects.push(Json::object()
                                  .set("name", info.name)
                                  .set("path", info.path)
                                  .set("kind", std::string(project::toString(info.kind)))
                                  .set("loaded", info.nodeKind == project::NodeKind::Project)
                                  .set("evaluation", std::move(evaluation))
                                  .set("files", std::move(files)));
            }
        }
        Json startup;
        if (build::RunService* runs = workbench.runService(); runs != nullptr)
            if (const auto id = runs->startupProject(); id && id->value() < solution->projects().size())
                startup = Json(solution->project(*id).name);
        Json last;
        if (const auto& outcome = controller.lastOutcome()) {
            last = Json::object()
                       .set("action", std::string(build::toString(outcome->target.action)))
                       .set("project", outcome->target.projectName)
                       .set("file", outcome->target.file)
                       .set("configuration", outcome->target.configuration.name())
                       .set("succeeded", outcome->succeeded())
                       .set("exitCode", outcome->result.exitCode)
                       .set("errors", static_cast<std::int64_t>(outcome->counts.errors))
                       .set("warnings", static_cast<std::int64_t>(outcome->counts.warnings))
                       .set("error", outcome->error ? Json(outcome->error->describe()) : Json());
        }
        return Json::object()
            .set("available", true)
            .set("path", solution->path())
            .set("format", solution->format() == project::SolutionFormat::Slnx ? "slnx" : "sln")
            .set("configurations", std::move(configurations))
            .set("active", active && *active < listed.size() ? Json(listed[*active].name()) : Json())
            .set("state", std::string(build::toString(controller.state())))
            .set("building", controller.isBuilding())
            .set("trusted", project::checkBuildTrust(workbench.trustStore(), *solution).has_value())
            .set("untrusted", std::move(untrusted))
            .set("banner", explorer != nullptr ? explorer->evaluationBanner().toStdString() : std::string())
            .set("shownSolution", shown ? Json(shown->path()) : Json())
            .set("projects", std::move(projects))
            .set("startup", std::move(startup))
            .set("lastBuild", std::move(last));
    }

    Result<Json> solutionMethod(const std::string& method, const Json& params) {
        if (method == "solution")
            return solutionJson();
        ui::BuildController& controller = workbench.build();
        const auto solution = controller.solution();
        if (!solution)
            return fail(ErrorCode::InvalidArgument, "no solution is open");
        if (method == "solution.configuration") {
            // The toolbar's configuration combo (ConfigurationCombo) sets the same index.
            const std::string_view name = params.stringAt("name");
            const std::vector<project::SolutionConfiguration>& listed = controller.configurations();
            const auto it = std::ranges::find_if(listed, [name](const project::SolutionConfiguration& configuration) {
                return configuration.name() == name;
            });
            if (it == listed.end())
                return fail(ErrorCode::NotFound, std::format("the solution has no configuration '{}'", name));
            if (auto set = controller.setActiveConfiguration(static_cast<std::size_t>(it - listed.begin())); !set)
                return std::unexpected(set.error());
            return Json::object();
        }
        if (method == "solution.build") {
            auto action = buildActionOf(params.stringAt("action", "build"));
            if (!action)
                return std::unexpected(action.error());
            std::optional<project::ProjectId> project;
            if (const std::string_view name = params.stringAt("project"); !name.empty()) {
                project = projectNamed(*solution, name);
                if (!project)
                    return fail(ErrorCode::NotFound, std::format("the solution has no project '{}'", name));
            }
            if (auto started = controller.build(*action, project); !started)
                return std::unexpected(started.error());
            return Json::object();
        }
        if (method == "solution.compile") {
            // Build > Compile (Ctrl+F7) on a file, as the action does for the current editor's file.
            const std::string path(params.stringAt("path"));
            if (!controller.canCompile(path))
                return fail(ErrorCode::InvalidArgument, std::format("Compile is disabled for {}", path));
            if (auto started = controller.compileFile(path); !started)
                return std::unexpected(started.error());
            return Json::object();
        }
        if (method == "solution.startup") {
            // The Solution Explorer's Set as Startup Project goes to the same RunController call.
            ui::RunController* runs = workbench.runController();
            if (runs == nullptr)
                return fail(ErrorCode::InvalidArgument, "the workbench has no run controller");
            const std::string_view name = params.stringAt("project");
            const auto project = projectNamed(*solution, name);
            if (!project)
                return fail(ErrorCode::NotFound, std::format("the solution has no project '{}'", name));
            if (auto set = runs->setStartupProject(*project); !set)
                return std::unexpected(set.error());
            return Json::object();
        }
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    // --- The debugger (FND-7b) ----------------------------------------------------------------

    // What the Debug tool window shows, from the controller's GUI-thread copies of the session's
    // events, and how the last Debug run ended (DebugController::lastOutcome).
    Json debugJson() {
        ui::DebugController* controller = workbench.debugController();
        if (controller == nullptr)
            return Json::object().set("available", false);
        Json out = Json::object()
                       .set("available", true)
                       .set("status", controller->statusText().toStdString())
                       .set("session", controller->debugging())
                       .set("state", std::string(debug::toString(controller->state())));
        if (debug::DebugSession* session = controller->session(); session != nullptr)
            if (const auto pid = session->processId())
                out.set("processId", *pid);
        if (const auto& stop = controller->stopInfo(); stop && controller->paused()) {
            Json frames = Json::array();
            for (const debug::DebugFrame& frame : controller->frames().frames)
                frames.push(Json::object()
                                .set("name", frame.name)
                                .set("path", frame.path)
                                .set("line", frame.line)
                                .set("module", frame.module));
            out.set("stop", Json::object()
                                .set("reason", stop->reason)
                                .set("description", stop->description)
                                .set("exceptionFilter", stop->exceptionFilter)
                                .set("exceptionType", stop->exceptionType)
                                .set("threadId", stop->threadId ? Json(*stop->threadId) : Json())
                                .set("frames", std::move(frames)));
        }
        if (const auto& outcome = controller->lastOutcome())
            out.set("outcome", Json::object()
                                   .set("exitCode", outcome->exitCode ? Json(*outcome->exitCode) : Json())
                                   .set("error", outcome->error ? Json(outcome->error->describe()) : Json())
                                   .set("buildFailed", outcome->buildFailed)
                                   .set("stopped", outcome->stopped));
        // The Breakpoints tab's rows: each line breakpoint with lldb-dap's verdict on it.
        Json breakpoints = Json::array();
        for (const debug::LineBreakpoint& breakpoint : controller->breakpoints().all()) {
            const char* status = "unknown";
            switch (breakpoint.status) {
            case debug::BreakpointStatus::Unknown: status = "unknown"; break;
            case debug::BreakpointStatus::Pending: status = "pending"; break;
            case debug::BreakpointStatus::Verified: status = "verified"; break;
            case debug::BreakpointStatus::Unverified: status = "unverified"; break;
            }
            breakpoints.push(Json::object()
                                 .set("path", breakpoint.path)
                                 .set("line", breakpoint.line)
                                 .set("enabled", breakpoint.enabled)
                                 .set("status", std::string(status))
                                 .set("message", breakpoint.message));
        }
        out.set("breakpoints", std::move(breakpoints));
        // The Debug console's last lines (the adapter's and FrostIDE's notes, the program's output).
        if (const ui::RunConsole* console = controller->console(); console != nullptr) {
            const QString text = console->text();
            constexpr qsizetype kConsoleTail = 8000;
            out.set("console", text.right(kConsoleTail).toStdString());
        }
        return out;
    }

    Result<Json> debugMethod(const std::string& method, const Json& params) {
        if (method == "debug.state")
            return debugJson();
        ui::DebugController* controller = workbench.debugController();
        if (controller == nullptr)
            return fail(ErrorCode::InvalidArgument, "the workbench has no debugger");
        if (method == "debug.breakpoint") {
            // A gutter click's breakpoint (1-based line), kept by the same BreakpointManager.
            auto path = io::normalizePath(params.stringAt("path"));
            if (!path)
                return std::unexpected(path.error());
            if (auto added = controller->breakpoints().add(*path, params.integerAt("line").value_or(0)); !added)
                return std::unexpected(added.error());
            return Json::object();
        }
        if (method == "debug.exception") {
            // The Exceptions list's check box and its Edit dialog's type list (DBG-2b).
            const std::string filter(params.stringAt("filter", "cpp_throw"));
            if (const Json* types = params.find("types"); types != nullptr && types->isArray()) {
                std::vector<std::string> list;
                for (const Json& type : types->items())
                    if (type.isString())
                        list.push_back(*type.asString());
                if (!controller->breakpoints().setExceptionFilterTypes(filter, std::move(list)))
                    return fail(ErrorCode::NotFound, std::format("no exception filter '{}' takes types", filter));
            }
            const bool enabled = params.boolAt("enabled").value_or(true);
            if (!controller->setExceptionFilterEnabled(filter, enabled))
                return fail(ErrorCode::NotFound,
                            std::format("the exception filter '{}' is unknown or unavailable", filter));
            return Json::object();
        }
        if (method == "debug.continue") {
            if (!controller->resume())
                return fail(ErrorCode::InvalidArgument, "the debuggee is not stopped");
            return Json::object();
        }
        if (method == "debug.stop") {
            controller->stop();
            return Json::object();
        }
        return fail(ErrorCode::NotFound, std::format("unknown method '{}'", method));
    }

    /*
     * debug.locals: the selected frame's Locals scope, each variable with its children one level
     * down (the Variables view's first expansion), from lldb-dap's own scopes and variables replies.
     * Asynchronous: the reply follows the adapter's answers, or an error after kLocalsTimeoutMs.
     */
    void debugLocals(Client& client, const Json& id) {
        ui::DebugController* controller = workbench.debugController();
        if (controller == nullptr || !controller->paused() || !controller->selectedFrame()) {
            reply(client, id, fail(ErrorCode::InvalidArgument, "the debuggee is not stopped in a frame"));
            return;
        }
        struct Pending {
            std::vector<Json> variables;
            std::size_t waiting = 0;
            bool answered = false;
        };
        auto pending = std::make_shared<Pending>();
        QPointer<QLocalSocket> socket = client.socket;
        Client* raw = &client;
        const auto finish = [this, pending, socket, raw, id](Result<Json> result) {
            if (pending->answered || socket.isNull())
                return;
            pending->answered = true;
            reply(*raw, id, std::move(result));
        };
        QTimer::singleShot(std::chrono::milliseconds(kLocalsTimeoutMs), client.socket.data(), [finish] {
            finish(fail(ErrorCode::Timeout, "the debug adapter did not answer the Locals requests"));
        });
        const auto done = [pending, finish] {
            Json all = Json::array();
            for (Json& variable : pending->variables)
                all.push(std::move(variable));
            finish(Json::object().set("variables", std::move(all)));
        };
        // One child request per variable with children; `done` runs after the last reply.
        const auto expand = [pending, done](QPointer<ui::DebugController> guard, const debug::VariableList& list) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].variablesReference <= 0)
                    continue;
                ++pending->waiting;
                guard->variables(list[i].variablesReference, 0, 0,
                                 [pending, i, done](Result<debug::VariableList> children) {
                                     if (children) {
                                         Json shown = Json::array();
                                         for (const debug::dap::Variable& child : *children)
                                             shown.push(Json::object()
                                                            .set("name", child.name)
                                                            .set("value", child.value)
                                                            .set("type", child.type)
                                                            .set("reference", child.variablesReference));
                                         pending->variables[i].set("children", std::move(shown));
                                     }
                                     if (--pending->waiting == 0)
                                         done();
                                 });
            }
            if (pending->waiting == 0)
                done();
        };
        QPointer<ui::DebugController> guard(controller);
        controller->scopes(controller->selectedFrame()->id, [guard, pending, finish,
                                                             expand](Result<debug::ScopeList> scopes) {
            if (!scopes || guard.isNull()) {
                finish(scopes ? fail(ErrorCode::Cancelled, "the debugger ended") : std::unexpected(scopes.error()));
                return;
            }
            const auto locals = std::ranges::find_if(*scopes, [](const debug::dap::Scope& scope) {
                return scope.presentationHint == "locals" || scope.name == "Locals";
            });
            if (locals == scopes->end() || locals->variablesReference <= 0) {
                finish(fail(ErrorCode::NotFound, "the frame has no Locals scope"));
                return;
            }
            guard->variables(
                locals->variablesReference, 0, 0, [guard, pending, finish, expand](Result<debug::VariableList> list) {
                    if (!list || guard.isNull()) {
                        finish(list ? fail(ErrorCode::Cancelled, "the debugger ended") : std::unexpected(list.error()));
                        return;
                    }
                    for (const debug::dap::Variable& variable : *list)
                        pending->variables.push_back(Json::object()
                                                         .set("name", variable.name)
                                                         .set("value", variable.value)
                                                         .set("type", variable.type)
                                                         .set("reference", variable.variablesReference)
                                                         .set("children", Json::array()));
                    expand(guard, *list);
                });
        });
    }

    // debug.variables {reference}: the children of one Locals row (a "reference" debug.locals gave),
    // as expanding it in the Debug tool window asks lldb-dap for them.
    void debugVariables(Client& client, const Json& id, const Json& params) {
        ui::DebugController* controller = workbench.debugController();
        const std::int64_t reference = params.integerAt("reference").value_or(0);
        if (controller == nullptr || !controller->paused() || reference <= 0) {
            reply(client, id, fail(ErrorCode::InvalidArgument, "the debuggee is not stopped, or no reference"));
            return;
        }
        auto answered = std::make_shared<bool>(false);
        QPointer<QLocalSocket> socket = client.socket;
        Client* raw = &client;
        const auto finish = [this, answered, socket, raw, id](Result<Json> result) {
            if (*answered || socket.isNull())
                return;
            *answered = true;
            reply(*raw, id, std::move(result));
        };
        QTimer::singleShot(std::chrono::milliseconds(kLocalsTimeoutMs), client.socket.data(), [finish] {
            finish(fail(ErrorCode::Timeout, "the debug adapter did not answer the variables request"));
        });
        controller->variables(reference, 0, 0, [finish](Result<debug::VariableList> children) {
            if (!children) {
                finish(std::unexpected(children.error()));
                return;
            }
            Json shown = Json::array();
            for (const debug::dap::Variable& child : *children)
                shown.push(Json::object()
                               .set("name", child.name)
                               .set("value", child.value)
                               .set("type", child.type)
                               .set("reference", child.variablesReference));
            finish(Json::object().set("variables", std::move(shown)));
        });
    }

    // --- Typing in the editor (FND-7b's edit step) ------------------------------------------

    Result<Json> editorMethod(const std::string& method, const Json& params) {
        DocumentEditorView* view = workbench.currentView();
        if (view == nullptr)
            return fail(ErrorCode::InvalidArgument, "no editor is open");
        if (method == "editor.type") {
            // As typed at the caret (the editor's own typing path: auto-indent, pairs).
            if (!view->editor().typeText(params.stringAt("text")))
                return fail(ErrorCode::InvalidArgument, "the editor refused the typed text (read-only?)");
            return Json::object();
        }
        if (method == "editor.undo") {
            // Edit > Undo (Ctrl+Z): one step of the editor's own undo history.
            if (!view->editor().perform(ui::EditorAction::Undo))
                return fail(ErrorCode::InvalidArgument, "there is nothing to undo");
            return Json::object();
        }
        if (method == "editor.save") {
            // File > Save (Ctrl+S) of the current editor.
            if (auto saved = workbench.save(*view); !saved)
                return std::unexpected(saved.error());
            return Json::object();
        }
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
        if (method == "run.start" || method == "run.debug" || method == "run.stop") {
            QToolButton* button = window->mainToolbar().button(method == "run.start"   ? ui::MainToolbar::kRun
                                                               : method == "run.debug" ? ui::MainToolbar::kDebug
                                                                                       : ui::MainToolbar::kStop);
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
        // state() first: it takes no lock, so a running job answers without waiting for the service.
        const build::BuildState state = service->state();
        return (state == build::BuildState::Idle || state == build::BuildState::NeedsTrust) && !service->modelLoading();
    }

    [[nodiscard]] bool clangdRunning() {
        lang::ClangdService* clangd = workbench.clangd();
        return clangd != nullptr && clangd->state() == lang::ClangdState::Running;
    }

    [[nodiscard]] bool luaRunning() {
        lang::LuaService* lua = workbench.lua();
        return lua != nullptr && lua->state() == lang::LuaState::Running;
    }

    Json luaStateJson() {
        lang::LuaService* lua = workbench.lua();
        if (lua == nullptr)
            return Json::object().set("available", false);
        const auto pid = lua->serverPid();
        const std::string_view stateStr = lua->state() == lang::LuaState::Running      ? "Running"
                                          : lua->state() == lang::LuaState::Starting   ? "Starting"
                                          : lua->state() == lang::LuaState::Restarting ? "Restarting"
                                          : lua->state() == lang::LuaState::Failed     ? "Failed"
                                                                                       : "Stopped";
        std::string runtimeVersion;
        const Json cfg = lua->configuration();
        if (const Json* runtime = cfg.find("runtime"))
            runtimeVersion = std::string(runtime->stringAt("version"));
        return Json::object()
            .set("available", true)
            .set("state", std::string(stateStr))
            .set("pid", pid ? Json(static_cast<std::int64_t>(*pid)) : Json())
            .set("restarts", static_cast<std::int64_t>(lua->restarts()))
            .set("configurationChangesSent", static_cast<std::int64_t>(lua->configurationChangesSent()))
            .set("runtimeVersion", std::move(runtimeVersion));
    }

    void luaInspect(Client& client, const Json& id, const Json& params) {
        QPointer<QLocalSocket> socket = client.socket;
        Client* raw = &client;
        workbench.inspectLuaCodeAsync(
            params.stringAt("directory"),
            [this, socket, raw, id](Result<std::vector<lang::FileDiagnostics>> inspected) {
                if (socket.isNull())
                    return;
                if (!inspected) {
                    reply(*raw, id, std::unexpected(inspected.error()));
                    return;
                }
                Json files = Json::array();
                for (const lang::FileDiagnostics& file : *inspected) {
                    Json diags = Json::array();
                    for (const lang::LiveDiagnostic& d : file.diagnostics) {
                        diags.push(Json::object()
                                       .set("line", d.line)
                                       .set("column", d.column)
                                       .set("code", d.code)
                                       .set("message", d.message));
                    }
                    files.push(Json::object().set("file", file.file).set("diagnostics", std::move(diags)));
                }
                reply(*raw, id, Json::object().set("files", std::move(files)));
            });
    }

    void luaComplete(Client& client, const Json& id, const Json& params) {
        DocumentEditorView* view = workbench.currentView();
        ui::lang::LanguageController* language = workbench.language();
        if (view == nullptr || language == nullptr) {
            reply(client, id, fail(ErrorCode::InvalidArgument, "no active editor for completion"));
            return;
        }
        if (const auto line = params.integerAt("line"); line && *line > 0) {
            const int col = static_cast<int>(params.integerAt("column").value_or(1));
            view->editor().goToLine(static_cast<int>(*line), std::max(col, 1));
        }
        const std::int64_t timeoutMs =
            std::clamp<std::int64_t>(params.integerAt("timeoutMs").value_or(30000), 100, kMaxWaitMs);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        language->completionPopup().setList({});
        language->completeCode();

        auto* timer = new QTimer(client.socket.data());
        timer->setInterval(50);
        QPointer<QLocalSocket> socket = client.socket;
        Client* raw = &client;
        auto ticks = std::make_shared<int>(0);
        const auto check = [this, timer, socket, raw, id, deadline, ticks] {
            if (socket.isNull()) {
                timer->deleteLater();
                return;
            }
            ui::lang::LanguageController* langCtrl = workbench.language();
            if (langCtrl == nullptr) {
                timer->stop();
                timer->deleteLater();
                reply(*raw, id, fail(ErrorCode::Cancelled, "language controller detached"));
                return;
            }
            const auto& model = langCtrl->completionPopup().model();
            const bool hasItems = !model.items().empty();
            const bool timedOut = std::chrono::steady_clock::now() >= deadline;
            if (!hasItems && !timedOut) {
                if (++(*ticks) % 6 == 0)
                    langCtrl->completeCode();
                return;
            }
            timer->stop();
            timer->deleteLater();
            Json items = Json::array();
            Json labels = Json::array();
            std::set<std::string, std::less<>> seenLabels;
            const auto& allItems = model.items();
            const auto& entries = model.entries();
            const auto pushItem = [&](const lang::CompletionItem& item) {
                std::string bareLabel(item.raw.stringAt("label"));
                if (bareLabel.empty())
                    bareLabel = item.label;
                if (const auto paren = bareLabel.find('('); paren != std::string::npos)
                    bareLabel.resize(paren);
                if (bareLabel.empty() || !seenLabels.insert(bareLabel).second)
                    return;
                items.push(Json::object()
                               .set("label", bareLabel)
                               .set("name", bareLabel)
                               .set("displayLabel", item.label)
                               .set("detail", item.detail)
                               .set("insertText", item.insertText));
                labels.push(Json(bareLabel));
            };
            if (!entries.empty()) {
                for (const auto& entry : entries) {
                    if (entry.item < allItems.size())
                        pushItem(allItems[entry.item]);
                }
            } else {
                for (const lang::CompletionItem& item : allItems)
                    pushItem(item);
            }
            reply(*raw, id,
                  Json::object()
                      .set("items", std::move(items))
                      .set("labels", std::move(labels))
                      .set("timedOut", !hasItems));
        };
        QObject::connect(timer, &QTimer::timeout, timer, check);
        timer->start();
    }

    // Replies once the condition holds, checked on a 50 ms timer on the GUI thread (the event loop
    // keeps running meanwhile), or at the timeout.
    void wait(Client& client, const Json& id, const Json& params) {
        const std::string what(params.stringAt("for", "cmake"));
        if (what != "cmake" && what != "clangd" && what != "lua" && what != "luals") {
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
            const bool reached = what == "cmake" ? cmakeIdle() : what == "clangd" ? clangdRunning() : luaRunning();
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
            .set("lua", luaStateJson())
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
