// SPDX-License-Identifier: Apache-2.0
// Unity session trust, identity, selection and cancellation at the workbench boundary.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif
#include "frost/lang/json.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "workbench_rig.hpp"
#include <QComboBox>
#include <QLabel>
#include <QMessageAuthenticationCode>
#include <QPushButton>

#include <atomic>
#include <thread>

namespace {
using namespace std::chrono_literals;
using namespace frost::app::test;

TEST(WorkbenchUnity, ControlsStayHiddenUntilTheirToolWindowOpens) {
    WorkDir dir("unity-panel-visibility");
    WorkbenchRig rig(dir);
    auto* panel = rig.window->findChild<QWidget*>(QStringLiteral("unityControlsPanel"));
    ASSERT_NE(panel, nullptr);
    rig.show();
    EXPECT_FALSE(panel->isVisible());
    rig.window->toolWindows().show("unityControls");
    ASSERT_EQ(rig.window->toolWindows().content("unityControls"), panel);
    EXPECT_TRUE(panel->isVisible());
}

frost::lang::Json announcement(const std::string& root, char session, int pid = 123) {
    return frost::lang::Json::object()
        .set("version", 2)
        .set("root", root)
        .set("instance", std::string(32, session))
        .set("generation", 1)
        .set("session", std::string(32, session))
        .set("token", std::string(64, 'a'))
        .set("pid", pid)
        .set("port", 1);
}
QPushButton* button(QWidget* panel, const QString& text) {
    for (auto* item : panel->findChildren<QPushButton*>())
        if (item->text() == text)
            return item;
    return nullptr;
}
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
void closeSocket(Socket value) { closesocket(value); }
#else
using Socket = int;
constexpr Socket invalidSocket = -1;
void closeSocket(Socket value) { close(value); }
#endif
QByteArray fixtureMac(std::string_view key, std::initializer_list<std::string_view> fields) {
    QByteArray data;
    for (auto field : fields) {
        auto length = static_cast<std::uint32_t>(field.size());
        for (int shift = 24; shift >= 0; shift -= 8)
            data.append(static_cast<char>((length >> shift) & 255));
        data.append(field.data(), static_cast<qsizetype>(field.size()));
    }
    return QMessageAuthenticationCode::hash(data, QByteArray(key.data(), static_cast<qsizetype>(key.size())),
                                            QCryptographicHash::Sha256)
        .toHex();
}
class UnityServer {
public:
    UnityServer(std::string project, std::string session, std::string token, bool playing)
        : root(std::move(project)), id(std::move(session)), secret(std::move(token)), playing(playing) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        initialized = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == invalidSocket)
            return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 4) != 0)
            return;
#ifdef _WIN32
        int length = sizeof(addr);
#else
        socklen_t length = sizeof(addr);
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length) != 0)
            return;
        port = ntohs(addr.sin_port);
        worker = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~UnityServer() {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
        if (listener != invalidSocket)
            closeSocket(listener);
#ifdef _WIN32
        if (initialized)
            WSACleanup();
#endif
    }
    std::uint16_t port = 0;
    std::atomic<bool> holdPlay{false}, playSeen{false}, release{false}, badProof{false};
    std::atomic<int> states{0}, validRequests{0}, greetings{0}, requestFrames{0};

