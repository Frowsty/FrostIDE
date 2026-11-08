// SPDX-License-Identifier: Apache-2.0
//
// A client of the automation endpoint (src/app/src/automation_endpoint.hpp; FND-7a, ADR 0080) for
// tests that run the endpoint in their own process over a WorkbenchRig (tests/app/workbench_rig.hpp).
// The out-of-process journeys (tests/journeys) speak the same protocol from Python.
#pragma once

#include "frost/rpc/json.hpp"

#include <QApplication>
#include <QDeadlineTimer>
#include <QLocalSocket>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace frost::test {

/*
 * Connects to `serverName` and sends one JSON request per line. request() processes Qt events while
 * it waits (the endpoint answers on this same GUI thread) and returns the reply object, or a null
 * Json when none came within `timeout` or the server closed the connection first. GUI thread only.
 */
class EndpointClient {
public:
    explicit EndpointClient(const std::string& serverName) {
        QObject::connect(&m_socket, &QLocalSocket::readyRead, &m_socket, [this] {
            const QByteArray bytes = m_socket.readAll();
            m_buffer.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        });
        m_socket.connectToServer(QString::fromStdString(serverName));
        pump([this] { return m_socket.state() == QLocalSocket::ConnectedState; }, std::chrono::seconds(10));
    }
    EndpointClient(const EndpointClient&) = delete ("owns a socket");
    EndpointClient& operator=(const EndpointClient&) = delete ("owns a socket");

    [[nodiscard]] bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    rpc::Json request(std::string_view method, rpc::Json params = rpc::Json::object(),
                      std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        const std::int64_t id = ++m_nextId;
        std::string line = rpc::Json::object()
                               .set("id", id)
                               .set("method", std::string(method))
                               .set("params", std::move(params))
                               .dump();
        line += '\n';
        m_socket.write(line.data(), static_cast<qint64>(line.size()));
        m_socket.flush();
        rpc::Json reply;
        pump(
            [&] {
                for (std::size_t end = m_buffer.find('\n'); end != std::string::npos; end = m_buffer.find('\n')) {
                    auto parsed = rpc::parseJson(std::string_view(m_buffer).substr(0, end));
                    m_buffer.erase(0, end + 1);
                    if (parsed && parsed->integerAt("id") == id) {
                        reply = std::move(*parsed);
                        return true;
                    }
                }
                return m_socket.state() != QLocalSocket::ConnectedState;
            },
            timeout);
        return reply;
    }

private:
    // Processes events until `done` or `timeout`; returns the evaluation that ended the wait.
    template<typename Done>
    static bool pump(Done&& done, std::chrono::milliseconds timeout) {
        const QDeadlineTimer deadline(timeout);
        for (;;) {
            if (done())
                return true;
            if (deadline.hasExpired())
                return false;
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    QLocalSocket m_socket;
    std::string m_buffer;
    std::int64_t m_nextId = 0;
};

} // namespace frost::test
