// SPDX-License-Identifier: Apache-2.0
// C2 reads real IDE services and stages mutations as explicit user-approved cancellable MCP tasks.
#include "agent_tools.hpp"
#include "agent_private.hpp"
#include "agent_scope.hpp"
#include "frost/agent/protocol.hpp"
#include "frost/agent/ide_queries.hpp"
#include "frost/settings/registry.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/local_ipc.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/uri.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/test_results_tree.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "cmake_integration.hpp"
#include "language_bridge.hpp"
#include "unit_tests_window.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>
#include <condition_variable>
#include <atomic>
#include <array>
#include <algorithm>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include "frost/ui/vcs/diff_viewer.hpp"
#include <mutex>
#include <set>
#include <QRegularExpression>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <cerrno>
#endif
#include <QElapsedTimer>
namespace frost::app {
namespace {
using Json = agent::Json;
using Reply = std::function<void(QJsonObject)>;
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }
QJsonObject qjson(const Json& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).object(); }
QJsonObject error(const QJsonValue& id, const QString& reason, int code = -32602) { return {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", reason}}}}; }
QJsonObject answer(const QJsonValue& id, const QJsonObject& result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; }
bool deadOwner(qint64 pid) {
    if (pid <= 0 || pid > 0x7fffffff) return false;
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    DWORD code = STILL_ACTIVE;
    const bool dead = GetExitCodeProcess(process, &code) && code != STILL_ACTIVE;
    CloseHandle(process); return dead;
#else
    return kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
#endif
}
void cleanC2Records(const QString& directory) {
    for (const auto& file : QDir(directory).entryInfoList({"*.discovery.json"}, QDir::Files | QDir::NoSymLinks)) {
        const auto instance = file.fileName().chopped(QStringLiteral(".discovery.json").size());
        if (!QRegularExpression("^[0-9a-f]{32}$").match(instance).hasMatch()) continue;
        const auto bytes = readAgentPrivateFile(utf8(file.absoluteFilePath()));
        if (!bytes) continue;
        const auto record = QJsonDocument::fromJson(QByteArray::fromStdString(*bytes)).object();
        if (record.value("kind") != "frostide-mcp" || record.value("instance").toString() != instance || !deadOwner(record.value("pid").toInteger())) continue;
        for (const auto* suffix : {".discovery.json", ".mcp.json", ".hooks.json"}) {
            const auto path = QDir(directory).filePath(instance + suffix);
            if (readAgentPrivateFile(utf8(path))) QFile::remove(path);
        }
    }
}
QString randomToken() {
    QString result;
    for (int i = 0; i < 4; ++i) result += QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0');
    return result;
}
bool sameToken(const QString& left, const QString& right) {
    unsigned difference = static_cast<unsigned>(left.size() ^ right.size());
    for (qsizetype i = 0; i < right.size(); ++i) difference |= static_cast<unsigned>(i < left.size() ? left[i].unicode() : 0) ^ right[i].unicode();
    return difference == 0;
}
sched::Task<void> languageResult(sched::Task<Result<lang::Json>> task, std::function<void(Result<Json>)> done) {
    auto result = co_await std::move(task);
    done(std::move(result));
}
struct Waiter { std::mutex mutex; std::condition_variable changed; bool done = false; Result<std::string> result = fail(ErrorCode::Timeout, "MCP GUI request timed out"); };
}
struct AgentTools::State {
    AgentTools& self;
    Workbench& owner;
    std::function<bool()> admitted;
    QString root, token, instance, discovery, config, hooks; QStringList ownedFiles;
    std::unique_ptr<io::LocalIpcOwner> listener;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(false);
    std::stop_source stop;
    agent::StopHookGuard stopHook;
    std::map<std::string, std::set<std::string>> beforeErrors;
    std::set<std::string> knownBaseline, edited;
    QString debuggerStamp; std::int64_t notificationRevision = 0;
    struct Page { std::vector<Json> pages; std::size_t next = 0; QString tool; };
    std::map<QString, Page> pages;
    struct Task { QString status = "input_required"; QJsonObject result; QDateTime created; std::function<void()> cancel; QString tool; };
    std::map<QString, Task> tasks;
    State(AgentTools& self, Workbench& owner, std::function<bool()> admitted) : self(self), owner(owner), admitted(std::move(admitted)) {}
    bool live() const { return alive->load() && admitted() && owner.projectRoot() == utf8(root); }
    DocumentEditorView* openView(const std::string& path) const {
        for (auto* view : owner.editors().views()) {
            const auto physical = owner.m_services.fs.physicalPath(view->path());
            if (physical && *physical == path) return view;
        }
        return nullptr;
    }
    std::optional<Json> scopedLocations(const Json& value) const {
        if (value.isArray()) {
            auto array = Json::array();
            for (const auto& item : value.items()) if (auto scoped = scopedLocations(item)) array.push(std::move(*scoped));
            return array;
        }
        if (!value.isObject()) return value;
        for (const auto* key : {"uri", "targetUri"})
            if (const auto* uri = value.find(key); uri && uri->isString() && !agentWorkspaceResource(owner.m_services.fs, utf8(root), *uri->asString())) return std::nullopt;
        if (const auto* location = value.find("location"); location && !scopedLocations(*location)) return std::nullopt;
        auto object = Json::object();
        for (std::size_t i = 0; i < value.keys().size(); ++i) {
            if (value.keys()[i] == "data") continue; // Keep opaque server handles private to this dispatch.
            if (auto scoped = scopedLocations(value.items()[i])) object.set(value.keys()[i], std::move(*scoped));
        }
        return object;
    }
    void result(const QJsonValue& id, const QString& tool, Json data, Reply reply) {
        if (!live()) { reply(error(id, "workspace consent expired")); return; }
        if (data.dump().size() > 1024 * 1024) { reply(error(id, "tool result exceeds retention bound; narrow the query")); return; }
        auto split = agent::paginate(data);
        if (!split) { reply(error(id, qs(split.error().message()))); return; }
        QString cursor;
        if (split->size() > 1) {
            if (pages.size() >= 8) { reply(error(id, "too many retained result pages")); return; }
            cursor = randomToken();
            pages.emplace(cursor, Page{std::move(*split), 1, tool});
            data = pages.at(cursor).pages.front();
        }
        QJsonObject body{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", qs(data.dump())}}}}, {"isError", false}};
        if (!cursor.isEmpty()) body.insert("nextCursor", cursor);
        reply(answer(id, body));
    }
    std::optional<lang::FileDiagnostics> published(const std::string& file) const {
        const auto* view = openView(file);
        const auto& key = view ? view->path() : file;
        if (QFileInfo(qs(key)).suffix().compare("lua", Qt::CaseInsensitive) == 0)
            return owner.m_lua ? owner.m_lua->diagnostics().forFile(key) : std::nullopt;
        return owner.m_clangd ? owner.m_clangd->diagnostics().forFile(key) : std::nullopt;
    }
    bool fresh(const std::string& file) const {
        const auto* current = openView(file);
        const auto view = current && owner.m_bridge ? owner.m_bridge->find(current->path()) : std::nullopt;
        const auto set = published(file);
        return view && set && !set->stale && set->textVersion == view->snapshot.version();
    }
    void baseline(const std::string& file) {
        if (edited.contains(file) || !fresh(file)) return;
        knownBaseline.insert(file);
        auto& previous = beforeErrors[file]; previous.clear();
        for (const auto& diagnostic : published(file)->diagnostics)
            if (diagnostic.severity == lang::DiagnosticSeverity::Error) previous.insert(diagnostic.code + ":" + diagnostic.message);
    }
    void hook(const QJsonValue& id, const QString& tool, const QJsonObject& arguments, Reply reply) {
        auto args = arguments;
        if (args.value("input").isObject()) args = args.value("input").toObject();
        auto input = args.value("tool_input").toObject();
        const auto file = input.value("file_path").toString();
        if (!file.isEmpty()) {
            auto path = agentWorkspaceResource(owner.m_services.fs, utf8(root), utf8(file));
            if (!path) { reply(error(id, qs(path.error().message()))); return; }
            edited.insert(*path);
        }
        const auto issued = instance;
        auto* wait = new QTimer(&self); wait->setInterval(25);
        auto deadline = std::make_shared<QElapsedTimer>(); deadline->start();
        QObject::connect(wait, &QTimer::timeout, &self, [this, id, tool, args, reply, wait, deadline, issued] {
            if (!live() || instance != issued) { wait->stop(); wait->deleteLater(); reply(error(id, "hook consent expired")); return; }
            const bool ready = std::ranges::all_of(edited, [this](const auto& file) { return fresh(file); });
            if (!ready && deadline->elapsed() < 1500) return;
            wait->stop(); wait->deleteLater();
            if (!ready) { result(id, tool, Json::object().set("warning", "T1 diagnostics have not refreshed at the edited version; no invented errors block completion"), reply); return; }
            std::string errors;
            auto warnings = Json::array();
            for (const auto& file : edited) {
                if (!knownBaseline.contains(file)) { warnings.push(Json::object().set("file", file).set("warning", "no pre-edit diagnostic baseline; new-error comparison unavailable")); continue; }
                const auto set = published(file);
                for (const auto& diagnostic : set->diagnostics) {
                    const auto fingerprint = diagnostic.code + ":" + diagnostic.message;
                    if (diagnostic.severity == lang::DiagnosticSeverity::Error && !beforeErrors[file].contains(fingerprint)) errors += file + ": " + diagnostic.message + "\n";
                    if (diagnostic.severity == lang::DiagnosticSeverity::Error || diagnostic.severity == lang::DiagnosticSeverity::Warning)
                        warnings.push(Json::object().set("file", file).set("message", diagnostic.message).set("code", diagnostic.code).set("severity", static_cast<int>(diagnostic.severity)));
                }
            }
            if (tool == "stop_hook") {
                const auto fingerprint = QCryptographicHash::hash(QByteArray::fromStdString(errors), QCryptographicHash::Sha256).toHex().toStdString();
                result(id, tool, stopHook.evaluate(errors.empty() ? "" : fingerprint, errors, args.value("stop_hook_active").toBool()), reply);
            } else result(id, tool, std::move(warnings), reply);
        });
        wait->start();
    }
    void finish(const QString& task, Result<Json> outcome) {
        const auto found = tasks.find(task);
        if (found == tasks.end() || found->second.status == "cancelled") return;
        found->second.status = outcome ? "completed" : "failed";
        if (outcome) {
            auto split = agent::paginate(*outcome);
            if (!split) outcome = std::unexpected(split.error());
            else {
                QString cursor;
                Json first = split->front();
                if (split->size() > 1) {
                    if (pages.size() >= 8) outcome = fail(ErrorCode::Busy, "retained result capacity reached");
                    else { cursor = randomToken(); pages.emplace(cursor, Page{std::move(*split), 1, found->second.tool}); }
                }
                if (outcome) {
                    found->second.result = {{"isError", false}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", qs(first.dump())}}}}};
                    if (!cursor.isEmpty()) found->second.result.insert("nextCursor", cursor);
                    return;
                }
            }
        }
        found->second.status = "failed";
        found->second.result = {{"isError", true}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", qs(outcome.error().message())}}}}};
    }    void lsp(const QString& method, Json params, std::function<void(Result<Json>)> done) {
        if (!live() || !owner.m_bridge || !owner.m_pool) { done(fail(ErrorCode::Unsupported, "language tool unavailable")); return; }
        auto request = owner.m_bridge->request(utf8(method), std::move(params), stop.get_token());
        QPointer<AgentTools> guard(&self);
        const auto issued = instance;
        auto publish = [guard, issued, done = std::move(done)](Result<Json> result) mutable {
            if (!guard) return;
            QMetaObject::invokeMethod(guard, [guard, issued, done = std::move(done), result = std::move(result)]() mutable {
                if (!guard || !guard->m->live() || guard->m->instance != issued) return;
                done(std::move(result));
            }, Qt::QueuedConnection);
        };
        owner.m_pool->spawn(sched::Lane::UserVisible, languageResult(std::move(request), std::move(publish))).detach();
    }
    Result<Json> diagnostics(const QString& file = {}) {
        auto records = Json::array();
        for (auto* view : owner.editors().views()) {
            auto authorized = agentWorkspaceResource(owner.m_services.fs, utf8(root), view->path());
            if (!authorized || (!file.isEmpty() && *authorized != utf8(file))) continue;
            for (const auto& diagnostic : view->editor().diagnostics()) {
                const auto& text = view->editor().editor().text();
                const auto start = text.utf16PointAt(std::min(diagnostic.range.begin, text.size()));
                const auto end = text.utf16PointAt(std::min(diagnostic.range.end, text.size()));
                records.push(Json::object().set("file", *authorized).set("message", diagnostic.message).set("code", diagnostic.code)
                    .set("severity", static_cast<std::int64_t>(diagnostic.severity)).set("line", start.line.value()).set("character", start.column.value())
                    .set("endLine", end.line.value()).set("endCharacter", end.column.value()).set("textVersion", text.version()));
                if (records.items().size() > 8192) return fail(ErrorCode::OutOfRange, "diagnostic result too large; request one file");
            }
        }
        return records;
    }
    void apply(const QString& task, const Json& proposed, bool cppRename = false) {
        if (!live() || !tasks.contains(task) || tasks.at(task).status == "cancelled") { finish(task, fail(ErrorCode::Cancelled, "workspace consent expired")); return; }
        auto edit = lang::parseWorkspaceEdit(proposed);
        if (!edit) { finish(task, std::unexpected(edit.error())); return; }
        for (const auto& file : edit->files) if (!agentWorkspaceResource(owner.m_services.fs, utf8(root), file.path)) {
            finish(task, fail(ErrorCode::PermissionDenied, "edit escapes consenting workspace")); return;
        }
        if (auto checked = owner.m_bridge->validateRefactoringTargets(*edit); !checked) { finish(task, std::unexpected(checked.error())); return; }
        document::RefactoringPlan plan;
        plan.label = "AI Agent proposed edit";
        for (const auto& file : edit->files) {
            const auto physical = agentWorkspaceResource(owner.m_services.fs, utf8(root), file.path);
            if (!physical) { finish(task, std::unexpected(physical.error())); return; }
            const auto* existing = openView(*physical);
            const auto path = existing ? existing->path() : *physical;
            if (auto opened = owner.openPath(path); !opened) { finish(task, std::unexpected(opened.error())); return; }
            auto current = owner.m_bridge->find(path);
            if (!current || current->changedOnDisk || (file.version && current->lspVersion != file.version)) { finish(task, fail(ErrorCode::Busy, "source version changed before preview")); return; }
            auto after = lang::applyTextEdits(current->snapshot.text(), file.edits, owner.m_bridge->encoding());
            if (!after) { finish(task, std::unexpected(after.error())); return; }
            plan.buffers.push_back({path, current->snapshot.text(), std::move(*after), document::BufferPersistence::Automatic});
        }
        if (cppRename) { auto admitted = owner.admitCppRename(*edit, plan); if (!admitted) { finish(task, std::unexpected(admitted.error())); return; } }
        preview(task, std::move(plan));
    }
    void preview(const QString& task, document::RefactoringPlan plan) {
        const auto issued = instance;
        QPointer<AgentTools> guard(&self);
        std::vector<std::pair<std::string, std::string>> identities;
        for (const auto& file : plan.buffers) {
            auto physical = agentWorkspaceResource(owner.m_services.fs, utf8(root), file.path);
            if (!physical) { finish(task, std::unexpected(physical.error())); return; }
            identities.emplace_back(file.path, *physical);
        }
        auto previousAdmission = std::move(plan.revalidateContext);
        plan.revalidateContext = [guard, task, issued, identities, previousAdmission = std::move(previousAdmission)]() -> Result<void> {
            if (!guard || !guard->m->live() || guard->m->instance != issued || !guard->m->tasks.contains(task) || guard->m->tasks.at(task).status == "cancelled")
                return fail(ErrorCode::Cancelled, "agent edit consent/session changed");
            for (const auto& [path, expected] : identities) {
                const auto physical = agentWorkspaceResource(guard->m->owner.m_services.fs, utf8(guard->m->root), path);
                if (!physical || *physical != expected) return fail(ErrorCode::Busy, "agent edit resource identity changed");
            }
            return previousAdmission ? previousAdmission() : Result<void>{};
        };
        QDialog dialog;
        dialog.setWindowTitle("Review AI Agent changes");
        auto* layout = new QVBoxLayout(&dialog);
        auto* tabs = new QTabWidget(&dialog);
        for (const auto& file : plan.buffers) {
            if (file.expectedText.size() > 1024 * 1024 || file.afterText.size() > 1024 * 1024) { finish(task, fail(ErrorCode::OutOfRange, "edit exceeds review size")); return; }
            auto diff = vcs::SideBySideDiff::compute(file.expectedText, file.afterText);
            if (!diff) { finish(task, std::unexpected(diff.error())); return; }
            auto* viewer = new ui::DiffViewer(tabs);
            viewer->setDiff(std::move(*diff), "Current workspace text", "Proposed change");
            tabs->addTab(viewer, qs(file.path));
        }
        layout->addWidget(tabs);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(&self, &QObject::destroyed, &dialog, &QDialog::reject);
        QTimer lease;
        lease.setInterval(75);
        QObject::connect(&lease, &QTimer::timeout, &dialog, [&] { if (!plan.revalidateContext()) dialog.reject(); });
        lease.start();
        dialog.resize(1100, 750);
        const bool approved = dialog.exec() == QDialog::Accepted;
        if (!guard) return;
        if (!approved || !plan.revalidateContext()) { finish(task, fail(ErrorCode::Cancelled, "agent changes rejected or expired")); return; }
        for (const auto& file : plan.buffers) { auto path = agentWorkspaceResource(owner.m_services.fs, utf8(root), file.path); if (path) { baseline(*path); edited.insert(*path); } }
        auto applied = owner.applyRefactoring(plan);
        finish(task, applied ? Result<Json>(Json::object().set("applied", true).set("status", static_cast<int>(applied->status))) : Result<Json>(std::unexpected(applied.error())));
    }    void mutation(const QString& task, const QString& tool, const QJsonObject& args) {
        if (!live() || !tasks.contains(task) || tasks.at(task).status == "cancelled") return;
        QPointer<AgentTools> guard(&self);
        QMessageBox approval(QMessageBox::Question, "AI Agent tool request", "Allow this operation in " + root + "?\n" + tool + "\n" + QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), QMessageBox::Yes | QMessageBox::No);
        approval.setTextFormat(Qt::PlainText);
        approval.setDefaultButton(QMessageBox::No);
        QObject::connect(&self, &QObject::destroyed, &approval, &QDialog::reject);
        const bool accepted = approval.exec() == QMessageBox::Yes;
        if (!guard) return;
        if (!accepted || !live() || !tasks.contains(task) || tasks.at(task).status == "cancelled") { finish(task, fail(ErrorCode::Cancelled, "tool request rejected or expired")); return; }
        tasks.at(task).status = "working";
        if (tool == "build") {
            if (!owner.build().canBuild()) { finish(task, fail(ErrorCode::Busy, "build unavailable or already running")); return; }
            const auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = QObject::connect(&owner.build(), &ui::BuildController::buildFinished, &self, [this, task, connection](const build::BuildOutcome& outcome) {
                QObject::disconnect(*connection);
                finish(task, Json::object().set("succeeded", outcome.succeeded()).set("source", "build-tool-outcome"));
            });
            auto started = owner.build().build(build::BuildAction::Build);
            if (!started) { QObject::disconnect(*connection); finish(task, std::unexpected(started.error())); return; }
            tasks.at(task).cancel = [this] { owner.build().cancel(); };
            return;
        }
        if (tool.startsWith("debug_")) {
            auto* debugger = owner.debugController();
            if (!debugger) { finish(task, fail(ErrorCode::Unsupported, "debugger unavailable")); return; }
            if (tool == "debug_start") { auto started = debugger->debug(); finish(task, started ? Result<Json>(Json::object().set("accepted", true).set("state", static_cast<int>(debugger->state()))) : Result<Json>(std::unexpected(started.error()))); }
            else if (tool == "debug_stop") { debugger->stop(); finish(task, Json::object().set("accepted", true).set("state", static_cast<int>(debugger->state()))); }
            else { const bool accepted = tool == "debug_continue" ? debugger->resume() : debugger->pause(); finish(task, accepted ? Result<Json>(Json::object().set("accepted", true).set("state", static_cast<int>(debugger->state()))) : Result<Json>(fail(ErrorCode::Busy, "debugger command refused"))); }
            return;
        }
        if (tool == "run_tests") {
            auto* widget = owner.m_window ? owner.m_window->toolWindows().content(kUnitTestsToolWindow) : nullptr;
            auto started = runUnitTestsForAgent(widget);
            if (!started) { finish(task, std::unexpected(started.error())); return; }
            auto* timer = new QTimer(&self);
            timer->setInterval(100);
            QPointer<QWidget> window(widget);
            const auto generation = *started;
            tasks.at(task).cancel = [window, generation] { if (window) cancelUnitTestsForAgent(window, generation); };
            QObject::connect(timer, &QTimer::timeout, &self, [this, task, timer, window, generation] {
                if (!live() || !tasks.contains(task) || tasks.at(task).status == "cancelled") { timer->stop(); timer->deleteLater(); return; }
                auto snapshot = unitTestsAgentSnapshot(window);
                if (!snapshot) { timer->stop(); timer->deleteLater(); finish(task, std::unexpected(snapshot.error())); return; }
                if (snapshot->generation != generation) { timer->stop(); timer->deleteLater(); finish(task, fail(ErrorCode::Cancelled, "test run changed")); return; }
                if (snapshot->busy) return;
                auto results = Json::array();
                for (const auto& result : snapshot->results) results.push(Json::object().set("name", result.name).set("outcome", std::string(run::toString(result.outcome))).set("reason", result.reason));
                timer->stop(); timer->deleteLater(); finish(task, std::move(results));
            });
            timer->start();
            return;
        }
        auto path = agentWorkspaceResource(owner.m_services.fs, utf8(root), utf8(args.value("file").toString()));
        if (!path) { finish(task, std::unexpected(path.error())); return; }
        if (tool == "propose_edit") {
            if (!args.value("expectedText").isString() || !args.value("newText").isString()) { finish(task, fail(ErrorCode::InvalidArgument, "edit requires concrete expectedText/newText")); return; }
            const auto* existing = openView(*path);
            const auto editorPath = existing ? existing->path() : *path;
            if (auto opened = owner.openPath(editorPath); !opened) { finish(task, std::unexpected(opened.error())); return; }
            auto current = owner.m_bridge->find(editorPath);
            const auto expected = utf8(args.value("expectedText").toString());
            auto after = utf8(args.value("newText").toString());
            if (!current || current->changedOnDisk) { finish(task, fail(ErrorCode::Busy, "source is unavailable or changed on disk")); return; }
            const auto before = current->snapshot.text();
            if (args.contains("offset")) {
                const auto offset = args.value("offset").toInteger(-1);
                if (offset < 0 || static_cast<std::uint64_t>(offset) > before.size() ||
                    before.substr(static_cast<std::size_t>(offset), expected.size()) != expected ||
                    !io::isValidUtf8(std::string_view(before).substr(0, static_cast<std::size_t>(offset)))) { finish(task, fail(ErrorCode::Busy, "proposal text slice does not match current source")); return; }
                after = before.substr(0, static_cast<std::size_t>(offset)) + after + before.substr(static_cast<std::size_t>(offset) + expected.size());
            } else if (before != expected) { finish(task, fail(ErrorCode::Busy, "current source differs from proposal baseline")); return; }
            if (after.size() > 512 * 1024 || after.find(char(0)) != std::string::npos) { finish(task, fail(ErrorCode::OutOfRange, "invalid or excessive proposal text")); return; }
            document::RefactoringPlan plan;
            plan.label = "AI Agent proposed edit";
            plan.buffers.push_back({editorPath, before, after, document::BufferPersistence::Automatic});
            lang::WorkspaceEdit target; target.files.push_back(lang::FileEdit{editorPath, {}, {}, {}});
            if (auto checked = owner.m_bridge->validateRefactoringTargets(target); !checked) { finish(task, std::unexpected(checked.error())); return; }
            preview(task, std::move(plan)); return;
        }
        const auto line = args.value("line").toInt(-1), character = args.value("character").toInt(-1);
        if (line < 0 || character < 0) { finish(task, fail(ErrorCode::InvalidArgument, "invalid source position")); return; }
        auto params = Json::object().set("textDocument", Json::object().set("uri", lang::pathToUri(*path).value_or("")))
            .set("position", Json::object().set("line", line).set("character", character));
        if (tool == "rename_symbol") {
            params.set("newName", utf8(args.value("newName").toString()));
            const auto newName = utf8(args.value("newName").toString());
            const auto source = *path;
            lsp("textDocument/rename", std::move(params), [this, task, source, line, character, newName](Result<Json> result) {
                if (!result) { finish(task, std::unexpected(result.error())); return; }
                const auto suffix = QFileInfo(qs(source)).suffix().toLower();
                if (QStringList{"c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx"}.contains(suffix)) {
                    auto edit = lang::parseWorkspaceEdit(*result);
                    if (!edit) { finish(task, std::unexpected(edit.error())); return; }
                    auto proposed = std::move(*result);
                    QPointer<AgentTools> guard(&self);
                    const auto issued = instance;
                    owner.checkCppRename(source, {static_cast<std::uint32_t>(line), static_cast<std::uint32_t>(character)}, newName, *edit, *edit, stop.get_token(), [guard, issued, task, proposed = std::move(proposed)](Result<void> checked) {
                        if (!guard || !guard->m->live() || guard->m->instance != issued) return;
                        if (!checked) guard->m->finish(task, std::unexpected(checked.error())); else guard->m->apply(task, proposed, true);
                    });
                } else apply(task, *result);
            });
        } else {
            params.set("range", Json::object().set("start", *params.find("position")).set("end", *params.find("position"))).set("context", Json::object().set("diagnostics", Json::array()));
            const auto title = args.value("title").toString();
            lsp("textDocument/codeAction", std::move(params), [this, task, title](Result<Json> result) {
                if (!result) { finish(task, std::unexpected(result.error())); return; }
                if (result->isArray()) for (const auto& action : result->items()) if (action.stringAt("title") == utf8(title)) {
                    if (const auto* edit = action.find("edit")) { apply(task, *edit); return; }
                }
                finish(task, fail(ErrorCode::Unsupported, "no exact engine-provided edit-only quick fix matches; use the IDE intention chooser"));
            });
        }
    }
};
AgentTools::AgentTools(Workbench& owner, std::function<bool()> admission, QObject* parent)
    : QObject(parent ? parent : &owner), m(std::make_unique<State>(*this, owner, std::move(admission))) {}
AgentTools::~AgentTools() { stop(); }
QString AgentTools::mcpConfig() const { return m->config; }
QString AgentTools::hookSettings() const { return m->hooks; }
Result<void> AgentTools::start(const QString& root, const QString& configDirectory) {
    stop();
    if (!m->admitted() || root.isEmpty()) return fail(ErrorCode::Untrusted, "MCP requires current workspace consent");
    const auto folder = QDir(configDirectory).filePath("frostide");
    if (auto secured = ensureAgentPrivateDirectory(utf8(folder)); !secured) return secured;
    cleanC2Records(folder);
    m->root = root;
    for (auto* view : m->owner.editors().views()) { const auto path = agentWorkspaceResource(m->owner.m_services.fs, utf8(root), view->path()); if (path) m->baseline(*path); }
    m->token = randomToken(); m->instance = randomToken();
    const auto key = QCryptographicHash::hash((root + m->instance).toUtf8(), QCryptographicHash::Sha256).toHex().toStdString();
    auto claimed = io::LocalIpcOwner::claim(key);
    if (!claimed) return std::unexpected(claimed.error());
    m->listener = std::move(*claimed);
    m->stop = std::stop_source{};
    m->alive = std::make_shared<std::atomic<bool>>(true);
    const auto alive = m->alive;
    QPointer<AgentTools> guard(this);
    auto serving = m->listener->start([guard, alive](std::string input) -> Result<std::string> {
        if (!alive->load()) return fail(ErrorCode::Cancelled, "MCP consent ended");
        auto waiter = std::make_shared<Waiter>();
        if (!guard || !QMetaObject::invokeMethod(guard, [guard, waiter, input = std::move(input)] {
            { std::lock_guard lock(waiter->mutex); if (waiter->done) return; }
            auto envelope = QJsonDocument::fromJson(QByteArray::fromStdString(input)).object();
            if (!guard || !guard->m->live() || !sameToken(envelope.value("token").toString(), guard->m->token) ||
                envelope.value("root").toString() != guard->m->root || envelope.value("instance").toString() != guard->m->instance) {
                std::lock_guard lock(waiter->mutex); waiter->result = fail(ErrorCode::PermissionDenied, "invalid MCP consent/session token"); waiter->done = true; waiter->changed.notify_all(); return;
            }
            guard->request(envelope.value("request").toObject(), [waiter](QJsonObject response) {
                std::lock_guard lock(waiter->mutex);
                if (waiter->done) return;
                auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
                waiter->result = bytes.size() <= 65536 ? Result<std::string>(std::move(bytes)) : Result<std::string>(fail(ErrorCode::OutOfRange, "MCP response exceeds IPC bound"));
                waiter->done = true; waiter->changed.notify_all();
            });
        }, Qt::QueuedConnection)) return fail(ErrorCode::Cancelled, "MCP owner disappeared");
        std::unique_lock lock(waiter->mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!waiter->done && alive->load() && std::chrono::steady_clock::now() < deadline) waiter->changed.wait_for(lock, std::chrono::milliseconds(10));
        if (!waiter->done) { waiter->done = true; return fail(ErrorCode::Timeout, "MCP owner busy or language tool timed out"); }
        return std::move(waiter->result);
    });
    if (!serving) { stop(); return serving; }
    m->discovery = QDir(folder).filePath(m->instance + ".discovery.json");
    m->config = QDir(folder).filePath(m->instance + ".mcp.json");
    m->hooks = QDir(folder).filePath(m->instance + ".hooks.json");
    const auto executable = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef _WIN32
        "frostide-mcp.exe"
#else
        "frostide-mcp"
#endif
    );
    if (!QFileInfo(executable).isExecutable()) { stop(); return fail(ErrorCode::NotFound, "the staged frostide-mcp proxy is unavailable"); }
    const QJsonObject record{{"kind", "frostide-mcp"}, {"pid", QCoreApplication::applicationPid()}, {"root", root}, {"instance", m->instance},
        {"key", qs(key)}, {"token", m->token}, {"expires", QDateTime::currentSecsSinceEpoch() + 8 * 60 * 60}};
    const QJsonObject config{{"mcpServers", QJsonObject{{"frostide", QJsonObject{{"type", "stdio"}, {"command", executable},
        {"args", QJsonArray{"--discovery", m->discovery, "--root", root}}}}}}};
    const auto hook = [](const QString& tool) { return QJsonObject{{"type", "mcp_tool"}, {"server_name", "frostide"}, {"tool_name", tool}}; };
    const QJsonObject hooks{{"hooks", QJsonObject{
        {"PostToolUse", QJsonArray{QJsonObject{{"matcher", "Edit|Write"}, {"hooks", QJsonArray{hook("post_edit_diagnostics")}}}}},
        {"Stop", QJsonArray{QJsonObject{{"hooks", QJsonArray{hook("stop_hook")}}}}}}}};
    for (const auto& [path, data] : std::array<std::pair<QString, QJsonObject>, 3>{{{m->discovery, record}, {m->config, config}, {m->hooks, hooks}}}) {
        auto written = writeAgentPrivateFile(utf8(path), QJsonDocument(data).toJson(QJsonDocument::Compact).toStdString());
        if (!written) { stop(); return written; }
        m->ownedFiles.push_back(path);
    }
    return {};
}
void AgentTools::stop() {
    m->alive->store(false);
    m->stop.request_stop();
    for (auto& [id, task] : m->tasks) { (void)id; if (task.status == "working" && task.cancel) task.cancel(); }
    if (m->listener) { m->listener->stop(); m->listener.reset(); }
    for (const auto& path : m->ownedFiles) QFile::remove(path);
    m->ownedFiles.clear();
    m->discovery.clear(); m->config.clear(); m->hooks.clear(); m->token.clear(); m->tasks.clear(); m->pages.clear(); m->stopHook.reset(); m->beforeErrors.clear(); m->knownBaseline.clear(); m->edited.clear();
}
void AgentTools::request(const QJsonObject& request, Reply reply) {
    const auto id = request.value("id");
    if (!m->live() || request.value("jsonrpc") != "2.0") { reply(error(id, "workspace consent/session unavailable")); return; }
    const auto method = request.value("method").toString();
    const auto params = request.value("params").toObject();
    if (method == "initialize") {
        reply(answer(id, {{"protocolVersion", "2026-07-28"}, {"serverInfo", QJsonObject{{"name", "frostide"}, {"version", "1"}}},
            {"instructions", "Tools report real IDE state in the consenting workspace. Mutations require explicit user approval and reviewed transactions. Follow nextCursor for bounded result pages. Never request credentials or bypass trust."},
            {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"tasks", QJsonObject{{"requests", QJsonObject{{"tools", QJsonObject{{"call", QJsonObject{}}}}}}}}}}})); return;
    }
    if (method == "notifications/poll") {
        auto* debugger = m->owner.debugController();
        const auto stamp = debugger ? QString::number(debugger->sessionGeneration()) + ":" + QString::number(static_cast<int>(debugger->state())) : QStringLiteral("unavailable");
        if (stamp != m->debuggerStamp) { m->debuggerStamp = stamp; ++m->notificationRevision; }
        QJsonArray events;
        if (params.value("after").toInteger() < m->notificationRevision) events.append(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
        reply(answer(id, {{"revision", m->notificationRevision}, {"notifications", events}})); return;
    }
    if (method == "ping" || method.startsWith("notifications/")) { reply(answer(id, {})); return; }
    if (method == "tools/list") {
        auto catalog = qjson(agent::mcpToolCatalog());
        auto tools = catalog.value("tools").toArray();
        for (const auto* name : {"post_edit_diagnostics", "stop_hook"}) tools.append(QJsonObject{{"name", name}, {"description", "Session-scoped read-only IDE diagnostic hook"}, {"inputSchema", QJsonObject{{"type", "object"}}}, {"annotations", QJsonObject{{"readOnlyHint", true}}}});
        catalog.insert("tools", tools); reply(answer(id, catalog)); return;
    }
    if (method.startsWith("tasks/")) {
        const auto task = params.value("taskId").toString();
        if (!m->tasks.contains(task)) { reply(error(id, "unknown or expired task")); return; }
        auto& item = m->tasks.at(task);
        if (method == "tasks/cancel") {
            if (item.status == "working" && item.cancel) item.cancel();
            if (item.status != "completed" && item.status != "failed") item.status = "cancelled";
        }
        if (method == "tasks/result") {
            if (item.status != "completed" && item.status != "failed") { reply(error(id, "task has not produced a result")); return; }
            reply(answer(id, item.result)); return;
        }
        reply(answer(id, {{"task", QJsonObject{{"taskId", task}, {"status", item.status}, {"createdAt", item.created.toString(Qt::ISODateWithMs)}, {"ttl", 8 * 60 * 60 * 1000}, {"pollInterval", 250}}}})); return;
    }
    if (method != "tools/call") { reply(error(id, "unsupported MCP method", -32601)); return; }
    const auto tool = params.value("name").toString();
    const auto args = params.value("arguments").toObject();
    bool known = tool == "post_edit_diagnostics" || tool == "stop_hook";
    const auto catalog = agent::mcpToolCatalog();
    for (const auto& descriptor : catalog.find("tools")->items()) if (descriptor.stringAt("name") == utf8(tool)) known = true;
    if (!known) { reply(error(id, "unknown tool", -32601)); return; }
    const auto cursor = args.value("cursor").toString();
    if (!cursor.isEmpty()) {
        if (!m->pages.contains(cursor) || m->pages.at(cursor).tool != tool) { reply(error(id, "invalid result cursor")); return; }
        auto& pages = m->pages.at(cursor);
        const auto data = pages.pages[pages.next++];
        QJsonObject body{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", qs(data.dump())}}}}, {"isError", false}};
        if (pages.next < pages.pages.size()) body.insert("nextCursor", cursor); else m->pages.erase(cursor);
        reply(answer(id, body)); return;
    }
    const bool mutation = tool == "propose_edit" || tool == "rename_symbol" || tool == "apply_quick_fix" || tool == "build" || tool == "run_tests" || tool.startsWith("debug_");
    if (mutation) {
        if (m->tasks.size() >= 64) { reply(error(id, "task capacity reached")); return; }
        const auto task = randomToken();
        m->tasks.emplace(task, State::Task{"input_required", {}, QDateTime::currentDateTimeUtc(), {}, tool});
        reply(answer(id, {{"task", QJsonObject{{"taskId", task}, {"status", "input_required"}, {"ttl", 8 * 60 * 60 * 1000}, {"pollInterval", 250}}}}));
        QTimer::singleShot(0, this, [this, task, tool, args] { m->mutation(task, tool, args); }); return;
    }
    if (tool == "read_file") {
        auto path = agentWorkspaceResource(m->owner.m_services.fs, utf8(m->root), utf8(args.value("file").toString()));
        if (!path) { reply(error(id, qs(path.error().message()))); return; }
        auto bytes = m->owner.m_services.fs.readFileSnapshot(*path, 1024 * 1024);
        if (!bytes) { reply(error(id, qs(bytes.error().message()))); return; }
        if (auto* view = m->openView(*path)) bytes = view->editor().editor().text().text();
        const auto offset = args.value("offset").toInteger(0), limit = args.value("limit").toInteger(8000);
        if (offset < 0 || limit < 1 || limit > 8000 || static_cast<std::uint64_t>(offset) > bytes->size() || bytes->find(char(0)) != std::string::npos || !io::isValidUtf8(*bytes)) { reply(error(id, "invalid text slice or non-text file")); return; }
        const auto remaining = std::string_view(*bytes).substr(static_cast<std::size_t>(offset));
        const auto size = io::validUtf8Prefix(remaining.substr(0, static_cast<std::size_t>(limit)));
        if (!size && !remaining.empty()) { reply(error(id, "offset/limit splits UTF-8 text")); return; }
        m->result(id, tool, Json::object().set("file", *path).set("text", std::string(remaining.substr(0, size))).set("nextOffset", static_cast<std::uint64_t>(offset) + size).set("eof", static_cast<std::uint64_t>(offset) + size == bytes->size()), std::move(reply)); return;
    }
    if (tool == "post_edit_diagnostics" || tool == "stop_hook") { m->hook(id, tool, args, std::move(reply)); return; }
    if (tool == "file_problems" || tool == "workspace_diagnostics") {
        QString file;
        if (tool == "file_problems") {
            auto path = agentWorkspaceResource(m->owner.m_services.fs, utf8(m->root), utf8(args.value("file").toString()));
            if (!path) { reply(error(id, qs(path.error().message()))); return; }
            file = qs(*path);
        }
        auto data = m->diagnostics(file);
        if (!data) { reply(error(id, qs(data.error().message()))); return; }
        m->result(id, tool, std::move(*data), std::move(reply));
        return;
    }
    if (tool == "project_model") {
        auto entries = Json::array();
        if (auto solution = m->owner.solution()) for (const auto& project : solution->projects()) entries.push(Json::object().set("name", project.name).set("path", project.path).set("kind", std::string(project::toString(project.kind))));
        else if (m->owner.m_cmake && m->owner.m_cmake->shownModel()) for (const auto& target : m->owner.m_cmake->shownModel()->targets) entries.push(Json::object().set("name", target.name).set("kind", "cmake-target"));
        else { reply(error(id, "project model unavailable")); return; }
        m->result(id, tool, std::move(entries), std::move(reply)); return;
    }
    if (tool == "build_status") {
        auto data = Json::object().set("state", static_cast<int>(m->owner.build().state()));
        if (const auto& outcome = m->owner.build().lastOutcome()) data.set("lastSucceeded", outcome->succeeded());
        m->result(id, tool, std::move(data), std::move(reply)); return;
    }
    if (tool == "debugger_state") {
        auto* debugger = m->owner.debugController();
        if (!debugger) { reply(error(id, "debugger unavailable")); return; }
        m->result(id, tool, Json::object().set("state", static_cast<int>(debugger->state())).set("generation", debugger->sessionGeneration()).set("status", utf8(debugger->statusText())), std::move(reply)); return;
    }
    if (tool == "test_results") {
        auto* window = m->owner.m_window ? m->owner.m_window->toolWindows().content(kUnitTestsToolWindow) : nullptr;
        auto snapshot = unitTestsAgentSnapshot(window);
        if (!snapshot) { reply(error(id, qs(snapshot.error().message()))); return; }
        auto data = Json::array();
        for (const auto& result : snapshot->results) data.push(Json::object().set("name", result.name).set("outcome", std::string(run::toString(result.outcome))).set("reason", result.reason));
        m->result(id, tool, std::move(data), std::move(reply)); return;
    }
    if (tool == "inspection_info") {
        auto info = agent::inspectionInfo(utf8(args.value("id").toString()));
        if (!info) { reply(error(id, qs(info.error().message()))); return; }
        const auto setting = info->stringAt("settingKey");
        info->set("configuredSeverity", m->owner.m_services.settings.snapshot()->get(settings::Key<std::string>{setting}));
        m->result(id, tool, std::move(*info), std::move(reply)); return;
    }
    auto paramsJson = Json::object();
    QString methodName;
    if (tool == "symbol_search") { methodName = "workspace/symbol"; paramsJson.set("query", utf8(args.value("query").toString())); }
    else {
        auto path = agentWorkspaceResource(m->owner.m_services.fs, utf8(m->root), utf8(args.value("file").toString()));
        const auto line = args.value("line").toInt(-1), character = args.value("character").toInt(-1);
        if (!path || line < 0 || character < 0) { reply(error(id, "invalid or unauthorized source position")); return; }
        paramsJson.set("textDocument", Json::object().set("uri", lang::pathToUri(*path).value_or(""))).set("position", Json::object().set("line", line).set("character", character));
        if (tool == "find_usages") { methodName = "textDocument/references"; paramsJson.set("context", Json::object().set("includeDeclaration", true)); }
        else if (tool == "go_to_definition") methodName = "textDocument/definition";
        else if (tool == "type_hierarchy") methodName = "textDocument/prepareTypeHierarchy";
        else if (tool == "call_hierarchy") methodName = "textDocument/prepareCallHierarchy";
        else { reply(error(id, "unknown tool", -32601)); return; }
    }
    m->lsp(methodName, std::move(paramsJson), [this, id, tool, reply = std::move(reply)](Result<Json> value) mutable {
        if (!value) reply(error(id, qs(value.error().message())));
        else { auto scoped = m->scopedLocations(*value); m->result(id, tool, scoped ? std::move(*scoped) : Json::array(), std::move(reply)); }
    });
}
}