private:
    static std::string readLine(Socket peer) {
        std::string result;
        while (result.size() < 8192) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(peer, &set);
            timeval timeout{1, 0};
            if (select(static_cast<int>(peer + 1), &set, nullptr, nullptr, &timeout) <= 0)
                return {};
            char c = 0;
            if (recv(peer, &c, 1, 0) != 1)
                return {};
            if (c == '\n')
                return result;
            result.push_back(c);
        }
        return {};
    }
    static bool sendLine(Socket peer, const std::string& value) {
        const std::string bytes = value + "\n";
#ifdef _WIN32
        return send(peer, bytes.data(), static_cast<int>(bytes.size()), 0) == static_cast<int>(bytes.size());
#else
        return send(peer, bytes.data(), bytes.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
#endif
    }
    void serve(std::stop_token stop) {
        while (!stop.stop_requested()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval timeout{0, 100000};
            if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0)
                continue;
            Socket peer = accept(listener, nullptr, nullptr);
            if (peer == invalidSocket)
                continue;
            auto hello = frost::lang::parseJson(readLine(peer));
            if (!hello || hello->stringAt("session") != id || hello->integerAt("version") != 2) {
                closeSocket(peer);
                continue;
            }
            const std::string challenge(hello->stringAt("challenge")), nonce(64, 'e');
            const auto proof =
                badProof.load()
                    ? std::string(64, '0')
                    : fixtureMac(secret, {"frost-unity-server-v2", root, id, challenge, nonce}).toStdString();
            auto greeting =
                frost::lang::Json::object().set("session", id).set("nonce", nonce).set("proof", proof).dump();
            ++greetings;
            if (!sendLine(peer, greeting)) {
                closeSocket(peer);
                continue;
            }
            const std::string frame = readLine(peer);
            if (!frame.empty())
                ++requestFrames;
            auto request = frost::lang::parseJson(frame);
            if (!request) {
                closeSocket(peer);
                continue;
            }
            const std::string command(request->stringAt("command")), requestId(request->stringAt("id"));
            const auto mac =
                fixtureMac(secret, {"frost-unity-request-v2", root, id, requestId, command, "4500", challenge, nonce})
                    .toStdString();
            if (request->find("token") || request->stringAt("mac") != mac || request->stringAt("nonce") != nonce ||
                request->stringAt("project") != root || request->integerAt("version") != 2) {
                closeSocket(peer);
                continue;
            }
            ++validRequests;
            if (command == "state")
                ++states;
            if (command == "play" && holdPlay.load()) {
                playSeen.store(true);
                for (int i = 0; i < 100 && !release.load() && !stop.stop_requested(); ++i)
                    std::this_thread::sleep_for(20ms);
            }
            auto result = frost::lang::Json::object()
                              .set("id", requestId)
                              .set("session", id)
                              .set("ok", true)
                              .set("playing", playing)
                              .set("paused", false)
                              .set("frame", 3)
                              .set("message", "observed")
                              .dump();
            (void)sendLine(peer, result);
            closeSocket(peer);
        }
    }
    Socket listener = invalidSocket;
    std::string root, id, secret;
    bool playing = false;
#ifdef _WIN32
    bool initialized = false;
