// SPDX-License-Identifier: Apache-2.0
// Offline C1 transport fixtures; local sockets only, no provider process or outbound connection.
#include "../../src/app/src/agent_bridge.hpp"
#include "../../src/app/src/agent_scope.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <thread>
namespace {
bool pump(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}
QByteArray masked(const QByteArray& payload, bool mask = true, unsigned char first = 0x81) {
    QByteArray frame(1, char(first));
    frame += char((mask ? 0x80 : 0) | (payload.size() < 126 ? payload.size() : 126));
    if (payload.size() >= 126) {
        frame += char(payload.size() >> 8);
        frame += char(payload.size());
    }
    if (!mask)
        return frame + payload;
    const QByteArray key("abcd");
    frame += key;
    auto encoded = payload;
    for (qsizetype i = 0; i < encoded.size(); ++i)
        encoded[i] = char(encoded[i] ^ key[i % 4]);
    return frame + encoded;
}
struct Fixture {
    QTemporaryDir directory;
    bool admitted = true;
    int calls = 0;
    frost::app::AgentBridge bridge{
        [this] { return admitted; },
        [this](const QString&, const QJsonObject&, const frost::app::AgentBridge::Reply& reply) {
            ++calls;
            reply({{"fixture", true}});
        }};
    QJsonObject record() const {
        QFile file(QDir(directory.path()).filePath("ide/" + QString::number(bridge.port()) + ".lock"));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    void connect(QTcpSocket& client, const QByteArray& extra = {}, QByteArray token = {}) {
        if (token.isEmpty())
            token = record().value("authToken").toString().toLatin1();
        client.connectToHost(QHostAddress::LocalHost, bridge.port());
        ASSERT_TRUE(pump([&] { return client.state() == QAbstractSocket::ConnectedState; }));
        client.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: Upgrade\r\nUpgrade: "
                     "websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
                     "YWJjZGVmZ2hpamtsbW5vcA==\r\nSec-WebSocket-Protocol: mcp\r\nX-Claude-Code-Ide-Authorization: " +
                     token + "\r\n" + extra + "\r\n");
        ASSERT_TRUE(
            pump([&] { return client.bytesAvailable() > 0 || client.state() == QAbstractSocket::UnconnectedState; }));
    }
};
TEST(AgentBridge, RefusesWithoutConsentAndPublishesWindowScopedPrivateRecord) {
    Fixture fixture;
    fixture.admitted = false;
    EXPECT_FALSE(fixture.bridge.start("/workspace", fixture.directory.path()));
    EXPECT_EQ(fixture.bridge.port(), 0);
    fixture.admitted = true;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    const auto record = fixture.record();
    EXPECT_EQ(record.value("workspaceFolders").toArray(), QJsonArray{"/workspace"});
    EXPECT_EQ(record.value("authToken").toString().size(), 32);
    EXPECT_EQ(record.value("ideName"), "FrostIDE");
    const auto lock =
        QDir(fixture.directory.path()).filePath("ide/" + QString::number(fixture.bridge.port()) + ".lock");
    fixture.bridge.stop();
    EXPECT_FALSE(QFile::exists(lock));
}
TEST(AgentBridge, RejectsOriginAndAnotherWindowsTokenWithoutDispatch) {
    Fixture first, second;
    ASSERT_TRUE(first.bridge.start("/one", first.directory.path()));
    ASSERT_TRUE(second.bridge.start("/two", second.directory.path()));
    QTcpSocket browser;
    first.connect(browser, "Origin: https://example.invalid\r\n");
    EXPECT_EQ(browser.state(), QAbstractSocket::UnconnectedState);
    QTcpSocket wrongWindow;
    first.connect(wrongWindow, {}, second.record().value("authToken").toString().toLatin1());
    EXPECT_EQ(wrongWindow.state(), QAbstractSocket::UnconnectedState);
    EXPECT_EQ(first.calls, 0);
}
TEST(AgentBridge, RequiresInitializationAndMaskedFramesThenRevocationDisconnects) {
    Fixture fixture;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    QTcpSocket client;
    fixture.connect(client);
    EXPECT_TRUE(client.readAll().contains("101 Switching Protocols"));
    client.write(masked(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
    ASSERT_TRUE(pump([&] { return client.bytesAvailable() > 0; }));
    EXPECT_TRUE(client.readAll().contains("initialize is required"));
    EXPECT_EQ(fixture.calls, 0);
    client.write(masked(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})"));
    ASSERT_TRUE(pump([&] { return client.bytesAvailable() > 0; }));
    EXPECT_TRUE(client.readAll().contains("FrostIDE"));
    client.write(masked(R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})"));
    ASSERT_TRUE(pump([&] { return fixture.calls == 1; }));
    fixture.admitted = false;
    fixture.bridge.notify("selection_changed", {});
    ASSERT_TRUE(pump([&] { return client.state() == QAbstractSocket::UnconnectedState; }));
    EXPECT_EQ(fixture.bridge.port(), 0);
    EXPECT_TRUE(QDir(fixture.directory.path() + "/ide").entryList({"*.lock"}, QDir::Files).empty());
}
TEST(AgentBridge, RejectsUnmaskedDataBeforeDispatch) {
    Fixture fixture;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    QTcpSocket client;
    fixture.connect(client);
    client.readAll();
    client.write(masked(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", false));
    ASSERT_TRUE(pump([&] { return client.state() == QAbstractSocket::UnconnectedState; }));
    EXPECT_EQ(fixture.calls, 0);
}
TEST(AgentScope, DeniesSiblingPrefixTraversalDirectoriesAndCredentialResources) {
    QTemporaryDir temp;
    const auto root = QDir(temp.path()).filePath("workspace");
    const auto sibling = QDir(temp.path()).filePath("workspace-other");
    ASSERT_TRUE(QDir().mkpath(root));
    ASSERT_TRUE(QDir().mkpath(sibling));
    const auto write = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
    };
    ASSERT_TRUE(write(root + "/main.lua"));
    ASSERT_TRUE(write(sibling + "/secret.txt"));
    ASSERT_TRUE(write(root + "/.credentials.json"));
    auto& fs = frost::io::realFileSystem();
    EXPECT_TRUE(frost::app::agentWorkspaceResource(fs, root.toStdString(), (root + "/main.lua").toStdString()));
    EXPECT_FALSE(frost::app::agentWorkspaceResource(fs, root.toStdString(), (sibling + "/secret.txt").toStdString()));
    EXPECT_FALSE(frost::app::agentWorkspaceResource(fs, root.toStdString(),
                                                    (root + "/../workspace-other/secret.txt").toStdString()));
    EXPECT_FALSE(frost::app::agentWorkspaceResource(fs, root.toStdString(), root.toStdString()));
    EXPECT_FALSE(
        frost::app::agentWorkspaceResource(fs, root.toStdString(), (root + "/.credentials.json").toStdString()));
    EXPECT_FALSE(frost::app::agentWorkspaceResource(fs, root.toStdString(), "https://example.invalid/main.lua"));
}
TEST(AgentBridge, ReassemblesBoundedTextFragmentsAndRejectsUnexpectedContinuation) {
    Fixture fixture;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    QTcpSocket client;
    fixture.connect(client);
    client.readAll();
    const QByteArray init(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    client.write(masked(init.left(20), true, 0x01) + masked(init.mid(20), true, 0x80));
    ASSERT_TRUE(pump([&] { return client.bytesAvailable() > 0; }));
    EXPECT_TRUE(client.readAll().contains("FrostIDE"));
    client.write(masked("unexpected", true, 0x80));
    ASSERT_TRUE(pump([&] { return client.state() == QAbstractSocket::UnconnectedState; }));
    EXPECT_EQ(fixture.calls, 0);
}
TEST(AgentBridge, RejectsCompletedOversizedHandshakeBeforeDispatch) {
    Fixture fixture;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    QTcpSocket client;
    fixture.connect(client, "X-Fill: " + QByteArray(17000, 'x') + "\r\n");
    EXPECT_EQ(client.state(), QAbstractSocket::UnconnectedState);
    EXPECT_EQ(fixture.calls, 0);
}
TEST(AgentBridge, DisconnectsSlowReaderBeforeControlOutputCanGrowWithoutBound) {
    Fixture fixture;
    ASSERT_TRUE(fixture.bridge.start("/workspace", fixture.directory.path()));
    QTcpSocket client;
    fixture.connect(client);
    client.readAll();
    client.setReadBufferSize(1);
    const auto ping = masked(QByteArray(125, 'p'), true, 0x89);
    QElapsedTimer deadline;
    deadline.start();
    // Small input chunks and event pumping avoid triggering the inbound frame/body cap. The
    // unread peer receive window eventually fills; every pong must share the outbound budget.
    for (int count = 0; count < 100000 && deadline.elapsed() < 10000 && fixture.bridge.clientCount(); ++count) {
        if (client.bytesToWrite() < 4096)
            client.write(ping);
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(pump([&] { return fixture.bridge.clientCount() == 0; }));
    EXPECT_EQ(fixture.calls, 0);
}
} // namespace
