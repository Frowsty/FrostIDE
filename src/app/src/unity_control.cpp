// SPDX-License-Identifier: Apache-2.0
// Trusted Unity editor discovery and acknowledged play controls.
#include "unity_control.hpp"

#include "frost/app/workbench.hpp"
#include "frost/io/loopback_stream.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "project_open_broker.hpp"
#include <QComboBox>
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
    void cancel() {
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
            if (it->root != root || it->instance != instance || it->generation != *generation || it->token != token ||
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
                        static_cast<std::uint16_t>(*port), std::chrono::steady_clock::now()});
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
    }
    void run(std::string command) {
        if (!trusted()) {
            cancel();
            if (status)
                status->setText(QStringLiteral("Trust the Unity project first"));
            return;
        }
        if (busy)
            return;
        const auto found = std::ranges::find_if(live, [&](const auto& s) { return s.id == selected; });
        if (found == live.end()) {
            status->setText(QStringLiteral("Select a connected Unity editor"));
            return;
        }
        Session target = *found;
        if (worker.joinable())
            worker.join();
        busy = true;
        const auto round = ++epoch;
        const auto id = randomHex(32);
        const auto challenge = randomHex(64);
        status->setText(QStringLiteral("Waiting for Unity…"));
        QPointer<UnityControlIntegration> receiver(&owner);
        auto* trust = &workbench.m_services.trust;
        const auto trustRoot = workbench.projectRoot();
        worker = std::jthread([receiver, round, target, id, challenge, trust, trustRoot,
                               command = std::move(command)](std::stop_token stop) mutable {
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
                    auto reply = (*connection)->exchange(request, 8192);
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
                        if (command != "state")
                            state.interrupted = true;
                        state.status->setText(QStringLiteral("Unity request interrupted; state is uncertain"));
                        return;
                    }
                    if (!result->boolAt("ok").value_or(false)) {
                        state.status->setText(qs(result->stringAt("message")));
                        return;
                    }
                    const bool playing = result->boolAt("playing").value_or(false),
                               paused = result->boolAt("paused").value_or(false);
                    QString observed = !playing ? QStringLiteral("Unity stopped")
                                       : paused ? QStringLiteral("Unity paused")
                                                : QStringLiteral("Unity playing");
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
    m_impl->panel = panel;
    auto* layout = new QVBoxLayout(panel);
    auto* row = new QHBoxLayout();
    m_impl->sessions = new QComboBox(panel);
    row->addWidget(m_impl->sessions);
    m_impl->rebuild();
    connect(m_impl->sessions, &QComboBox::activated, this, [this](int index) {
        auto& impl = *m_impl;
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
                                         {"Stop", "stop"}}) {
        auto* button = new QPushButton(QString::fromLatin1(title), panel);
        row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, command] { m_impl->run(command); });
    }
    layout->addLayout(row);
    m_impl->status = new QLabel(QStringLiteral("Disconnected"), panel);
    layout->addWidget(m_impl->status);
    auto added = workbench.m_window->toolWindows().add(
        {"unityControls", QStringLiteral("Unity"), ui::ToolWindowSide::Bottom, {}, {}, [panel] { return panel; }, 180});
    if (!added) {
        delete panel;
        m_impl->stopped = true;
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
    m_impl->timer.stop();
    m_impl->cancel();
}
} // namespace frost::app