#endif
    std::jthread worker;
};
bool hasStatus(QWidget* panel, const QString& text) {
    for (auto* label : panel->findChildren<QLabel*>())
        if (label->text().contains(text))
            return true;
    return false;
}
TEST(WorkbenchUnity, RefusesUntrustedWrongProjectAndMutatedSession) {
    WorkDir dir("unity-control-trust");
    dir.write("first/Assets/a.cs", "");
    dir.write("first/Packages/manifest.json", "{}");
    dir.write("first/ProjectSettings/ProjectVersion.txt", "scratch");
    dir.write("second/Assets/a.cs", "");
    dir.write("second/Packages/manifest.json", "{}");
    dir.write("second/ProjectSettings/ProjectVersion.txt", "scratch");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openExternalProject(dir.join("first")));
    auto first = announcement(dir.join("first"), 'a');
    EXPECT_FALSE(rig.workbench->announceUnity(first));
    ASSERT_TRUE(rig.trust->trust(dir.join("first")));
    EXPECT_FALSE(rig.workbench->announceUnity(announcement(dir.join("second"), 'b')));
    EXPECT_TRUE(rig.workbench->announceUnity(first));
    auto altered = announcement(dir.join("first"), 'a', 456);
    EXPECT_FALSE(rig.workbench->announceUnity(altered));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto* combo = panel->findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->count(), 2);
    EXPECT_TRUE(rig.workbench->announceUnity(announcement(dir.join("first"), 'b', 456)));
    EXPECT_EQ(combo->count(), 3);
    EXPECT_EQ(combo->currentIndex(), 0); // Two editors require an explicit choice.
    ASSERT_TRUE(rig.workbench->openExternalProject(dir.join("second")));
    EXPECT_EQ(combo->count(), 1);
}
TEST(WorkbenchUnity, DoesNotSendControlAfterInvalidServerProof) {
    WorkDir dir("unity-control-bad-proof");
    dir.write("project/Assets/a.cs", "");
    dir.write("project/Packages/manifest.json", "{}");
    dir.write("project/ProjectSettings/ProjectVersion.txt", "scratch");
    const auto root = dir.join("project");
    UnityServer server(root, std::string(32, 'a'), std::string(64, 'a'), false);
    ASSERT_NE(server.port, 0);
    server.badProof.store(true);
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto message = announcement(root, 'a');
    message.set("port", server.port);
    ASSERT_TRUE(rig.workbench->announceUnity(message));
    ASSERT_TRUE(
        waitFor([&] { return server.greetings.load() == 1 && hasStatus(panel, QStringLiteral("uncertain")); }, 5s));
    static_cast<void>(waitFor([] { return false; }, 200ms));
    EXPECT_EQ(server.requestFrames.load(), 0);
    EXPECT_EQ(server.validRequests.load(), 0);
}
TEST(WorkbenchUnity, ReloadCancelsOldRequestAndFetchesNewObservedState) {
    WorkDir dir("unity-control-reload-server");
    dir.write("project/Assets/a.cs", "");
    dir.write("project/Packages/manifest.json", "{}");
    dir.write("project/ProjectSettings/ProjectVersion.txt", "scratch");
    const auto root = dir.join("project");
    const std::string firstId(32, 'a'), nextId(32, 'b'), token(64, 'a');
    UnityServer first(root, firstId, token, false), next(root, nextId, token, true);
    ASSERT_NE(first.port, 0);
    ASSERT_NE(next.port, 0);
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto initial = announcement(root, 'a');
    initial.set("port", first.port);
    ASSERT_TRUE(rig.workbench->announceUnity(initial));
    ASSERT_TRUE(
        waitFor([&] { return first.states.load() == 1 && hasStatus(panel, QStringLiteral("Unity stopped")); }, 5s));
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(play, nullptr);
    first.holdPlay.store(true);
    play->click();
    ASSERT_TRUE(waitFor([&] { return first.playSeen.load(); }, 5s));
    auto reloaded = initial;
    reloaded.set("generation", 2).set("session", nextId).set("port", next.port);
    ASSERT_TRUE(rig.workbench->announceUnity(reloaded));
    ASSERT_TRUE(
        waitFor([&] { return next.states.load() == 1 && hasStatus(panel, QStringLiteral("Unity playing")); }, 5s));
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("previous request interrupted")));
    first.release.store(true);
    static_cast<void>(waitFor([] { return false; }, 300ms));
    EXPECT_TRUE(hasStatus(panel, QStringLiteral("Unity playing")));
    EXPECT_EQ(first.validRequests.load(), 2);
    EXPECT_EQ(next.validRequests.load(), 1);
    EXPECT_FALSE(rig.workbench->announceUnity(initial));
}
TEST(WorkbenchUnity, ReloadKeepsSelectedInstanceAndRejectsOldDomain) {
    WorkDir dir("unity-control-reload");
    dir.write("project/Assets/a.cs", "");
    dir.write("project/Packages/manifest.json", "{}");
    dir.write("project/ProjectSettings/ProjectVersion.txt", "scratch");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openExternalProject(dir.join("project")));
    auto first = announcement(dir.join("project"), 'a');
    ASSERT_TRUE(rig.workbench->announceUnity(first));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto* combo = panel->findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toString(), QString(32, QLatin1Char('a')));
    auto newer = first;
    newer.set("generation", 2).set("session", std::string(32, 'b')).set("token", std::string(64, 'c'));
    ASSERT_TRUE(rig.workbench->announceUnity(newer));
    EXPECT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->currentData().toString(), QString(32, QLatin1Char('b')));
    EXPECT_FALSE(rig.workbench->announceUnity(first));
    auto peer = announcement(dir.join("project"), 'd', 456);
    ASSERT_TRUE(rig.workbench->announceUnity(peer));
    EXPECT_EQ(combo->currentIndex(), 0); // An unrelated editor forces an explicit selection.
    const int chosen = combo->findData(QString(32, QLatin1Char('b')));
    ASSERT_GT(chosen, 0);
    combo->setCurrentIndex(chosen);
    ASSERT_TRUE(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, chosen)));
    auto third = newer;
    third.set("generation", 3).set("session", std::string(32, 'e')).set("token", std::string(64, 'f'));
    ASSERT_TRUE(rig.workbench->announceUnity(third));
    EXPECT_EQ(combo->currentData().toString(), QString(32, QLatin1Char('e')));
    EXPECT_FALSE(rig.workbench->announceUnity(newer));
}
TEST(WorkbenchUnity, ExpiredSelectedSessionResumesOnlyWithIdenticalIdentity) {
    WorkDir dir("unity-control-heartbeat-gap");
    dir.write("project/Assets/a.cs", "");
    dir.write("project/Packages/manifest.json", "{}");
    dir.write("project/ProjectSettings/ProjectVersion.txt", "scratch");
    const auto root = dir.join("project");
    UnityServer server(root, std::string(32, 'a'), std::string(64, 'a'), false);
    ASSERT_NE(server.port, 0);
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(root));
    ASSERT_TRUE(rig.workbench->openExternalProject(root));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto* combo = panel->findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    auto original = announcement(root, 'a');
    original.set("generation", 2).set("port", server.port);
    ASSERT_TRUE(rig.workbench->announceUnity(original));
    ASSERT_TRUE(
        waitFor([&] { return server.states.load() == 1 && hasStatus(panel, QStringLiteral("Unity stopped")); }, 5s));
    ASSERT_TRUE(waitFor([&] { return combo->count() == 1 && hasStatus(panel, QStringLiteral("disconnected")); }, 10s));
    auto older = original;
    older.set("generation", 1).set("session", std::string(32, 'b'));
    EXPECT_FALSE(rig.workbench->announceUnity(older));
    auto altered = original;
    altered.set("token", std::string(64, 'c'));
    EXPECT_FALSE(rig.workbench->announceUnity(altered));
    altered = original;
    altered.set("port", 1);
    EXPECT_FALSE(rig.workbench->announceUnity(altered));
    altered = original;
    altered.set("session", std::string(32, 'b'));
    EXPECT_FALSE(rig.workbench->announceUnity(altered));
    ASSERT_TRUE(rig.workbench->announceUnity(original));
    EXPECT_EQ(combo->currentData().toString(), QString(32, QLatin1Char('a')));
    EXPECT_EQ(combo->count(), 2);
    ASSERT_TRUE(
        waitFor([&] { return server.states.load() == 2 && hasStatus(panel, QStringLiteral("Unity stopped")); }, 5s));
}
TEST(WorkbenchUnity, RevocationRemovesSessionsAndRefusesFurtherCommands) {
    WorkDir dir("unity-control-revoke");
    dir.write("project/Assets/a.cs", "");
    dir.write("project/Packages/manifest.json", "{}");
    dir.write("project/ProjectSettings/ProjectVersion.txt", "scratch");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.trust->trust(dir.join("project")));
    ASSERT_TRUE(rig.workbench->openExternalProject(dir.join("project")));
    auto message = announcement(dir.join("project"), 'c');
    ASSERT_TRUE(rig.workbench->announceUnity(message));
    rig.window->toolWindows().show("unityControls");
    auto* panel = rig.window->toolWindows().content("unityControls");
    ASSERT_NE(panel, nullptr);
    auto* combo = panel->findChild<QComboBox*>();
    auto* play = button(panel, QStringLiteral("Play"));
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(play, nullptr);
    ASSERT_TRUE(rig.trust->untrust(dir.join("project")));
    play->click();
    EXPECT_EQ(combo->count(), 1);
    EXPECT_FALSE(rig.workbench->announceUnity(message));
}
} // namespace
