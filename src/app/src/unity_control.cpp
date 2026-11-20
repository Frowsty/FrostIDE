// SPDX-License-Identifier: Apache-2.0
// Trusted Unity editor discovery and acknowledged play controls.
#include "unity_control.hpp"
#include "unity_events.hpp"
#include "frost/ui/toolwindows/problems.hpp"

#include "frost/app/workbench.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "project_open_broker.hpp"
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QTreeWidget>
#include <QHeaderView>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageAuthenticationCode>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include <map>
namespace frost::app {
namespace {
using namespace std::chrono_literals;
QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
bool hex(std::string_view value, std::size_t length) {
    return value.size() == length &&
           std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
QByteArray transcript(std::initializer_list<std::string_view> fields) {
    QByteArray result;
    for (const auto field : fields) {
        const auto size = static_cast<std::uint32_t>(field.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            result.append(static_cast<char>((size >> shift) & 0xff));
        result.append(field.data(), static_cast<qsizetype>(field.size()));
    }
    return result;
}
QByteArray mac(std::string_view secret, std::initializer_list<std::string_view> fields) {
    return QMessageAuthenticationCode::hash(transcript(fields),
                                            QByteArray(secret.data(), static_cast<qsizetype>(secret.size())),
                                            QCryptographicHash::Sha256)
        .toHex();
}
std::string randomHex(std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size, '0');
    for (std::size_t i = 0; i < result.size(); i += 8) {
        auto value = QRandomGenerator::system()->generate();
        for (std::size_t j = 0; j < 8; ++j) {
            result[i + j] = digits[value & 15];
            value >>= 4;
        }
    }
    return result;
}
} // namespace
struct UnityControlIntegration::Impl {
    struct Session {
        std::string root, instance, id, token;
        int generation = 0, pid = 0;
        std::uint16_t port = 0;
        std::chrono::steady_clock::time_point seen;
        bool events = false;
    };
    UnityControlIntegration& owner;
    Workbench& workbench;
    QPointer<QWidget> panel;
    QPointer<QComboBox> sessions;
    QPointer<QLabel> status;
    QTimer timer;
    std::vector<Session> live;
    std::string selected;
    std::optional<Session> awaiting;
    bool awaitingExplicit = false, interrupted = false;
    std::jthread worker;
    std::uint64_t epoch = 0;
    bool busy = false, stopped = false, autoSelected = false, explicitSelection = false;
    QPointer<QTreeWidget> console;
    QPointer<QLineEdit> filter;
    QPointer<QComboBox> level;
    QPointer<QCheckBox> collapse, clearOnPlay, refreshOnSave, compileOnSave;
    QTimer saveTimer;
    std::string consentRoot, consentInstance, savedRoot, savedSession;
    bool pendingRefresh = false, pendingCompile = false;
    QPointer<QLabel> eventsStatus;
    std::vector<UnityBridgeEvent> journal;
    std::vector<build::BuildDiagnostic> compilerProblems;
    std::string eventSession;
    std::int64_t eventCursor = 0, projectVersion = -1, lostEvents = 0;
    std::optional<bool> lastPlaying;
    std::size_t droppedCompiler = 0;
    void clearSavedWork() {
        saveTimer.stop(); pendingRefresh = pendingCompile = false; savedRoot.clear(); savedSession.clear();
    }
    void revokeSaveConsent() {
        clearSavedWork(); consentRoot.clear(); consentInstance.clear();
        if (refreshOnSave) refreshOnSave->setChecked(false);
        if (compileOnSave) compileOnSave->setChecked(false);
    }
    void captureSaveConsent(QCheckBox* checkbox, bool enabled) {
        if (!enabled) {
            if (checkbox == refreshOnSave) pendingRefresh = false;
            if (checkbox == compileOnSave) pendingCompile = false;
            if (!pendingRefresh && !pendingCompile) saveTimer.stop();
            return;
        }
        const auto found = std::ranges::find_if(live, [&](const auto& item) { return item.id == selected; });
        if (!trusted() || found == live.end() || !found->events) {
            checkbox->setChecked(false);
            if (status) status->setText(QStringLiteral("Connect and select a compatible trusted Unity editor before enabling save actions"));
            return;
        }
        consentRoot = workbench.projectRoot(); consentInstance = found->instance;
    }
    void documentSaved(const document::SavedDocumentCompletion& completion) {
        if (!trusted() || consentRoot != workbench.projectRoot()) return;
        const auto found = std::ranges::find_if(live, [&](const auto& item) { return item.id == selected; });
        if (found == live.end() || !found->events || found->instance != consentInstance) return;
        const auto physicalRoot = externalProjectRoot(workbench.projectRoot());
        const QFileInfo target(qs(completion.target));
        if (!physicalRoot || !target.isFile()) return;
        const auto relative = QDir(qs(*physicalRoot)).relativeFilePath(target.canonicalFilePath());
        if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
            !(relative.startsWith(QStringLiteral("Assets/")) || relative.startsWith(QStringLiteral("Packages/")))) return;
        const bool refresh = refreshOnSave && refreshOnSave->isChecked();
        const bool compile = compileOnSave && compileOnSave->isChecked() && target.suffix().compare(QStringLiteral("cs"), Qt::CaseInsensitive) == 0;
        if (!refresh && !compile) return;
        savedRoot = workbench.projectRoot(); savedSession = selected;
        pendingRefresh = pendingRefresh || refresh; pendingCompile = pendingCompile || compile;
        saveTimer.start(200); // Save All and nearby saves produce one follow-up per selected domain.
    }
    void flushSavedWork() {
        const auto found = std::ranges::find_if(live, [&](const auto& item) { return item.id == selected; });
        if (!trusted() || savedRoot != workbench.projectRoot() || consentRoot != savedRoot || savedSession != selected ||
            found == live.end() || !found->events || found->instance != consentInstance) { clearSavedWork(); return; }
        if (busy) { saveTimer.start(200); return; }
        if (pendingRefresh) { pendingRefresh = false; run("refresh"); }
        else if (pendingCompile) { pendingCompile = false; run("recompile"); }
        if (pendingRefresh || pendingCompile) saveTimer.start(200);
    }    QString sourcePath(std::string_view file) const {
        if (file.empty()) return {};
        const auto path = QDir(qs(workbench.projectRoot())).absoluteFilePath(qs(file));
        const QFileInfo info(path);
        return info.isFile() ? info.canonicalFilePath() : QString{};
    }
    void clearDomain(const std::string& id) {
        clearSavedWork();
        eventSession = id; eventCursor = 0; projectVersion = -1; lostEvents = 0; lastPlaying.reset();
        journal.clear(); compilerProblems.clear(); droppedCompiler = 0;
        if (console) console->clear();
        if (eventsStatus) eventsStatus->setText(QStringLiteral("Waiting for Unity event journal"));
        if (workbench.problems()) workbench.problems()->model()->clear(ui::DiagnosticSource::Unity);
    }
    void renderConsole() {
        if (!console) return;
        console->clear();
        std::map<std::string, QTreeWidgetItem*> duplicates;
        const QString query = filter ? filter->text() : QString{};
        const QString severity = level ? level->currentData().toString() : QString{};
        for (const auto& entry : journal) {
            if ((!severity.isEmpty() && severity != qs(entry.level)) ||
                (!query.isEmpty() && !qs(entry.message + entry.stack + entry.file).contains(query, Qt::CaseInsensitive))) continue;
            const auto key = lang::Json::array().push(entry.kind).push(entry.level).push(entry.message).push(entry.stack).push(entry.file).push(entry.line).push(entry.column).dump();
            if (collapse && collapse->isChecked()) {
                if (const auto found = duplicates.find(key); found != duplicates.end()) {
                    auto* row = found->second;
                    row->setText(3, QString::number(row->text(3).toInt() + 1));
                    continue;
                }
            }
            auto* row = new QTreeWidgetItem(console, {qs(entry.level), qs(entry.message) + (entry.truncated ? QStringLiteral(" [truncated]") : QString{}), qs(entry.file), QStringLiteral("1")});
            duplicates[key] = row;
            const auto path = sourcePath(entry.file);
            if (!entry.truncated && !path.isEmpty() && entry.line > 0) {
                row->setData(0, Qt::UserRole, path); row->setData(0, Qt::UserRole + 1, entry.line); row->setData(0, Qt::UserRole + 2, entry.column);
            }
            const QRegularExpression frame(QStringLiteral("\\(at (.+):(\\d+)\\)"));
            for (const auto& lineText : qs(entry.stack).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                auto* child = new QTreeWidgetItem(row, {QString{}, lineText});
                const auto match = frame.match(lineText);
                if (!match.hasMatch()) continue;
                const auto target = sourcePath(match.captured(1).toUtf8().toStdString());
                const auto line = match.captured(2).toInt();
                if (target.isEmpty() || line <= 0) continue;
                child->setData(0, Qt::UserRole, target); child->setData(0, Qt::UserRole + 1, line); child->setData(0, Qt::UserRole + 2, 1);
            }
        }
    }
    void publishEvents(const UnityEventPage& page) {
        lostEvents += page.lost;
        if (page.lost && workbench.problems()) {
            compilerProblems.clear();
            workbench.problems()->model()->clear(ui::DiagnosticSource::Unity);
        }
        for (const auto& entry : page.events) {
            if (entry.kind == "compilationStarted") { compilerProblems.clear(); droppedCompiler = 0; }
            if (entry.kind == "compiler") {
                build::BuildDiagnostic row;
                row.diagnostic.message = entry.message;
                row.diagnostic.severity = entry.level == "error" ? build::Severity::Error : build::Severity::Warning;
                row.diagnostic.file = entry.file; row.diagnostic.line = entry.line; row.diagnostic.column = entry.column;
                row.diagnostic.project = entry.assembly;
                row.path = sourcePath(entry.file).toUtf8().toStdString();
                row.jumpable = !entry.truncated && !row.path.empty() && entry.line > 0;
                if (compilerProblems.size() < 4096) compilerProblems.push_back(std::move(row)); else ++droppedCompiler;
            }
            journal.push_back(entry);
        }
        if (journal.size() > 2000) journal.erase(journal.begin(), journal.begin() + static_cast<std::ptrdiff_t>(journal.size() - 2000));
        if (workbench.problems() && !page.events.empty()) workbench.problems()->model()->setProblems(ui::DiagnosticSource::Unity, compilerProblems);
        const bool changed = projectVersion != page.projectVersion;
        eventCursor = page.next; projectVersion = page.projectVersion;
        if (changed || std::ranges::any_of(page.events, [](const auto& entry) { return entry.kind == "compilationFinished"; })) workbench.checkUnityProjectContext();
        if (eventsStatus) eventsStatus->setText(QStringLiteral("Domain %1 · project change %2 · %3 · %4 events unavailable%5 · %6 retained console events · %7 compiler rows omitted")
            .arg(qs(eventSession).left(8)).arg(projectVersion).arg(page.compiling ? QStringLiteral("compiling") : QStringLiteral("not compiling"))
            .arg(lostEvents).arg(lostEvents ? QStringLiteral("; compiler diagnostics may be incomplete") : QString{}).arg(journal.size()).arg(droppedCompiler));
        if (!page.events.empty()) renderConsole();
    }
    void cancel() {
        revokeSaveConsent();
        clearDomain({});
        ++epoch;
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        busy = false;
        live.clear();
        selected.clear();
        awaiting.reset();
        awaitingExplicit = false;
        interrupted = false;
        autoSelected = false;
        explicitSelection = false;
        if (sessions) {
            sessions->clear();
            sessions->addItem(QStringLiteral("No Unity editor"));
        }
        if (status)
            status->setText(QStringLiteral("Disconnected"));
    }
    bool trusted() const {
        return !stopped && !workbench.m_shutDown && !workbench.projectRoot().empty() &&
               workbench.m_services.trust.isTrusted(workbench.projectRoot());
    }
    void rebuild() {
        if (!sessions)
            return;
        sessions->blockSignals(true);
        sessions->clear();
        sessions->addItem(live.empty() ? QStringLiteral("No Unity editor") : QStringLiteral("Select a Unity editor"),
                          QString{});
        for (const auto& item : live)
            sessions->addItem(QStringLiteral("Unity editor (reported PID %1)").arg(item.pid), qs(item.id));
        int index = selected.empty() ? 0 : sessions->findData(qs(selected));
        if (index < 0) {
            selected.clear();
            index = 0;
        }
        sessions->setCurrentIndex(index);
        sessions->blockSignals(false);
    }
    Result<void> announce(const lang::Json& json) {
        if (!trusted()) {
            cancel();
            return fail(ErrorCode::Untrusted, "project is not trusted");
        }
        const std::string root(json.stringAt("root")), instance(json.stringAt("instance")),
            id(json.stringAt("session")), token(json.stringAt("token"));
        const auto generation = json.integerAt("generation"), pid = json.integerAt("pid"),
                   port = json.integerAt("port");
        if (json.integerAt("version") != 2 || root.empty() || !hex(instance, 32) || !hex(id, 32) || !hex(token, 64) ||
            !generation || *generation <= 0 || *generation > INT32_MAX || !pid || *pid <= 0 || *pid > INT32_MAX ||
            !port || *port <= 0 || *port > 65535)
            return fail(ErrorCode::InvalidArgument, "invalid Unity session announcement");
        auto canonical = externalProjectRoot(workbench.projectRoot());
        if (!canonical || lang::documentKey(*canonical) != lang::documentKey(root))
            return fail(ErrorCode::InvalidArgument, "Unity session project mismatch");
        const QDir project(qs(*canonical));
        if (!QFileInfo(project.filePath(QStringLiteral("Assets"))).isDir() ||
            !QFileInfo(project.filePath(QStringLiteral("Packages"))).isDir() ||
            !QFileInfo(project.filePath(QStringLiteral("ProjectSettings"))).isDir())
            return fail(ErrorCode::InvalidArgument, "project is not a Unity project");
        auto it = std::ranges::find_if(live, [&](const auto& s) { return s.id == id; });
        if (it != live.end()) {
            if (it->events != (json.integerAt("eventVersion").value_or(0) == 1) || it->root != root || it->instance != instance || it->generation != *generation || it->token != token ||
                it->pid != *pid || it->port != *port)
                return fail(ErrorCode::InvalidArgument, "Unity session changed identity");
            it->seen = std::chrono::steady_clock::now();
            return {};
        }
        if (awaiting && awaiting->instance == instance) {
            if (*generation < awaiting->generation)
                return fail(ErrorCode::InvalidArgument, "stale Unity domain announcement");
            if (*generation == awaiting->generation &&
                (awaiting->root != root || awaiting->id != id || awaiting->token != token || awaiting->pid != *pid ||
                 awaiting->port != *port))
                return fail(ErrorCode::InvalidArgument, "expired Unity session changed identity");
        }
        auto previous = std::ranges::find_if(live, [&](const auto& s) { return s.instance == instance; });
        bool reconnect = false;
        if (previous != live.end()) {
            if (*generation <= previous->generation)
                return fail(ErrorCode::InvalidArgument, "stale Unity domain announcement");
            reconnect = selected == previous->id;
            if (reconnect) {
                interrupted = interrupted || busy;
                ++epoch;
                if (worker.joinable()) {
                    worker.request_stop();
                    worker.join();
                }
                busy = false;
                selected.clear();
            }
            live.erase(previous);
        }
        if (live.size() >= 16)
            return fail(ErrorCode::Busy, "too many Unity sessions");
        live.push_back({root, instance, id, token, static_cast<int>(*generation), static_cast<int>(*pid),
                        static_cast<std::uint16_t>(*port), std::chrono::steady_clock::now(), json.integerAt("eventVersion").value_or(0) == 1});
        if (!reconnect && awaiting && awaiting->instance == instance && *generation >= awaiting->generation &&
            (awaitingExplicit || live.size() == 1)) {
            reconnect = true;
            explicitSelection = awaitingExplicit;
        }
        if (reconnect) {
            selected = id;
            awaiting.reset();
            awaitingExplicit = false;
            if (status)
                status->setText(QStringLiteral("Unity reconnected; checking state"));
        } else if (live.size() == 1 && selected.empty() && !autoSelected && !awaiting) {
            selected = id;
            autoSelected = true;
        }
        if (live.size() > 1 && !explicitSelection && !reconnect && !selected.empty()) {
            ++epoch;
            if (worker.joinable()) {
                worker.request_stop();
                worker.join();
            }
            busy = false;
            selected.clear();
            if (status)
                status->setText(QStringLiteral("Select a Unity editor"));
        }
        rebuild();
        if (selected == id && !busy)
            run("state");
        return {};
    }
    void expire() {
        if (!trusted()) {
            if (!live.empty() || busy || awaiting)
                cancel();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto old = live.size();
        const auto current = std::ranges::find_if(live, [&](const auto& item) { return item.id == selected; });
        if (current != live.end() && now - current->seen > 7s) {
            awaiting = *current;
            awaitingExplicit = explicitSelection;
        }
        std::erase_if(live, [&](const auto& item) { return now - item.seen > 7s; });
        if (live.size() != old) {
            if (!selected.empty() && std::ranges::none_of(live, [&](const auto& s) { return s.id == selected; })) {
                ++epoch;
                if (worker.joinable()) {
                    worker.request_stop();
                    worker.join();
                }
                interrupted = interrupted || busy;
                busy = false;
                selected.clear();
                if (status)
                    status->setText(QStringLiteral("Unity editor disconnected; waiting for the selected editor"));
            }
            rebuild();
        }
        if (!busy && !pendingRefresh && !pendingCompile && !selected.empty()) {
            const auto found = std::ranges::find_if(live, [&](const auto& item) { return item.id == selected; });
            if (found != live.end() && found->events) {
                if (eventSession != found->id) clearDomain(found->id);
                run("events:" + std::to_string(eventCursor));
            }
        }
    }
    void run(std::string command) {
        if (!trusted()) {
            cancel();
            if (status)
                status->setText(QStringLiteral("Trust the Unity project first"));
            return;
        }
        if (busy) { status->setText(QStringLiteral("A Unity request is already pending")); return; }
        const auto found = std::ranges::find_if(live, [&](const auto& s) { return s.id == selected; });
        if (found == live.end()) {
            status->setText(QStringLiteral("Select a connected Unity editor"));
            return;
        }
        if ((command.starts_with("events:") || command == "refresh" || command == "recompile") && !found->events) {
            status->setText(QStringLiteral("Update the Unity editor package to use event and refresh controls")); return;
        }
        if (eventSession != found->id) clearDomain(found->id);
        Session target = *found;
        if (worker.joinable())
            worker.join();
        busy = true;
        const auto round = ++epoch;
        const auto id = randomHex(32);
        const auto challenge = randomHex(64);
        if (!command.starts_with("events:")) status->setText(QStringLiteral("Waiting for Unity…"));
        QPointer<UnityControlIntegration> receiver(&owner);
        auto* trust = &workbench.m_services.trust;
        const auto trustRoot = workbench.projectRoot();
        worker = std::jthread([receiver, round, target, id, challenge, trust, trustRoot,
                               command = std::move(command)](const std::stop_token& stop) mutable {
            Result<lang::Json> result = fail(ErrorCode::Cancelled, "Unity request cancelled");
            auto connection = io::LoopbackStream::connect(target.port, std::chrono::steady_clock::now() + 5s, stop);
            if (connection) {
                const auto hello =
                    lang::Json::object().set("version", 2).set("session", target.id).set("challenge", challenge).dump();
                auto response = (*connection)->exchange(hello, 8192);
                auto proof = response ? lang::parseJson(*response)
                                      : Result<lang::Json>(std::unexpected(std::move(response.error())));
                const std::string nonce = proof ? std::string(proof->stringAt("nonce")) : std::string{};
                const QByteArray expected =
                    mac(target.token, {"frost-unity-server-v2", target.root, target.id, challenge, nonce});
                const std::string received = proof ? std::string(proof->stringAt("proof")) : std::string{};
                unsigned difference =
                    static_cast<unsigned>(received.size() ^ static_cast<std::size_t>(expected.size()));
                for (std::size_t i = 0; i < received.size() && i < static_cast<std::size_t>(expected.size()); ++i)
                    difference |=
                        static_cast<unsigned>(static_cast<unsigned char>(received[i]) ^
                                              static_cast<unsigned char>(expected[static_cast<qsizetype>(i)]));
                if (!proof || proof->stringAt("session") != target.id || !hex(nonce, 64) || difference != 0)
                    result = fail(ErrorCode::PermissionDenied, "Unity server authentication failed");
                else if (stop.stop_requested() || !trust->isTrusted(trustRoot))
                    result = fail(ErrorCode::Cancelled, "Unity request cancelled after trust changed");
                else {
                    const std::string signature = mac(target.token, {"frost-unity-request-v2", target.root, target.id,
                                                                     id, command, "4500", challenge, nonce})
                                                      .toStdString();
                    auto request = lang::Json::object()
                                       .set("version", 2)
                                       .set("project", target.root)
                                       .set("session", target.id)
                                       .set("id", id)
                                       .set("command", command)
                                       .set("ttlMs", 4500)
                                       .set("nonce", nonce)
                                       .set("mac", signature)
                                       .dump();
                    auto reply = (*connection)->exchange(request, command.starts_with("events:") ? 262144 : 8192);
                    if (reply)
                        result = lang::parseJson(*reply);
                    else
                        result = std::unexpected(std::move(reply.error()));
                }
            } else
                result = std::unexpected(std::move(connection.error()));
            if (!QCoreApplication::instance())
                return;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [receiver, round, target, id, command, result = std::move(result)]() mutable {
                    if (!receiver)
                        return;
                    auto& state = *receiver->m_impl;
                    if (state.epoch != round || !state.trusted() || state.selected != target.id)
                        return;
                    state.busy = false;
                    if (!result || !result->isObject() || result->stringAt("session") != target.id ||
                        result->stringAt("id") != id) {
                        if (command == "refresh" || command == "recompile") state.clearSavedWork();
                        if (command != "state" && !command.starts_with("events:"))
                            state.interrupted = true;
                        state.status->setText(QStringLiteral("Unity request interrupted; state is uncertain"));
                        return;
                    }
                    if (!result->boolAt("ok").value_or(false)) {
                        if (command == "refresh" || command == "recompile") state.clearSavedWork();
                        state.status->setText(qs(result->stringAt("message")));
                        return;
                    }
                    const auto observedPlaying = result->boolAt("playing"), observedPaused = result->boolAt("paused");
                    const auto observedFrame = result->integerAt("frame");
                    if (!observedPlaying || !observedPaused || !observedFrame || *observedFrame < 0) {
                        if (command == "refresh" || command == "recompile") { state.clearSavedWork(); state.interrupted = true; }
                        state.status->setText(QStringLiteral("Unity returned an invalid state snapshot")); return;
                    }
                    const bool playing = *observedPlaying, paused = *observedPaused;
                    if (state.lastPlaying && !*state.lastPlaying && playing && state.clearOnPlay && state.clearOnPlay->isChecked()) {
                        state.journal.clear(); state.renderConsole();
                    }
                    state.lastPlaying = playing;
                    if (command.starts_with("events:")) {
                        auto page = parseUnityEvents(*result, state.eventCursor);
                        if (!page) { state.eventsStatus->setText(qs(page.error().message())); return; }
                        state.publishEvents(*page);
                    }
                    QString observed = !playing ? QStringLiteral("Unity stopped")
                                       : paused ? QStringLiteral("Unity paused")
                                                : QStringLiteral("Unity playing");
                    if (command == "refresh" || command == "recompile") observed += QStringLiteral(" · ") + qs(result->stringAt("message"));
                    if (state.interrupted)
                        observed += QStringLiteral(" (previous request interrupted)");
                    state.status->setText(observed);
                    state.interrupted = false;
                },
                Qt::QueuedConnection);
        });
    }
};
UnityControlIntegration::UnityControlIntegration(Workbench& workbench)
    : QObject(&workbench), m_impl(std::make_unique<Impl>(*this, workbench)) {
    auto* panel = new QWidget(workbench.m_window);
    panel->setObjectName(QStringLiteral("unityControlsPanel"));
    // The tool window takes ownership lazily; do not show this child over the menu bar
    // while it is still parented to the main window.
    panel->hide();
    m_impl->panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout();
    m_impl->sessions = new QComboBox(panel);
    row->addWidget(m_impl->sessions);
    m_impl->rebuild();
    connect(m_impl->sessions, &QComboBox::activated, this, [this](int index) {
        auto& impl = *m_impl;
        impl.revokeSaveConsent();
        impl.clearDomain({});
        ++impl.epoch;
        if (impl.worker.joinable()) {
            impl.worker.request_stop();
            impl.worker.join();
        }
        impl.busy = false;
        impl.selected = impl.sessions->itemData(index).toString().toUtf8().toStdString();
        impl.explicitSelection = !impl.selected.empty();
        impl.awaiting.reset();
        impl.awaitingExplicit = false;
        if (!impl.selected.empty())
            impl.run("state");
    });
    for (const auto& [title, command] : {std::pair{"State", "state"},
                                         {"Play", "play"},
                                         {"Pause", "pause"},
                                         {"Resume", "resume"},
                                         {"Step", "step"},
                                         {"Stop", "stop"}, {"Refresh Assets", "refresh"}, {"Recompile", "recompile"}}) {
        auto* button = new QPushButton(QString::fromLatin1(title), panel);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { m_impl->run(command); });
    }
    layout->addLayout(row);
    m_impl->status = new QLabel(QStringLiteral("Disconnected"), panel);
    layout->addWidget(m_impl->status);
    layout->addWidget(new QLabel(QStringLiteral("Unity Console"), panel));
    auto* consoleTools = new QHBoxLayout;
    m_impl->filter = new QLineEdit(panel); m_impl->filter->setPlaceholderText(QStringLiteral("Filter Unity output"));
    m_impl->level = new QComboBox(panel);
    m_impl->level->addItem(QStringLiteral("All levels"), QString{});
    m_impl->level->addItem(QStringLiteral("Errors"), QStringLiteral("error"));
    m_impl->level->addItem(QStringLiteral("Warnings"), QStringLiteral("warning"));
    m_impl->level->addItem(QStringLiteral("Info"), QStringLiteral("info"));
    m_impl->collapse = new QCheckBox(QStringLiteral("Collapse"), panel);
    m_impl->clearOnPlay = new QCheckBox(QStringLiteral("Clear on Play"), panel);
    auto* clear = new QPushButton(QStringLiteral("Clear Console View"), panel);
    consoleTools->addWidget(m_impl->filter); consoleTools->addWidget(m_impl->level);
    consoleTools->addWidget(m_impl->collapse); consoleTools->addWidget(m_impl->clearOnPlay); consoleTools->addWidget(clear);
    layout->addLayout(consoleTools);
    m_impl->console = new QTreeWidget(panel);
    m_impl->console->setObjectName(QStringLiteral("unityConsole"));
    m_impl->console->setHeaderLabels({QStringLiteral("Level"), QStringLiteral("Message / Stack Frame"), QStringLiteral("Source"), QStringLiteral("Count")});
    m_impl->console->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(m_impl->console);
    m_impl->eventsStatus = new QLabel(QStringLiteral("Unity event stream unavailable until a compatible package connects"), panel);
    m_impl->eventsStatus->setWordWrap(true); layout->addWidget(m_impl->eventsStatus);
    connect(m_impl->filter, &QLineEdit::textChanged, this, [this] { m_impl->renderConsole(); });
    connect(m_impl->level, &QComboBox::currentIndexChanged, this, [this] { m_impl->renderConsole(); });
    connect(m_impl->collapse, &QCheckBox::toggled, this, [this] { m_impl->renderConsole(); });
    connect(clear, &QPushButton::clicked, this, [this] { m_impl->journal.clear(); m_impl->renderConsole(); });
    connect(m_impl->console, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !m_impl->trusted()) return;
        const auto path = item->data(0, Qt::UserRole).toString();
        const int line = item->data(0, Qt::UserRole + 1).toInt();
        if (!path.isEmpty() && line > 0) (void)openExternalFile(m_impl->workbench, path.toUtf8().toStdString(), line, std::max(1, item->data(0, Qt::UserRole + 2).toInt()), true);
    });
    auto* saveActions = new QHBoxLayout;
    m_impl->refreshOnSave = new QCheckBox(QStringLiteral("Refresh selected Unity editor after saved assets"), panel);
    m_impl->compileOnSave = new QCheckBox(QStringLiteral("Recompile selected Unity editor after saved C#"), panel);
    m_impl->refreshOnSave->setObjectName(QStringLiteral("unityRefreshOnSave"));
    m_impl->compileOnSave->setObjectName(QStringLiteral("unityCompileOnSave"));
    saveActions->addWidget(m_impl->refreshOnSave); saveActions->addWidget(m_impl->compileOnSave);
    layout->addLayout(saveActions);
    connect(m_impl->refreshOnSave, &QCheckBox::toggled, this, [this](bool enabled) { m_impl->captureSaveConsent(m_impl->refreshOnSave, enabled); });
    connect(m_impl->compileOnSave, &QCheckBox::toggled, this, [this](bool enabled) { m_impl->captureSaveConsent(m_impl->compileOnSave, enabled); });
    m_impl->saveTimer.setSingleShot(true);
    connect(&m_impl->saveTimer, &QTimer::timeout, this, [this] { m_impl->flushSavedWork(); });
    const QPointer<UnityControlIntegration> saveOwner(this);
    workbench.m_documents->setSavedObserver(this, [saveOwner](const document::SavedDocumentCompletion& completion) {
        if (saveOwner) saveOwner->m_impl->documentSaved(completion);
    });
    auto added = workbench.m_window->toolWindows().add({"unityControls",
                                                        QStringLiteral("Unity"),
                                                        ui::ToolWindowSide::Bottom,
                                                        std::string(ui::icons::kRunRun),
                                                        {},
                                                        [panel] { return panel; },
                                                        180});
    if (!added) {
        delete panel;
        m_impl->stopped = true;
        m_impl->workbench.m_documents->setSavedObserver(this, {});
        return;
    }
    m_impl->timer.setInterval(500);
    connect(&m_impl->timer, &QTimer::timeout, this, [this] { m_impl->expire(); });
    m_impl->timer.start();
}
UnityControlIntegration::~UnityControlIntegration() { shutdown(); }
Result<void> UnityControlIntegration::announce(const lang::Json& message) { return m_impl->announce(message); }
void UnityControlIntegration::cancel() { m_impl->cancel(); }
void UnityControlIntegration::shutdown() {
    if (m_impl->stopped)
        return;
    m_impl->stopped = true;
    m_impl->workbench.m_documents->setSavedObserver(this, {});
    m_impl->timer.stop();
    m_impl->cancel();
}
} // namespace frost::app
