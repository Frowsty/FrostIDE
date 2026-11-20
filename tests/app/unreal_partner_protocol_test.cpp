// SPDX-License-Identifier: Apache-2.0
// Authored synthetic loopback peer. Verifies client authority/transport, never real Unreal acceptance.
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/json.hpp"

#include "../../src/app/src/unreal_partner_window.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <gtest/gtest.h>

#include <functional>
namespace frost::app {
namespace {
bool pump(const std::function<bool()>& ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
} // namespace
TEST(UnrealPartnerProtocol, MatchingObservedRowsAreClearedOnRevokeRegrant) {
    io::MemFs fs(systemClock());
    const auto root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/Synthetic" : "/Synthetic";
    const auto project = io::joinPath(root, "Oracle.uproject").value();
    auto trust = project::TrustStore::open(fs, io::joinPath(root, "trust.txt").value());
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root));
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress(QStringLiteral("127.0.0.1")), 0));
    UnrealPartnerWindow window(project, **trust);
    auto* port = window.findChild<QSpinBox*>(QStringLiteral("unrealPartnerPort"));
    auto* token = window.findChild<QLineEdit*>(QStringLiteral("unrealPartnerToken"));
    auto* connectButton = window.findChild<QPushButton*>(QStringLiteral("unrealPartnerConnect"));
    auto* calls = window.findChild<QTableWidget*>(QStringLiteral("unrealBlueprintCalls"));
    ASSERT_TRUE(port && token && connectButton && calls);
    port->setValue(server.serverPort());
    token->setText(QString(64, QLatin1Char('a')));
    connectButton->click();
    ASSERT_TRUE(pump([&] { return server.hasPendingConnections(); }));
    auto* peer = server.nextPendingConnection();
    ASSERT_NE(peer, nullptr);
    QByteArray received;
    ASSERT_TRUE(pump([&] {
        received += peer->readAll();
        return received.contains('\n');
    }));
    auto request =
        rpc::parseJsonStrict(std::string_view(received.constData(), static_cast<std::size_t>(received.indexOf('\n'))));
    ASSERT_TRUE(request);
    EXPECT_EQ(request->stringAt("op"), "state");
    auto envelope = rpc::Json::object().set("schema", 1).set("project", request->stringAt("project"));
    auto output = rpc::Json(envelope).set("kind", "state").set("playing", false).set("paused", false).dump() + "\n" +
                  rpc::Json(envelope).set("kind", "reply").set("id", *request->find("id")).set("status", "ok").dump() +
                  "\n";
    ASSERT_EQ(peer->write(output.data(), static_cast<qint64>(output.size())), static_cast<qint64>(output.size()));
    auto* refresh = window.findChild<QPushButton*>(QStringLiteral("unrealPartnerOperation.blueprints.loadedCalls"));
    ASSERT_NE(refresh, nullptr);
    ASSERT_TRUE(pump([&] { return refresh->isEnabled(); }));
    refresh->click();
    received.clear();
    ASSERT_TRUE(pump([&] {
        received += peer->readAll();
        return received.contains('\n');
    }));
    request =
        rpc::parseJsonStrict(std::string_view(received.constData(), static_cast<std::size_t>(received.indexOf('\n'))));
    ASSERT_TRUE(request);
    EXPECT_EQ(request->stringAt("op"), "blueprints.loadedCalls");
    auto node = rpc::Json::object()
                    .set("blueprint", "/Game/Authored.Authored")
                    .set("function", "/Script/Authored.Oracle:Add")
                    .set("node", std::string(32, 'b'));
    output = rpc::Json(envelope)
                 .set("kind", "reply")
                 .set("id", *request->find("id"))
                 .set("status", "ok")
                 .set("scope", "loaded-blueprint-graphs")
                 .set("calls", rpc::Json::array({node}))
                 .dump() +
             "\n";
    ASSERT_EQ(peer->write(output.data(), static_cast<qint64>(output.size())), static_cast<qint64>(output.size()));
    ASSERT_TRUE(pump([&] { return calls->rowCount() == 1; }));
    EXPECT_EQ(calls->item(0, 0)->text(), QStringLiteral("/Game/Authored.Authored"));
    auto* filter = window.findChild<QLineEdit*>(QStringLiteral("unrealBlueprintFilter"));
    ASSERT_NE(filter, nullptr);
    filter->setText(QStringLiteral("Wanted"));
    EXPECT_TRUE(calls->isRowHidden(0));
    received.clear();
    refresh->click();
    ASSERT_TRUE(pump([&] {
        received += peer->readAll();
        return received.contains('\n');
    }));
    request =
        rpc::parseJsonStrict(std::string_view(received.constData(), static_cast<std::size_t>(received.indexOf('\n'))));
    ASSERT_TRUE(request);
    auto wanted = rpc::Json(node).set("blueprint", "/Game/Wanted.Wanted").set("node", std::string(32, 'c'));
    auto other = rpc::Json(node).set("blueprint", "/Game/Other.Other").set("node", std::string(32, 'd'));
    output = rpc::Json(envelope)
                 .set("kind", "reply")
                 .set("id", *request->find("id"))
                 .set("status", "ok")
                 .set("scope", "loaded-blueprint-graphs")
                 .set("calls", rpc::Json::array({wanted, other}))
                 .dump() +
             "\n";
    ASSERT_EQ(peer->write(output.data(), static_cast<qint64>(output.size())), static_cast<qint64>(output.size()));
    ASSERT_TRUE(pump([&] { return calls->rowCount() == 2; }));
    EXPECT_EQ(filter->text(), QStringLiteral("Wanted"));
    EXPECT_FALSE(calls->isRowHidden(0));
    EXPECT_TRUE(calls->isRowHidden(1));
    filter->clear();
    EXPECT_FALSE(calls->isRowHidden(0));
    EXPECT_FALSE(calls->isRowHidden(1));
    ASSERT_TRUE((*trust)->untrust(root));
    ASSERT_TRUE((*trust)->trust(root));
    ASSERT_TRUE(pump([&] { return calls->rowCount() == 0 && !refresh->isEnabled(); }));
    EXPECT_TRUE(token->text().isEmpty());
}
} // namespace frost::app
