// SPDX-License-Identifier: Apache-2.0
// Bounded RFC6455 masked text transport and private, current-user CLI discovery record.
#include "agent_bridge.hpp"
#include "frost/io/utf.hpp"
#include <QPointer>
#include <vector>
#include <memory>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpSocket>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#endif
namespace frost::app {
namespace {
constexpr qsizetype kMaximum = 1024 * 1024;
bool equalToken(const QByteArray& left, const QByteArray& right) {
    unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
    for (qsizetype i = 0; i < right.size(); ++i)
        difference |= static_cast<unsigned char>(i < left.size() ? left[i] : 0) ^ static_cast<unsigned char>(right[i]);
    return difference == 0;
}
void cleanStaleRecords(const QString& directory) {
    const auto candidates = QDir(directory).entryInfoList({"*.lock"}, QDir::Files | QDir::NoSymLinks);
    for (const auto& candidate : candidates) {
        if (candidate.size() > 16384) continue;
        bool owned = false;
#ifdef _WIN32
        PSID ownerSid = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto name = candidate.absoluteFilePath().toStdWString();
        if (GetNamedSecurityInfoW(name.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &ownerSid, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS) {
            HANDLE token = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                DWORD size = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &size);
                std::vector<std::byte> user(size);
                if (size && GetTokenInformation(token, TokenUser, user.data(), size, &size))
                    owned = EqualSid(ownerSid, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
                CloseHandle(token);
            }
            LocalFree(descriptor);
        }
#else
        struct stat info{};
        owned = lstat(QFile::encodeName(candidate.absoluteFilePath()).constData(), &info) == 0 &&
            S_ISREG(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 0077) == 0;
#endif
        if (!owned) continue;
        QFile file(candidate.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto document = QJsonDocument::fromJson(file.read(16384));
        file.close();
        const auto record = document.object();
        const auto pid = record.value("pid").toInteger();
        if (record.value("ideName") != "FrostIDE" || record.value("transport") != "ws" || pid <= 0 || pid > 0x7fffffff) continue;
        bool dead = false;
#ifdef _WIN32
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (process) {
            DWORD code = STILL_ACTIVE;
            dead = GetExitCodeProcess(process, &code) && code != STILL_ACTIVE;
            CloseHandle(process);
        } else dead = GetLastError() == ERROR_INVALID_PARAMETER;
#else
        dead = kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
#endif
        if (dead) QFile::remove(candidate.absoluteFilePath());
    }
}Result<void> privateRecord(const QString& directory, const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(directory)) return fail(ErrorCode::IoError, "cannot create agent discovery directory");
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return fail(ErrorCode::IoError, "cannot identify discovery owner");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    const bool queried = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (!queried) return fail(ErrorCode::IoError, "cannot identify discovery owner");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return fail(ErrorCode::IoError, "cannot format discovery owner");
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return fail(ErrorCode::IoError, "cannot protect agent discovery");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const HANDLE file = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_WRITE, 0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) return fail(ErrorCode::IoError, "cannot create private agent discovery record");
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
#else
    struct stat info{};
    const auto folder = QFile::encodeName(directory);
    if (lstat(folder.constData(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || chmod(folder.constData(), 0700))
        return fail(ErrorCode::PermissionDenied, "agent discovery directory is not privately owned");
    const auto name = QFile::encodeName(path);
    const int file = open(name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0) return fail(ErrorCode::IoError, "cannot create private agent discovery record");
    const auto written = write(file, bytes.constData(), static_cast<std::size_t>(bytes.size()));
    const bool okay = written == bytes.size() && fsync(file) == 0;
    close(file);
#endif
    if (!okay) { QFile::remove(path); return fail(ErrorCode::IoError, "cannot write agent discovery record"); }
    return {};
}
}
AgentBridge::AgentBridge(std::function<bool()> admission, Dispatch dispatch, QObject* parent)
    : QObject(parent), m_admission(std::move(admission)), m_dispatch(std::move(dispatch)) {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (m_server.hasPendingConnections()) {
            auto* socket = m_server.nextPendingConnection();
            if (!m_admission() || m_clients.size() >= 8) { socket->abort(); socket->deleteLater(); continue; }
            socket->setReadBufferSize(kMaximum + 16384);
            m_clients.emplace(socket, Client{{}, {}, false, false, false, 0, QDateTime::currentMSecsSinceEpoch()});
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { m_clients.erase(socket); socket->deleteLater(); });
        }
    });
    m_expiry.setInterval(1000);
    connect(&m_expiry, &QTimer::timeout, this, [this] {
        if (!m_admission()) { stop(); return; }
        const auto clients = m_clients;
        for (const auto& [socket, client] : clients) {
            if (QDateTime::currentMSecsSinceEpoch() - client.seen > (client.upgraded ? 120000 : 5000)) socket->abort();
            else if (client.upgraded) writeBounded(socket, QByteArray::fromHex("8900"));
        }
    });
}
AgentBridge::~AgentBridge() { stop(); }
Result<void> AgentBridge::start(const QString& root, const QString& configDirectory) {
    if (!m_admission() || root.isEmpty()) return fail(ErrorCode::Untrusted, "agent bridge requires consent and a trusted workspace");
    stop();
    m_token.clear();
    for (int i = 0; i < 4; ++i) {
        const auto random = QRandomGenerator::system()->generate();
        m_token += QByteArray::number(random, 16).rightJustified(8, '0');
    }
    if (!m_server.listen(QHostAddress::LocalHost, 0)) return fail(ErrorCode::IoError, "cannot bind agent loopback listener");
    const QString directory = QDir(configDirectory).filePath(QStringLiteral("ide"));
    cleanStaleRecords(directory);
    m_lock = QDir(directory).filePath(QString::number(port()) + QStringLiteral(".lock"));
    const QJsonObject record{{"pid", QCoreApplication::applicationPid()}, {"workspaceFolders", QJsonArray{root}},
        {"ideName", "FrostIDE"}, {"transport", "ws"}, {"authToken", QString::fromLatin1(m_token)},
#ifdef _WIN32
        {"runningInWindows", true}
#else
        {"runningInWindows", false}
#endif
    };
    auto saved = privateRecord(directory, m_lock, QJsonDocument(record).toJson(QJsonDocument::Compact));
    if (!saved) { m_lock.clear(); stop(); return saved; }
    m_expiry.start();
    return {};
}
void AgentBridge::stop() {
    m_expiry.stop();
    m_server.close();
    const auto clients = m_clients;
    for (const auto& [socket, client] : clients) { (void)client; socket->abort(); }
    m_clients.clear();
    if (!m_lock.isEmpty()) QFile::remove(m_lock);
    m_lock.clear();
    m_token.clear();
}
int AgentBridge::clientCount() const {
    int count = 0;
    for (const auto& [socket, client] : m_clients) { (void)socket; if (client.upgraded) ++count; }
    return count;
}
bool AgentBridge::writeBounded(QTcpSocket* socket, const QByteArray& bytes) {
    if (!m_clients.contains(socket) || !m_admission() || socket->bytesToWrite() + bytes.size() > 2 * kMaximum) {
        socket->abort(); return false;
    }
    if (socket->write(bytes) != bytes.size()) { socket->abort(); return false; }
    return true;
}
void AgentBridge::send(QTcpSocket* socket, const QJsonObject& object) {
    if (!m_clients.contains(socket) || !m_admission()) return;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaximum || socket->bytesToWrite() > 2 * kMaximum) { socket->abort(); return; }
    QByteArray frame(1, static_cast<char>(0x81));
    if (bytes.size() < 126) frame += static_cast<char>(bytes.size());
    else if (bytes.size() <= 65535) { frame += char(126); frame += char(bytes.size() >> 8); frame += char(bytes.size()); }
    else { frame += char(127); for (int i = 7; i >= 0; --i) frame += char(static_cast<quint64>(bytes.size()) >> (8 * i)); }
    writeBounded(socket, frame + bytes);
}
void AgentBridge::notify(const QString& method, const QJsonObject& params) {
    if (!m_admission()) { stop(); return; }
    const auto clients = m_clients;
    for (const auto& [socket, client] : clients) if (client.initialized) send(socket, {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}
void AgentBridge::read(QTcpSocket* socket) {
    if (!m_clients.contains(socket)) return;
    if (!m_admission()) { stop(); return; }
    auto& client = m_clients.at(socket);
    client.bytes += socket->readAll();
    client.seen = QDateTime::currentMSecsSinceEpoch();
    if (client.bytes.size() > kMaximum + 16384) { socket->abort(); return; }
    if (!client.upgraded) {
        const auto end = client.bytes.indexOf("\r\n\r\n");
        if (end < 0) { if (client.bytes.size() > 16384) socket->abort(); return; }
        if (end + 4 > 16384) { socket->abort(); return; }
        const auto lines = client.bytes.left(end).split('\n');
        std::map<QByteArray, QByteArray> headers;
        if (lines.empty() || !lines.front().trimmed().startsWith("GET / ") || !lines.front().trimmed().endsWith("HTTP/1.1")) { socket->abort(); return; }
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const auto colon = lines[i].indexOf(':');
            if (colon < 1) { socket->abort(); return; }
            const auto name = lines[i].left(colon).trimmed().toLower();
            if (headers.contains(name)) { socket->abort(); return; }
            headers.emplace(name, lines[i].mid(colon + 1).trimmed());
        }
        const auto key = headers["sec-websocket-key"];
        if (headers.contains("origin") || !equalToken(headers["x-claude-code-ide-authorization"], m_token) ||
            headers["upgrade"].toLower() != "websocket" || !headers["connection"].toLower().replace(" ", "").split(',').contains("upgrade") ||
            headers["sec-websocket-version"] != "13" || !headers["sec-websocket-protocol"].replace(" ", "").split(',').contains("mcp") || QByteArray::fromBase64(key).size() != 16 || QByteArray::fromBase64(key).toBase64() != key) {
            socket->abort(); return;
        }
        const auto accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", QCryptographicHash::Sha1).toBase64();
        if (!writeBounded(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: mcp\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n")) return;
        client.bytes.remove(0, end + 4);
        client.upgraded = true;
    }
    int handled = 0;
    while (client.bytes.size() >= 2) {
        if (++handled > 32) { QPointer<QTcpSocket> guard(socket); QTimer::singleShot(0, this, [this, guard] { if (guard) read(guard); }); return; }
        const auto first = static_cast<unsigned char>(client.bytes[0]);
        const auto second = static_cast<unsigned char>(client.bytes[1]);
        const auto opcode = first & 15;
        const bool final = (first & 0x80) != 0;
        if ((first & 0x70) || !(second & 0x80) || (opcode != 0 && opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10) || (opcode >= 8 && !final)) { socket->abort(); return; }
        quint64 length = second & 127;
        qsizetype offset = 2;
        if (length == 126 || length == 127) {
            const auto count = length == 126 ? 2 : 8;
            if (client.bytes.size() < offset + count) return;
            length = 0;
            for (int i = 0; i < count; ++i) length = (length << 8) | static_cast<unsigned char>(client.bytes[offset++]);
        }
        if (((second & 127) == 126 && length < 126) || ((second & 127) == 127 && length < 65536)) { socket->abort(); return; }
        if (length > kMaximum || (opcode >= 8 && length > 125) || (opcode == 8 && length == 1)) { socket->abort(); return; }
        if (static_cast<quint64>(client.bytes.size()) < static_cast<quint64>(offset + 4) + length) return;
        const auto mask = client.bytes.mid(offset, 4);
        offset += 4;
        auto payload = client.bytes.mid(offset, static_cast<qsizetype>(length));
        for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i % 4]);
        client.bytes.remove(0, offset + static_cast<qsizetype>(length));
        if (opcode == 8) {
            if (payload.size() >= 2) {
                const auto code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
                if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015 ||
                    !io::isValidUtf8(std::string_view(payload.constData() + 2, static_cast<std::size_t>(payload.size() - 2)))) { socket->abort(); return; }
            }
            if (!writeBounded(socket, QByteArray(1, char(0x88)) + QByteArray(1, char(payload.size())) + payload)) return;
            socket->disconnectFromHost(); return;
        }
        if (opcode == 9) { if (!writeBounded(socket, QByteArray(1, char(0x8a)) + QByteArray(1, char(payload.size())) + payload)) return; continue; }
        if (opcode == 10) continue;
        if (opcode == 0) {
            if (!client.fragmenting || client.fragments.size() + payload.size() > kMaximum) { socket->abort(); return; }
            client.fragments += payload;
            if (!final) continue;
            payload = std::move(client.fragments);
            client.fragments.clear();
            client.fragmenting = false;
        } else if (!final) {
            if (client.fragmenting) { socket->abort(); return; }
            client.fragmenting = true;
            client.fragments = std::move(payload);
            continue;
        } else if (client.fragmenting) { socket->abort(); return; }
        message(socket, payload);
        if (!m_clients.contains(socket)) return;
    }
}
void AgentBridge::message(QTcpSocket* socket, const QByteArray& bytes) {
    if (!io::isValidUtf8(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))) { socket->abort(); return; }
    QJsonParseError error;
    const auto parsed = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !parsed.isObject()) { socket->abort(); return; }
    const auto request = parsed.object();
    if (request.value("jsonrpc") != "2.0" || !request.value("method").isString()) { socket->abort(); return; }
    if (!request.contains("id")) return; // Client ide_connected/log_event carry no resource authority.
    const auto id = request.value("id");
    if (!id.isString() && !id.isDouble() && !id.isNull()) { socket->abort(); return; }
    const auto method = request.value("method").toString();
    const auto params = request.value("params").toObject();
    QPointer<QTcpSocket> guard(socket);
    QPointer<AgentBridge> self(this);
    if (m_clients.at(socket).pending >= 32) { socket->abort(); return; }
    ++m_clients.at(socket).pending;
    auto completed = std::make_shared<bool>(false);
    auto deliver = [self, guard, id, completed](QJsonObject result) {
        if (*completed) return;
        *completed = true;
        if (self && guard && self->m_clients.contains(guard.data())) {
            --self->m_clients.at(guard.data()).pending;
            self->send(guard, {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }
    };
    Reply reply{std::move(deliver), [self, guard] {
        return self && guard && self->m_clients.contains(guard.data()) && self->m_admission();
    }};
    if (method == "initialize") {
        const auto version = params.value("protocolVersion").toString();
        if (version != "2025-03-26" && version != "2025-06-18" && version != "2025-11-25" && version != "2026-07-28") {
            --m_clients.at(socket).pending;
            send(socket, {{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", -32602}, {"message", "unsupported protocol version"}}}}); return;
        }
        m_clients.at(socket).initialized = true;
        reply({{"protocolVersion", version}, {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
            {"serverInfo", QJsonObject{{"name", "FrostIDE"}, {"version", "1"}}}});
    } else if (method == "ping") reply({});
    else if (!m_clients.at(socket).initialized) reply({{"isError", true}, {"message", "initialize is required"}});
    else m_dispatch(method, params, std::move(reply));
}
}
