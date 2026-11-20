// SPDX-License-Identifier: Apache-2.0
// C3 has no automatic user turns: explicit consent/connect, actual init, Send, scoped MCP approvals.
#include "native_agent_chat.hpp"
#include "agent_private.hpp"
#include "agent_tools.hpp"
#include "agent_scope.hpp"
#include "frost/agent/native_session.hpp"
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTextCursor>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <atomic>
#include <thread>
namespace frost::app {
namespace {
using Json = agent::Json;
QString qs(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
Json json(const QJsonObject& value) { return rpc::parseJson(QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString()).value_or(Json::object()); }
QJsonObject qjson(const Json& value) { return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump())).object(); }
}
struct NativeAgentChat::State {
    NativeAgentChat& self;
    Workbench& owner;
    QPointer<QDialog> panel;
    QPointer<QPlainTextEdit> transcript, prompt;
    QPointer<QLineEdit> model, resumeAt;
    QPointer<QComboBox> sessions, effort, checkpoints;
    QPointer<QCheckBox> consent, fork;
    QPointer<QLabel> status, usage;
    QPointer<QPushButton> send;
    std::unique_ptr<AgentTools> tools;
    std::unique_ptr<agent::NativeSession> process;
    agent::NativeProtocol protocol;
    std::jthread probe;
    std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t generation = 0, trustRevision = 0;
    QString root, executable, configDirectory;
    QTimer timer;
    std::map<QString, std::shared_ptr<bool>> permissions;
    std::vector<Json> earlyEvents;
    std::size_t earlyBytes = 0;
    QString rewindRequest, rewindMessage;
    bool rewindDryRun = false;
    State(NativeAgentChat& self, Workbench& owner) : self(self), owner(owner) {}
    bool admitted() const {
        if (!live->load() || !consent || !consent->isChecked() || owner.m_shutDown || !owner.agentBridgeActive() || owner.projectRoot() != utf8(root)) return false;
        const auto trust = owner.m_services.trust.snapshot(utf8(root));
        return trust.trusted && trust.revision == trustRevision;
    }
    void append(const QString& text) {
        if (!transcript) return;
        // Display is bounded. No session cost/token values are synthesized when absent.
        if (transcript->document()->characterCount() > 1024 * 1024) transcript->clear();
        auto cursor = transcript->textCursor(); cursor.movePosition(QTextCursor::End); cursor.insertText(text); transcript->setTextCursor(cursor);
    }
    void response(const QString& id, Json value, bool permission) {
        if (!admitted() || !process || !permissions.contains(id) || *permissions.at(id)) return;
        *permissions.at(id) = true;
        permissions.erase(id);
        auto response = Json::object().set("subtype", "success").set("request_id", utf8(id)).set("response", std::move(value));
        if (auto written = process->send(Json::object().set("type", "control_response").set("response", std::move(response))); !written) append(qs(written.error().message()) + "\n");
        (void)permission;
    }
    void permission(const QString& id, const Json& request) {
        const auto tool = request.stringAt("tool_name");
        const auto* input = request.find("input");
        if (!input || !input->isObject()) { response(id, Json::object().set("behavior", "deny").set("message", "invalid permission input"), true); return; }
        if (tool.starts_with("mcp__frostide__")) {
            // The separately authenticated C2 endpoint performs resource admission and concrete
            // user approval. This does not grant a native CLI file/shell permission.
            response(id, Json::object().set("behavior", "allow").set("updatedInput", *input), true); return;
        }
        if (tool != "AskUserQuestion" && tool != "ExitPlanMode") {
            response(id, Json::object().set("behavior", "deny").set("message", "Native filesystem/shell tools are disabled. Use scoped frostide MCP read_file/propose_edit and approved project tools"), true); return;
        }
        QPointer<NativeAgentChat> guard(&self);
        const auto epoch = generation;
        QDialog dialog;
        dialog.setWindowTitle(tool == "AskUserQuestion" ? "AI Agent question" : "Review AI Agent plan");
        auto* layout = new QVBoxLayout(&dialog);
        auto updated = *input;
        std::vector<std::pair<std::string, QLineEdit*>> answers;
        if (tool == "AskUserQuestion") {
            const auto* questions = input->find("questions");
            if (!questions || !questions->isArray() || questions->items().size() > 8) { response(id, Json::object().set("behavior", "deny").set("message", "invalid question cards"), true); return; }
            for (const auto& question : questions->items()) {
                const auto label = std::string(question.stringAt("question"));
                auto* questionLabel = new QLabel(qs(label), &dialog); questionLabel->setTextFormat(Qt::PlainText); layout->addWidget(questionLabel);
                if (const auto* options = question.find("options"); options && options->isArray()) {
                    QStringList choices;
                    for (const auto& option : options->items()) choices << qs(option.stringAt("label")) + " — " + qs(option.stringAt("description"));
                    auto* optionLabel = new QLabel(choices.join("\n"), &dialog); optionLabel->setTextFormat(Qt::PlainText); layout->addWidget(optionLabel);
                }
                auto* edit = new QLineEdit(&dialog); edit->setAccessibleName(qs(label)); layout->addWidget(edit); answers.emplace_back(label, edit);
            }
        } else {
            if (input->stringAt("plan").empty() || input->stringAt("plan").size() > 256 * 1024) { response(id, Json::object().set("behavior", "deny").set("message", "a concrete bounded plan must be supplied for review"), true); return; }
            auto* plan = new QPlainTextEdit(&dialog); plan->setReadOnly(true); plan->setPlainText(qs(input->stringAt("plan"))); layout->addWidget(plan);
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(&self, &QObject::destroyed, &dialog, &QDialog::reject);
        QTimer lease;
        lease.setInterval(75);
        QObject::connect(&lease, &QTimer::timeout, &dialog, [&] { if (!guard || !admitted() || generation != epoch || !permissions.contains(id)) dialog.reject(); });
        lease.start();
        const bool approved = dialog.exec() == QDialog::Accepted;
        if (!guard) return;
        if (!approved || !admitted() || generation != epoch) { response(id, Json::object().set("behavior", "deny").set("message", "user rejected or session expired"), true); return; }
        if (!answers.empty()) { auto data = Json::object(); for (const auto& [key, edit] : answers) data.set(key, utf8(edit->text())); updated.set("answers", std::move(data)); }
        response(id, Json::object().set("behavior", "allow").set("updatedInput", std::move(updated)), true);
    }
    void event(Json value) {
        if (!admitted()) return;
        if (!process) {
            const auto bytes = value.dump().size();
            if (earlyEvents.size() >= 32 || earlyBytes + bytes > 1024 * 1024) { self.stop(); return; }
            earlyBytes += bytes; earlyEvents.push_back(std::move(value)); return;
        }
        auto accepted = protocol.receive(value);
        if (!accepted) { append(qs(accepted.error().message()) + "\n"); self.stop(); return; }
        const auto type = value.stringAt("type");
        if (type == "user" && checkpoints) {
            const auto id = qs(value.stringAt("uuid"));
            if (!id.isEmpty() && id.size() <= 256 && checkpoints->findData(id) < 0) checkpoints->addItem(id, id);
        }
        if (type == "control_response") {
            const auto* response = value.find("response");
            if (response && qs(response->stringAt("request_id")) == rewindRequest) {
                const auto* data = response->find("response");
                const auto message = rewindMessage;
                rewindRequest.clear();
                if (response->stringAt("subtype") != "success" || !data) { append("CLI rewind was not acknowledged; no fork was started\n"); return; }
                if (rewindDryRun) {
                    if (!data->boolAt("can_rewind").value_or(false)) { append("CLI reported no rewind checkpoint. MCP edits use IDE Undo\n"); return; }
                    QStringList files;
                    if (const auto* changed = data->find("files_changed"); changed && changed->isArray()) for (const auto& file : changed->items()) {
                        if (!file.isString() || !agentWorkspaceResource(owner.m_services.fs, utf8(root), *file.asString())) { append("Rewind would access an unauthorized resource; refused\n"); return; }
                        files << qs(*file.asString());
                    }
                    if (files.isEmpty()) { append("No CLI file changes are recorded. MCP transactions remain available through IDE Undo\n"); return; }
                    QPointer<NativeAgentChat> guard(&self);
                    const auto epoch = generation;
                    QMessageBox preview(QMessageBox::Question, "Rewind CLI checkpoint and fork", "Rewind these CLI-owned file changes, then reconnect as a fresh authenticated fork? MCP edits are separate IDE transactions.\n" + files.join("\n"), QMessageBox::Yes | QMessageBox::No);
                    preview.setTextFormat(Qt::PlainText);
                    preview.setDefaultButton(QMessageBox::No);
                    QObject::connect(&self, &QObject::destroyed, &preview, &QDialog::reject);
                    const bool approved = preview.exec() == QMessageBox::Yes;
                    if (!guard || !approved || !admitted() || generation != epoch) return;
                    for (const auto& file : files) if (!agentWorkspaceResource(owner.m_services.fs, utf8(root), utf8(file))) return;
                    auto control = protocol.control("rewind_files", Json::object().set("user_message_id", utf8(message)).set("dry_run", false), std::chrono::steady_clock::now());
                    if (!control) { append(qs(control.error().message()) + "\n"); return; }
                    rewindDryRun = false; rewindMessage = message; rewindRequest = qs(control->stringAt("request_id"));
                    (void)process->send(*control);
                } else {
                    const auto session = qs(protocol.sessionId());
                    if (sessions->findData(session) < 0) sessions->addItem(session, session);
                    sessions->setCurrentIndex(sessions->findData(session)); fork->setChecked(true); resumeAt->setText(message);
                    append("CLI rewind acknowledged. Reconnecting fork with fresh authentication; no prompt is submitted\n");
                    QTimer::singleShot(0, &self, [this] { start(); });
                }
            } else if (response && usage) usage->setText("CLI control result: " + qs(response->dump()).left(4096));
            return;
        }
        if (type == "system" && value.stringAt("subtype") == "init") {
            if (status) status->setText(protocol.ready() ? "Native CLI, IDE and frostide MCP connected" : "Waiting for actual IDE/MCP initialization");
            if (send) send->setEnabled(protocol.ready());
            // Own index entries contain identities only; never import or mutate the CLI's JSONL.
            if (!protocol.sessionId().empty() && tools) {
                const auto path = QDir(configDirectory).filePath("frostide/" + QString::number(generation) + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".session.json");
                const auto data = Json::object().set("kind", "frostide-session").set("root", utf8(root)).set("session", protocol.sessionId()).dump();
                (void)writeAgentPrivateFile(utf8(path), data);
            }
        } else if (type == "assistant") {
            if (const auto* message = value.find("message")) if (const auto* content = message->find("content"); content && content->isArray())
                for (const auto& block : content->items()) if (block.stringAt("type") == "text") append(qs(block.stringAt("text")) + "\n");
        } else if (type == "stream_event") {
            if (const auto* event = value.find("event")) if (const auto* delta = event->find("delta")) append(qs(delta->stringAt("text")));
        } else if (type == "result") {
            if (const auto* cost = value.find("total_cost_usd"); cost && usage) usage->setText("Provider-reported cost (USD): " + qs(cost->dump()));
        } else if (type == "control_cancel_request") {
            const auto id = qs(value.stringAt("request_id"));
            if (permissions.contains(id)) { *permissions.at(id) = true; permissions.erase(id); }
        } else if (type == "control_request") {
            const auto id = qs(value.stringAt("request_id"));
            const auto* request = value.find("request");
            if (id.isEmpty() || !request || !request->isObject() || permissions.contains(id) || permissions.size() >= 32) { self.stop(); return; }
            permissions.emplace(id, std::make_shared<bool>(false));
            QTimer::singleShot(60000, &self, [this, id] { response(id, Json::object().set("behavior", "deny").set("message", "permission/control request timed out"), true); });
            const auto subtype = request->stringAt("subtype");
            if (subtype == "can_use_tool") permission(id, *request);
            else if ((subtype == "mcp_message" || subtype == "hook_callback") && tools) {
                QJsonObject rpc;
                if (subtype == "mcp_message" && request->stringAt("server_name") == "frostide") { if (const auto* message = request->find("message")) rpc = qjson(*message); }
                else if (subtype == "hook_callback") {
                    const auto name = request->stringAt("hook_event_name") == "Stop" ? "stop_hook" : "post_edit_diagnostics";
                    rpc = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", request->find("input") ? qjson(*request->find("input")) : QJsonObject{}}}}};
                }
                if (rpc.isEmpty()) { response(id, Json::object().set("error", "unregistered MCP server/control"), false); return; }
                QPointer<NativeAgentChat> guard(&self);
                tools->request(rpc, [guard, id](QJsonObject reply) { if (guard) guard->m->response(id, json(reply), false); });
            } else response(id, Json::object().set("error", "unsupported native control request"), false);
        }
    }
    void start() {
        if (!consent || !consent->isChecked() || !owner.agentBridgeActive() || !owner.interactiveReady()) return;
        self.stop();
        const auto currentRoot = owner.projectRoot();
        const auto trust = owner.m_services.trust.snapshot(currentRoot);
        if (!trust.trusted) return;
        root = qs(currentRoot); trustRevision = trust.revision;
        live = std::make_shared<std::atomic<bool>>(true);
        const auto epoch = generation;
        const auto allowed = live;
        QPointer<NativeAgentChat> guard(&self);
        QFileDialog picker;
        picker.setWindowTitle("Select separately installed native CLI"); picker.setFileMode(QFileDialog::ExistingFile);
        picker.selectFile(qs(owner.m_services.settings.snapshot()->get(settings::key::toolsClaudePath)));
        QObject::connect(&self, &QObject::destroyed, &picker, &QDialog::reject);
        const auto picked = picker.exec();
        if (!guard) return;
        if (!admitted() || generation != epoch || picked != QDialog::Accepted || picker.selectedFiles().isEmpty()) return;
        executable = QFileInfo(picker.selectedFiles().front()).canonicalFilePath();
        const auto relative = QDir(root).relativeFilePath(executable);
        if (executable.isEmpty() || (!relative.startsWith("../") && !QDir::isAbsolutePath(relative))) { append("Choose an installed CLI outside the workspace\n"); return; }
#ifdef _WIN32
        if (QFileInfo(executable).fileName().compare("claude.exe", Qt::CaseInsensitive) != 0) { append("A native claude.exe is required; command shims are not admitted\n"); return; }
#else
        if (!QFileInfo(executable).isExecutable()) { append("Selected CLI is not executable\n"); return; }
#endif
        const auto binary = executable;
        configDirectory = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
        if (configDirectory.isEmpty()) configDirectory = QDir::home().filePath(".claude");
        tools = std::make_unique<AgentTools>(owner, [this] { return admitted(); }, &self);
        if (auto serving = tools->start(root, configDirectory); !serving) { append(qs(serving.error().message()) + "\n"); return; }
        agent::NativeLaunch launch;
        launch.executable = utf8(binary); launch.root = currentRoot;
        launch.settingsFile = utf8(tools->hookSettings()); launch.mcpConfig = utf8(tools->mcpConfig());
        launch.model = model ? utf8(model->text()) : "";
        launch.effort = effort ? utf8(effort->currentText()) : "";
        launch.resume = sessions ? utf8(sessions->currentData().toString()) : "";
        launch.forkSession = fork && fork->isChecked();
        launch.resumeAt = resumeAt ? utf8(resumeAt->text()) : "";
        launch.idePort = owner.agentBridgePort();
        const auto revision = trustRevision;
        if (status) status->setText("Checking fresh CLI authentication; no user turn submitted");
        probe = std::jthread([this, guard, allowed, launch, revision, epoch](std::stop_token stop) {
            const auto valid = [&] { const auto trust = owner.m_services.trust.snapshot(launch.root); return allowed->load() && !stop.stop_requested() && trust.trusted && trust.revision == revision; };
            if (!valid()) return;
            io::ProcessSpec auth; auth.executable = launch.executable; auth.workingDirectory = launch.root; auth.arguments = {"auth", "status", "--json"};
            auto checked = io::runToCompletion(owner.m_services.launcher, auth, {}, std::chrono::seconds(10), stop);
            auto data = checked && checked->exitCode == 0 ? rpc::parseJson(checked->stdoutBytes) : Result<Json>(fail(ErrorCode::PermissionDenied, "CLI authentication status unavailable"));
            auto route = data ? agent::nativeAuthRoute(*data) : Result<agent::AuthRoute>(std::unexpected(data.error()));
            if (!valid() || !guard) return;
            if (!route) {
                const auto reason = route.error().message();
                QMetaObject::invokeMethod(guard, [guard, epoch, reason] { if (guard && guard->m->generation == epoch) { guard->m->append(qs(reason) + "\n"); guard->stop(); } }, Qt::QueuedConnection); return;
            }
            agent::NativeSession::Observer observer;
            struct Mailbox { std::atomic<std::size_t> bytes{0}; std::atomic<unsigned> count{0}; };
            auto mailbox = std::make_shared<Mailbox>();
            observer.event = [guard, epoch, mailbox](Json event) {
                if (!guard) return false;
                const auto bytes = event.dump().size();
                const auto oldBytes = mailbox->bytes.fetch_add(bytes);
                const auto oldCount = mailbox->count.fetch_add(1);
                if (oldBytes + bytes > 2 * 1024 * 1024 || oldCount >= 64) { mailbox->bytes.fetch_sub(bytes); mailbox->count.fetch_sub(1); return false; }
                return QMetaObject::invokeMethod(guard, [guard, epoch, mailbox, bytes, event = std::move(event)]() mutable {
                    mailbox->bytes.fetch_sub(bytes); mailbox->count.fetch_sub(1);
                    if (guard && guard->m->generation == epoch) guard->m->event(std::move(event));
                }, Qt::QueuedConnection);
            };
            observer.stderrChunk = [guard, epoch](std::string text) { if (guard) QMetaObject::invokeMethod(guard, [guard, epoch, text = std::move(text)] { if (guard && guard->m->generation == epoch) guard->m->append(qs(text)); }, Qt::QueuedConnection); };
            observer.failure = [guard, epoch](Error error) { if (guard) QMetaObject::invokeMethod(guard, [guard, epoch, error = std::move(error)] { if (guard && guard->m->generation == epoch) { guard->m->append(qs(error.message()) + "\n"); guard->stop(); } }, Qt::QueuedConnection); };
            observer.exited = [guard, epoch](io::ExitStatus status) { if (guard) QMetaObject::invokeMethod(guard, [guard, epoch, status] { if (guard && guard->m->generation == epoch) { guard->m->append("CLI exited: " + QString::number(status.exitCode) + "\n"); guard->stop(); } }, Qt::QueuedConnection); };
            if (!valid()) return;
            auto session = agent::NativeSession::start(owner.m_services.launcher, {*owner.m_reactor, *owner.m_pool}, launch, *data, std::move(observer));
            if (!guard || !valid()) { if (session) (*session)->stop(); return; }
            auto retained = std::make_shared<Result<std::unique_ptr<agent::NativeSession>>>(std::move(session));
            QMetaObject::invokeMethod(guard, [guard, epoch, retained] {
                if (!guard || !guard->m->admitted() || guard->m->generation != epoch) return;
                if (!*retained) { guard->m->append(qs(retained->error().message()) + "\n"); guard->stop(); return; }
                guard->m->process = std::move(**retained);
                auto early = std::move(guard->m->earlyEvents); guard->m->earlyBytes = 0;
                auto initialize = guard->m->protocol.control("initialize", Json::object(), std::chrono::steady_clock::now());
                if (initialize) (void)guard->m->process->send(*initialize);
                guard->m->timer.start();
                for (auto& event : early) { if (!guard || guard->m->generation != epoch) break; guard->m->event(std::move(event)); }
            }, Qt::QueuedConnection);
        });
    }
};
NativeAgentChat::NativeAgentChat(Workbench& owner) : QObject(&owner), m(std::make_unique<State>(*this, owner)) {
    connect(&owner, &Workbench::projectChanged, this, [this](const QString&) { stop(); });
    m->timer.setInterval(100);
    connect(&m->timer, &QTimer::timeout, this, [this] {
        if (!m->admitted()) { stop(); return; }
        if (m->process) for (const auto& cancel : m->protocol.expire(std::chrono::steady_clock::now())) (void)m->process->send(cancel);
    });
}
NativeAgentChat::~NativeAgentChat() { stop(); if (m->panel) delete m->panel; }
void NativeAgentChat::stop() {
    m->live->store(false); ++m->generation;
    m->probe.request_stop();
    m->timer.stop();
    if (m->process) { m->process->stop(); m->process.reset(); }
    if (m->tools) { m->tools->stop(); m->tools.reset(); }
    m->permissions.clear();
    m->protocol = agent::NativeProtocol{}; m->earlyEvents.clear(); m->earlyBytes = 0;
    m->rewindRequest.clear(); m->rewindMessage.clear(); if (m->checkpoints) m->checkpoints->clear();
    if (m->send) m->send->setEnabled(false);
    if (m->status) m->status->setText("Disconnected; no user turn submitted automatically");
}
void NativeAgentChat::show() {
    if (m->panel) { m->panel->show(); m->panel->raise(); return; }
    auto* panel = new QDialog(m->owner.m_window);
    m->panel = panel;
    panel->setAttribute(Qt::WA_DeleteOnClose);
    panel->setWindowTitle("AI Agent"); panel->resize(850, 720);
    auto* layout = new QVBoxLayout(panel);
    auto* notice = new QLabel("Native chat uses API-key/apiKeyHelper or supported third-party authentication. Subscription sign-in is available only in your Agent Terminal. Filesystem/shell access is restricted to scoped IDE MCP tools.", panel);
    notice->setWordWrap(true); layout->addWidget(notice);
    m->consent = new QCheckBox("Allow this session to use my configured provider; pressing Send may transfer the prompt/workspace context and incur provider charges", panel);
    layout->addWidget(m->consent);
    connect(m->consent, &QCheckBox::toggled, this, [this](bool allowed) { if (!allowed) stop(); });
    auto* form = new QFormLayout;
    m->model = new QLineEdit(panel); form->addRow("Model (optional)", m->model);
    m->effort = new QComboBox(panel); m->effort->addItems({"medium", "low", "high", "max"}); form->addRow("Effort", m->effort);
    m->sessions = new QComboBox(panel); m->sessions->addItem("New session", "");
    QString config = qEnvironmentVariable("CLAUDE_CONFIG_DIR"); if (config.isEmpty()) config = QDir::home().filePath(".claude");
    for (const auto& file : QDir(QDir(config).filePath("frostide")).entryInfoList({"*.session.json"}, QDir::Files | QDir::NoSymLinks)) {
        auto bytes = readAgentPrivateFile(utf8(file.absoluteFilePath())); if (!bytes) continue;
        auto record = rpc::parseJson(*bytes);
        if (record && record->stringAt("kind") == "frostide-session" && record->stringAt("root") == m->owner.projectRoot() && record->stringAt("session").size() <= 256)
            m->sessions->addItem(qs(record->stringAt("session")), qs(record->stringAt("session")));
    }
    form->addRow("Own indexed session", m->sessions);
    m->fork = new QCheckBox("Fork selected session", panel); form->addRow(m->fork);
    m->resumeAt = new QLineEdit(panel); form->addRow("Resume at message UUID (optional)", m->resumeAt);
    layout->addLayout(form);
    auto* connectRow = new QHBoxLayout;
    auto* connectButton = new QPushButton("Connect (no prompt)", panel); connectRow->addWidget(connectButton);
    auto* disconnect = new QPushButton("Disconnect", panel); connectRow->addWidget(disconnect);
    auto* terminal = new QPushButton("Open Agent Terminal", panel); connectRow->addWidget(terminal);
    layout->addLayout(connectRow);
    m->status = new QLabel("Disconnected", panel); m->status->setAccessibleName("Native agent connection status"); layout->addWidget(m->status);
    m->transcript = new QPlainTextEdit(panel); m->transcript->setReadOnly(true); m->transcript->setAccessibleName("Native agent transcript"); layout->addWidget(m->transcript, 1);
    m->usage = new QLabel("Usage/cost: not reported", panel); m->usage->setTextFormat(Qt::PlainText); layout->addWidget(m->usage);
    m->prompt = new QPlainTextEdit(panel); m->prompt->setAccessibleName("Message to AI Agent"); m->prompt->setMaximumHeight(130); layout->addWidget(m->prompt);
    auto* actions = new QHBoxLayout;
    m->send = new QPushButton("Send", panel); m->send->setEnabled(false); actions->addWidget(m->send);
    auto* interrupt = new QPushButton("Interrupt", panel); actions->addWidget(interrupt);
    auto* context = new QPushButton("Refresh context usage", panel); actions->addWidget(context);
    auto* changeModel = new QPushButton("Set model", panel); actions->addWidget(changeModel);
    auto* rewind = new QPushButton("Review CLI rewind and fork...", panel); actions->addWidget(rewind);
    auto* planMode = new QCheckBox("Plan mode", panel); actions->addWidget(planMode);
    auto* transcript = new QPushButton("View transcript read-only...", panel); actions->addWidget(transcript);
    layout->addLayout(actions);
    connect(connectButton, &QPushButton::clicked, this, [this] { m->start(); });
    connect(disconnect, &QPushButton::clicked, this, [this] { stop(); });
    connect(terminal, &QPushButton::clicked, this, [this] { m->owner.enableAgentBridge(true); });
    connect(panel, &QDialog::finished, this, [this](int) { stop(); m->panel.clear(); });
    connect(m->send, &QPushButton::clicked, this, [this] {
        if (!m->admitted() || !m->process || !m->consent->isChecked()) return;
        const auto text = utf8(m->prompt->toPlainText());
        if (text == "/login") {
            io::ProcessSpec login; login.executable = utf8(m->executable); login.workingDirectory = utf8(m->root); login.arguments = {"auth", "login"};
            if (m->owner.m_terminal) { auto opened = m->owner.m_terminal->openProcess(login, "Agent Terminal sign-in"); if (!opened) m->append(qs(opened.error().message()) + "\n"); }
            stop(); return;
        }
        auto turn = m->protocol.userTurn(text);
        if (!turn) { m->append(qs(turn.error().message()) + "\n"); return; }
        auto sent = m->process->send(*turn);
        if (!sent) { m->append(qs(sent.error().message()) + "\n"); return; }
        m->append("You: " + qs(text) + "\n"); m->prompt->clear();
    });
    const auto control = [this](const QString& type, Json params) {
        if (!m->admitted() || !m->process) return;
        auto request = m->protocol.control(utf8(type), std::move(params), std::chrono::steady_clock::now());
        if (!request) m->append(qs(request.error().message()) + "\n");
        else if (auto written = m->process->send(*request); !written) m->append(qs(written.error().message()) + "\n");
    };
    connect(interrupt, &QPushButton::clicked, this, [control] { control("interrupt", Json::object()); });
    connect(context, &QPushButton::clicked, this, [control] { control("get_context_usage", Json::object()); });
    connect(planMode, &QCheckBox::toggled, this, [control](bool plan) { control("set_permission_mode", Json::object().set("mode", plan ? "plan" : "default")); });
    connect(rewind, &QPushButton::clicked, this, [this] {
        if (!m->admitted() || !m->process || !m->rewindRequest.isEmpty() || !m->checkpoints || m->checkpoints->currentData().toString().isEmpty()) return;
        const auto message = m->checkpoints->currentData().toString();
        auto request = m->protocol.control("rewind_files", Json::object().set("user_message_id", utf8(message)).set("dry_run", true), std::chrono::steady_clock::now());
        if (!request) { m->append(qs(request.error().message()) + "\n"); return; }
        m->rewindMessage = message; m->rewindDryRun = true; m->rewindRequest = qs(request->stringAt("request_id")); (void)m->process->send(*request);
    });
    connect(transcript, &QPushButton::clicked, this, [this] {
        QPointer<NativeAgentChat> guard(this);
        QFileDialog picker; picker.setWindowTitle("View one JSONL transcript locally (read-only)"); picker.setFileMode(QFileDialog::ExistingFile); picker.setNameFilter("CLI transcripts (*.jsonl)");
        QObject::connect(this, &QObject::destroyed, &picker, &QDialog::reject);
        if (picker.exec() != QDialog::Accepted || !guard || picker.selectedFiles().isEmpty()) return;
        const auto path = QFileInfo(picker.selectedFiles().front()).canonicalFilePath();
        const auto info = QFileInfo(path);
        if (info.suffix() != "jsonl" || info.fileName().contains("keychain", Qt::CaseInsensitive)) return;
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return;
        agent::NdjsonFramer framer; std::size_t bytes = 0;
        QString display;
        while (!file.atEnd()) {
            const auto chunk = file.read(4096); bytes += static_cast<std::size_t>(chunk.size());
            if (bytes > 8 * 1024 * 1024) { m->append("Transcript exceeds the local read cap\n"); return; }
            auto events = framer.feed(std::string_view(chunk.constData(), static_cast<std::size_t>(chunk.size())));
            if (!events) { m->append("Malformed transcript refused\n"); return; }
            for (const auto& event : *events) display += qs(event.dump()) + "\n";
        }
        if (!framer.finish()) { m->append("Incomplete transcript refused\n"); return; }
        m->append("Read-only local transcript (not sent):\n" + display);
    });    connect(changeModel, &QPushButton::clicked, this, [this, control] { control("set_model", Json::object().set("model", utf8(m->model->text()))); });
    panel->show();
}
void Workbench::showNativeAgent() {
    if (!agentBridgeActive() || !agentBridgePort()) enableAgentBridge(false);
    if (!agentBridgeActive() || !m_window || !m_services.trust.isTrusted(m_projectRoot)) return;
    if (!m_nativeAgent) m_nativeAgent = std::make_unique<NativeAgentChat>(*this);
    m_nativeAgent->show();
}
void Workbench::stopNativeAgent() { if (m_nativeAgent) m_nativeAgent->stop(); }
}
