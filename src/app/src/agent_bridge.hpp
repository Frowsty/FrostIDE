// SPDX-License-Identifier: Apache-2.0
// Per-window opt-in Claude Code WebSocket/MCP transport; no process launch or credential access.
#pragma once
#include "frost/core/error.hpp"
#include <QObject>
#include <QJsonObject>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <map>
class QTcpSocket;
namespace frost::app {
class AgentBridge final : public QObject {
public:
    struct Reply {
        std::function<void(QJsonObject)> deliver;
        std::function<bool()> alive;
        void operator()(QJsonObject result) const { if (deliver) deliver(std::move(result)); }
        [[nodiscard]] bool connected() const { return alive && alive(); }
    };
    using Dispatch = std::function<void(const QString&, const QJsonObject&, Reply)>;
    // GUI thread only. Admission is rechecked for every request and notification; false disconnects.
    explicit AgentBridge(std::function<bool()> admission, Dispatch dispatch, QObject* parent = nullptr);
    ~AgentBridge() override;
    [[nodiscard]] Result<void> start(const QString& root, const QString& configDirectory);
    void stop();
    void notify(const QString& method, const QJsonObject& params);
    [[nodiscard]] quint16 port() const { return m_server.serverPort(); }
    [[nodiscard]] int clientCount() const;
private:
    struct Client { QByteArray bytes; QByteArray fragments; bool fragmenting = false; bool upgraded = false; bool initialized = false; int pending = 0; qint64 seen = 0; };
    void read(QTcpSocket* socket);
    void message(QTcpSocket* socket, const QByteArray& bytes);
    void send(QTcpSocket* socket, const QJsonObject& object);
    bool writeBounded(QTcpSocket* socket, const QByteArray& bytes);
    std::function<bool()> m_admission;
    Dispatch m_dispatch;
    QTcpServer m_server;
    QTimer m_expiry;
    std::map<QTcpSocket*, Client> m_clients;
    QByteArray m_token;
    QString m_lock;
};
}
