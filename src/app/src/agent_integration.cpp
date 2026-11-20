// SPDX-License-Identifier: Apache-2.0
// C1 shares only the opted-in project's real editors; no prompt, credentials, hooks or chat API.
#include "agent_integration.hpp"
#include "agent_bridge.hpp"
#include "agent_scope.hpp"
#include "agent_tools.hpp"
#include "frost/app/workbench.hpp"
#include "frost/settings/keys.hpp"
#include "frost/io/utf.hpp"
#include "frost/lang/clangd_service.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/terminal/terminal_view.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QPointer>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <thread>
#include <atomic>
namespace frost::app {
namespace {
bool ask(QObject* lifetime, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    QObject::connect(lifetime, &QObject::destroyed, &box, &QDialog::reject);
    return box.exec() == QMessageBox::Yes;
}
void inform(QObject* lifetime, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok);
    QObject::connect(lifetime, &QObject::destroyed, &box, &QDialog::reject);
    box.exec();
}
QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string utf8(const QString& text) { return text.toUtf8().toStdString(); }
QJsonObject textResult(const QJsonValue& value, bool error = false) {
    const auto bytes = (value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject())).toJson(QJsonDocument::Compact);
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(bytes)}}}}, {"isError", error}};
}
QJsonObject failure(const QString& message) { return textResult(QJsonObject{{"error", message}}, true); }
QJsonObject position(const text::Snapshot& text, ByteOffset offset) {
    const auto point = text.utf16PointAt(std::min(offset, text.size()));
    return {{"line", static_cast<int>(point.line.value())}, {"character", static_cast<int>(point.column.value())}};
}
bool inRoot(const QString& root, const QString& path) {
    const auto relative = QDir(root).relativeFilePath(path);
    return !relative.isEmpty() && relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}
}
struct AgentIntegration::State {
    AgentIntegration& self;
    Workbench& owner;
    std::unique_ptr<AgentBridge> bridge;
    std::unique_ptr<AgentTools> tools;
    QTimer timer;
    QElapsedTimer consentAge;
    QString root;
    std::uint64_t trustRevision = 0;
    bool consent = false;
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(false);
    QPointer<QLabel> status;
    QPointer<ui::TerminalView> terminal;
    std::jthread probe;
    std::map<QString, QPointer<QDialog>> diffs;
    QByteArray selection;
    QElapsedTimer selectionAge;
    State(AgentIntegration& self, Workbench& owner) : self(self), owner(owner) {}
    bool admitted() const {
        if (!consent || !consentAge.isValid() || consentAge.elapsed() > 8 * 60 * 60 * 1000 || owner.m_shutDown ||
            !owner.m_interactiveStartupComplete || !owner.m_window || qs(owner.projectRoot()) != root) return false;
        const auto trust = owner.m_services.trust.snapshot(owner.projectRoot());
        return trust.trusted && trust.revision == trustRevision;
    }
    std::optional<std::string> authorize(const QString& name) const {
        if (!admitted()) return {};
        auto path = agentWorkspaceResource(owner.m_services.fs, utf8(root), utf8(name));
        return path ? std::optional<std::string>(*path) : std::nullopt;
    }
    DocumentEditorView* view(const std::string& path) const {
        for (auto* candidate : owner.editors().views()) {
            const auto real = owner.m_services.fs.physicalPath(candidate->path());
            if (real && *real == path) return candidate;
        }
        return nullptr;
    }
    QJsonObject selectionInfo() const {
        auto* current = owner.currentView();
        if (!current || !authorize(qs(current->path()))) return {};
        const auto& editor = current->editor().editor();
        const auto& selections = editor.state().selections;
        if (selections.empty()) return {};
        const auto chosen = selections.all()[selections.primaryIndex()];
        const auto range = chosen.range();
        const auto& text = editor.text();
        if (range.end.value() - range.begin.value() > 256 * 1024) return {};
        return {{"filePath", qs(current->path())}, {"text", qs(text.text(range))},
            {"selection", QJsonObject{{"start", position(text, range.begin)}, {"end", position(text, range.end)}}}};
    }
    bool diagnosticsFresh() const {
        for (auto* current : owner.editors().views()) {
            if (!authorize(qs(current->path()))) continue;
            std::optional<lang::FileDiagnostics> published;
            if (QFileInfo(qs(current->path())).suffix().compare("lua", Qt::CaseInsensitive) == 0) {
                if (owner.m_lua) published = owner.m_lua->diagnostics().forFile(current->path());
            } else if (owner.m_clangd) published = owner.m_clangd->diagnostics().forFile(current->path());
            if (!published || published->stale || published->textVersion != current->editor().editor().text().version()) return false;
        }
        return true;
    }
    QJsonArray diagnostics(const QString& uri) const {
        const auto requested = uri.isEmpty() ? std::optional<std::string>{} : authorize(uri);
        QJsonArray files;
        for (auto* current : owner.editors().views()) {
            const auto path = authorize(qs(current->path()));
            if (!path || (!uri.isEmpty() && requested != path)) continue;
            QJsonArray entries;
            const auto& text = current->editor().editor().text();
            for (const auto& diagnostic : current->editor().diagnostics()) {
                if (entries.size() >= 1024) break;
                entries.append(QJsonObject{{"message", qs(diagnostic.message)}, {"code", qs(diagnostic.code)},
                    {"severity", diagnostic.severity == ui::InsightSeverity::Error ? 1 : diagnostic.severity == ui::InsightSeverity::Warning ? 2 : 3},
                    {"range", QJsonObject{{"start", position(text, diagnostic.range.begin)}, {"end", position(text, diagnostic.range.end)}}}});
            }
            files.append(QJsonObject{{"uri", QUrl::fromLocalFile(qs(*path)).toString()}, {"diagnostics", entries}});
            if (files.size() >= 128) break;
        }
        return files;
    }
    void dispatch(const QString& method, const QJsonObject& params, AgentBridge::Reply reply) {
        if (!admitted()) { reply(failure("workspace consent expired")); return; }
        if (method == "tools/list") {
            QJsonArray tools;
            for (const auto* name : {"openDiff", "close_tab", "closeAllDiffTabs", "getDiagnostics", "openFile", "getOpenEditors", "getWorkspaceFolders", "checkDocumentDirty", "saveDocument"}) {
                const QString tool(name);
                QJsonObject properties;
                QJsonArray required;
                const auto field = [&](const QString& key, bool mandatory) {
                    properties.insert(key, QJsonObject{{"type", "string"}});
                    if (mandatory) required.append(key);
                };
                if (tool == "openDiff") for (const auto* key : {"old_file_path", "new_file_path", "new_file_contents", "tab_name"}) field(key, true);
                else if (tool == "close_tab") field("tab_name", true);
                else if (tool == "getDiagnostics") field("uri", false);
                else if (tool == "openFile" || tool == "checkDocumentDirty" || tool == "saveDocument") field("filePath", true);
                tools.append(QJsonObject{{"name", tool}, {"description", "Consent-scoped FrostIDE editor tool"},
                    {"inputSchema", QJsonObject{{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}});
            }            reply({{"tools", tools}}); return;
        }
        if (method != "tools/call") { reply(failure("unsupported MCP method")); return; }
        const auto name = params.value("name").toString();
        const auto args = params.value("arguments").toObject();
        if (name == "getWorkspaceFolders") { reply(textResult(QJsonObject{{"workspaceFolders", QJsonArray{root}}})); return; }
        if (name == "getOpenEditors") {
            QJsonArray editors;
            for (auto* current : owner.editors().views()) if (authorize(qs(current->path())))
                editors.append(QJsonObject{{"filePath", qs(current->path())}, {"isDirty", current->document().dirty()}});
            reply(textResult(QJsonObject{{"editors", editors}})); return;
        }
        if (name == "getDiagnostics") {
            if (args.contains("uri") && !args.value("uri").isString()) { reply(failure("diagnostic uri must be a string")); return; }
            const auto uri = args.value("uri").toString();
            if (!uri.isEmpty() && !authorize(uri)) { reply(failure("diagnostic resource is outside the consenting workspace")); return; }
            if (uri.isEmpty() && !diagnosticsFresh()) {
                auto* wait = new QTimer(&self);
                wait->setInterval(25);
                auto deadline = std::make_shared<QElapsedTimer>();
                deadline->start();
                QObject::connect(wait, &QTimer::timeout, &self, [this, wait, deadline, reply] {
                    if (!admitted() || !reply.connected()) { wait->stop(); wait->deleteLater(); reply(failure("workspace consent expired")); return; }
                    const bool fresh = diagnosticsFresh();
                    if (fresh || deadline->elapsed() >= 1500) {
                        wait->stop(); wait->deleteLater();
                        reply(textResult(QJsonObject{{"diagnostics", diagnostics({})}, {"cached", !fresh}}));
                    }
                });
                wait->start();
            } else reply(textResult(QJsonObject{{"diagnostics", diagnostics(uri)}, {"cached", !diagnosticsFresh()}}));            return;
        }
        if (name == "closeAllDiffTabs" || name == "close_tab") {
            const auto title = args.value("tab_name").toString();
            const auto open = diffs;
            for (const auto& [key, dialog] : open) if (dialog && (name == "closeAllDiffTabs" || key == title)) { dialog->setProperty("agentRejected", true); dialog->reject(); }
            reply(textResult(QJsonObject{{"closed", true}})); return;
        }
        const auto path = authorize(args.value(name == "openDiff" ? "new_file_path" : "filePath").toString());
        if (!path) { reply(failure("resource must be an existing file inside the consenting workspace")); return; }
        if (name == "checkDocumentDirty") {
            const auto* current = view(*path);
            reply(textResult(QJsonObject{{"isDirty", current && current->document().dirty()}})); return;
        }
        if (name == "openFile") {
            auto opened = owner.openPath(*path);
            reply(opened ? textResult(QJsonObject{{"opened", true}}) : failure(qs(opened.error().message()))); return;
        }
        if (name == "saveDocument") {
            auto* current = view(*path);
            if (!current) { reply(failure("document is not open")); return; }
            const auto requestGeneration = generation;
            QPointer<AgentIntegration> lifetime(&self);
            const bool approved = ask(&self, "AI Agent save request", "Save this document to disk?\n" + qs(*path));
            if (!lifetime) return;
            if (!approved || !reply.connected() || generation != requestGeneration || !authorize(qs(*path))) { reply(failure("save rejected")); return; }
            current = view(*path);
            if (!current) { reply(failure("document closed")); return; }
            auto saved = owner.save(*current);
            reply(saved ? textResult(QJsonObject{{"saved", true}}) : failure(qs(saved.error().message()))); return;
        }
        if (name != "openDiff") { reply(failure("unknown editor tool")); return; }
        const auto oldPath = authorize(args.value("old_file_path").toString());
        const auto title = args.value("tab_name").toString();
        const auto replacement = utf8(args.value("new_file_contents").toString());
        if (!oldPath || *oldPath != *path || !args.value("new_file_contents").isString() || title.isEmpty() || diffs.contains(title) || diffs.size() >= 8 || replacement.size() > 512 * 1024) {
            reply(failure("invalid or excessive diff request")); return;
        }
        if (auto* current = view(*path); current && current->document().dirty()) { reply(failure("save or discard existing local edits before agent diff")); return; }
        auto original = owner.m_services.fs.readFileSnapshot(*path, 512 * 1024);
        auto old = owner.m_services.fs.readFileSnapshot(*oldPath, 512 * 1024);
        if (!original || !old || !io::isValidUtf8(*old) || !io::isValidUtf8(replacement) || old->find(char(0)) != std::string::npos || replacement.find(char(0)) != std::string::npos) { reply(failure("diff requires bounded UTF-8 files")); return; }
        auto model = vcs::SideBySideDiff::compute(std::move(*old), replacement);
        if (!model) { reply(failure(qs(model.error().message()))); return; }
        auto* dialog = new QDialog(owner.m_window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(title + " — AI Agent Review");
        dialog->resize(1100, 750);
        auto* layout = new QVBoxLayout(dialog);
        auto* viewer = new ui::DiffViewer(dialog);
        viewer->setDiff(std::move(*model), "Original", "Proposed (editable)");
        layout->addWidget(viewer);
        auto* reject = new QPushButton("Reject Proposed Changes", dialog);
        layout->addWidget(reject);
        QObject::connect(reject, &QPushButton::clicked, dialog, [dialog] { dialog->setProperty("agentRejected", true); dialog->reject(); });
        diffs.emplace(title, dialog);
        auto complete = std::make_shared<bool>(false);
        viewer->setApplyHandler([this, dialog = QPointer<QDialog>(dialog), path = *path, original = *original, reply, complete](std::string_view, std::string_view final) -> Result<void> {
            if (*complete || !dialog || !reply.connected() || !authorize(qs(path))) return fail(ErrorCode::Cancelled, "agent diff is no longer authorized");
            if (auto* current = view(path); current && current->document().dirty()) return fail(ErrorCode::Busy, "local editor now has unsaved changes");
            auto current = owner.m_services.fs.readFileSnapshot(path, 512 * 1024);
            if (!current || *current != original) return fail(ErrorCode::Busy, "file changed since the diff opened");
            auto written = owner.m_services.fs.writeFileAtomic(path, final);
            if (!written) return written;
            *complete = true;
            reply(textResult(QJsonObject{{"status", "FILE_SAVED"}, {"final_text", qs(final)}}));
            dialog->accept();
            owner.checkExternalChanges();
            return {};
        });
        QObject::connect(dialog, &QDialog::finished, &self, [this, title, reply, complete, dialog](int code) {
            (void)code;
            diffs.erase(title);
            if (!*complete) { *complete = true; reply(textResult(QJsonObject{{"status", dialog->property("agentRejected").toBool() ? "DIFF_REJECTED" : "TAB_CLOSED"}})); }
        });
        auto* lease = new QTimer(dialog);
        lease->setInterval(75);
        QObject::connect(lease, &QTimer::timeout, dialog, [dialog, reply] { if (!reply.connected()) dialog->reject(); });
        lease->start();
        dialog->show();
    }
};
AgentIntegration::AgentIntegration(Workbench& owner) : QObject(&owner), m(std::make_unique<State>(*this, owner)) {
    connect(&owner, &Workbench::projectChanged, this, [this](const QString&) { stop(); });
    m->bridge = std::make_unique<AgentBridge>([this] { return m->admitted(); }, [this](const QString& method, const QJsonObject& params, AgentBridge::Reply reply) { m->dispatch(method, params, std::move(reply)); }, this);
    m->timer.setInterval(75);
    connect(&m->timer, &QTimer::timeout, this, [this] {
        if (!m->admitted()) { stop(); return; }
        if (m->status) m->status->setText(m->bridge->port() ? QStringLiteral("AI Agent: %1 connected").arg(m->bridge->clientCount()) : QStringLiteral("AI Agent: terminal only"));
        const auto info = m->selectionInfo();
        const auto data = QJsonDocument(info).toJson(QJsonDocument::Compact);
        if (data != m->selection) { m->selection = data; m->bridge->notify("selection_changed", info); }
    });
}
AgentIntegration::~AgentIntegration() { stop(); }
bool AgentIntegration::active() const { return m->admitted() && (m->bridge->port() != 0 || m->terminal); }
unsigned short AgentIntegration::port() const { return m->bridge->port(); }
void AgentIntegration::stop() {
    m->live->store(false, std::memory_order_release);
    ++m->generation;
    m->consent = false;
    m->probe.request_stop();
    m->timer.stop();
    if (m->tools) { m->tools->stop(); m->tools.reset(); }
    m->bridge->stop();
    const auto open = m->diffs;
    for (const auto& [title, dialog] : open) { (void)title; if (dialog) dialog->reject(); }
    if (m->terminal && m->owner.m_terminal && m->owner.m_terminal->toolWindow()) {
        auto* tabs = m->owner.m_terminal->toolWindow();
        for (int index = 0; index < tabs->count(); ++index) if (tabs->view(index) == m->terminal) { tabs->closeTab(index); break; }
    }
    m->terminal.clear();
    if (m->status) { delete m->status; m->status.clear(); }
}
void AgentIntegration::mention() { if (active()) m->bridge->notify("at_mentioned", m->selectionInfo()); }
void AgentIntegration::enable(bool launchTerminal) {
    auto& owner = m->owner;
    if (!owner.interactiveReady() || !owner.m_window || !owner.m_services.trust.isTrusted(owner.projectRoot())) return;
    const auto root = qs(owner.projectRoot());
    QPointer<AgentIntegration> lifetime(this);
    const auto consentGeneration = m->generation;
    if (!ask(this, "Connect AI Agent", "Allow your installed Claude Code CLI to access this window's workspace editors, selections, diagnostics and scoped project tools?\n" + root + "\nDiff changes require Apply; saving requires approval. This consent expires after eight hours or when workspace trust changes. The CLI may use its configured network provider. No prompt will be submitted by FrostIDE.")) return;
    if (!lifetime) return;
    if (m->generation != consentGeneration || root != qs(owner.projectRoot()) || !owner.m_services.trust.isTrusted(owner.projectRoot())) return;
    stop();
    m->root = root;
    m->trustRevision = owner.m_services.trust.snapshot(owner.projectRoot()).revision;
    m->consent = true;
    m->live = std::make_shared<std::atomic<bool>>(true);
    m->consentAge.start();
    QString config = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    if (config.isEmpty()) config = QDir::home().filePath(".claude");
    if (!QDir::isAbsolutePath(config)) { stop(); return; }
    auto started = m->bridge->start(root, config);
    if (!started) { inform(this, "AI Agent", qs(started.error().message())); if (lifetime) stop(); return; }
    m->tools = std::make_unique<AgentTools>(owner, [this] { return m->admitted(); }, this);
    if (auto tools = m->tools->start(root, config); !tools) { inform(this, "AI Agent", qs(tools.error().message())); if (lifetime) stop(); return; }
    m->status = new QLabel("AI Agent: 0 connected", owner.m_window);
    m->status->setAccessibleName("AI Agent connection status");
    owner.m_window->statusBar()->addPermanentWidget(m->status);
    m->timer.start();
    if (!launchTerminal) return;
    const auto pickerGeneration = m->generation;
    QFileDialog picker;
    picker.setWindowTitle("Select installed Claude Code executable");
    picker.setFileMode(QFileDialog::ExistingFile);
    picker.selectFile(qs(owner.m_services.settings.snapshot()->get(settings::key::toolsClaudePath)));
    QObject::connect(this, &QObject::destroyed, &picker, &QDialog::reject);
    const auto selected = picker.exec();
    if (!lifetime || m->generation != pickerGeneration) return;
    const auto executable = selected == QDialog::Accepted && !picker.selectedFiles().isEmpty() ? QFileInfo(picker.selectedFiles().front()).canonicalFilePath() : QString{};
    if (executable.isEmpty() || !m->admitted() || inRoot(root, executable) ||
#ifdef _WIN32
        QFileInfo(executable).fileName().compare("claude.exe", Qt::CaseInsensitive) != 0
#else
        !QFileInfo(executable).isExecutable()
#endif
    ) { stop(); return; }
    const auto epoch = m->generation;
    const auto revision = m->trustRevision;
    const auto live = m->live;
    const auto sessionRoot = m->root;
    QPointer<AgentIntegration> guard(this);
    m->probe = std::jthread([this, guard, executable, epoch, revision, sessionRoot, live](std::stop_token stopToken) {
        const auto admitted = [&] {
            const auto trust = m->owner.m_services.trust.snapshot(utf8(sessionRoot));
            return live->load(std::memory_order_acquire) && !stopToken.stop_requested() && trust.trusted && trust.revision == revision;
        };
        if (!admitted()) return;
        io::ProcessSpec spec;
        spec.executable = utf8(executable);
        spec.arguments = {"--version"};
        spec.workingDirectory = utf8(sessionRoot);
        auto version = io::runToCompletion(m->owner.m_services.launcher, spec, {}, std::chrono::seconds(10), stopToken);
        if (!admitted()) return;
        spec.arguments = {"auth", "status", "--json"};
        auto auth = version ? io::runToCompletion(m->owner.m_services.launcher, spec, {}, std::chrono::seconds(10), stopToken) : Result<io::ProcessResult>(std::unexpected(version.error()));
        if (stopToken.stop_requested() || !guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, executable, epoch, version = std::move(version), auth = std::move(auth)]() mutable {
            if (!guard || !guard->m->admitted() || guard->m->generation != epoch) return;
            auto& state = *guard->m;
            const auto match = version ? QRegularExpression("(\\d+)\\.(\\d+)\\.(\\d+)").match(qs(version->stdoutBytes)) : QRegularExpressionMatch{};
            const bool supported = version && version->exitCode == 0 && match.hasMatch() &&
                (match.captured(1).toInt() > 2 || (match.captured(1).toInt() == 2 && (match.captured(2).toInt() > 1 || (match.captured(2).toInt() == 1 && match.captured(3).toInt() >= 316))));
            if (!version || version->exitCode != 0 || !auth) { inform(guard, "AI Agent", "Could not inspect this installed CLI. Nothing was launched in a terminal."); if (guard) guard->stop(); return; }
            const auto authJson = QJsonDocument::fromJson(QByteArray::fromStdString(auth->stdoutBytes));
            if (!authJson.isObject() || !authJson.object().value("loggedIn").isBool()) { guard->stop(); return; }
            if (!authJson.object().value("loggedIn").toBool()) {
                const bool approved = ask(guard, "CLI sign-in required", "The CLI is not signed in. Open Agent Terminal to sign in yourself? FrostIDE will not run login or submit a prompt.");
                if (!guard) return;
                if (!approved) { guard->stop(); return; }
            } else if (auth->exitCode != 0) { guard->stop(); return; }
            if (!supported) {
                const bool approved = ask(guard, "Older CLI", "This CLI is below 2.1.316. Open a terminal without the editor bridge?");
                if (!guard) return;
                if (!approved) { guard->stop(); return; }
            }
            if (!guard->m->admitted() || guard->m->generation != epoch) return;
            io::ProcessSpec launch;
            launch.executable = utf8(executable);
            launch.workingDirectory = utf8(state.root);
            if (supported) { launch.arguments = {"--ide", "--settings", utf8(state.tools->hookSettings()), "--mcp-config", utf8(state.tools->mcpConfig())}; launch.environment.emplace_back("CLAUDE_CODE_SSE_PORT", std::to_string(state.bridge->port())); }
            else { state.bridge->stop(); if (state.tools) state.tools->stop(); }
            auto opened = state.owner.m_terminal->openProcess(launch, "Agent Terminal (Claude Code)");
            if (!opened) { inform(guard, "AI Agent", qs(opened.error().message())); if (guard) guard->stop(); return; }
            state.terminal = state.owner.m_terminal->toolWindow()->view(*opened);
        }, Qt::QueuedConnection);
    });
}
void Workbench::enableAgentBridge(bool terminal) {
    if (!m_agent) m_agent = std::make_unique<AgentIntegration>(*this);
    m_agent->enable(terminal);
}
void Workbench::stopAgentBridge() { if (m_agent) m_agent->stop(); }
void Workbench::mentionAgentSelection() { if (m_agent) m_agent->mention(); }
unsigned short Workbench::agentBridgePort() const { return m_agent ? m_agent->port() : 0; }
bool Workbench::agentBridgeActive() const { return m_agent && m_agent->active(); }
}
