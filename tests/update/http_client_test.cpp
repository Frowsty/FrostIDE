// SPDX-License-Identifier: Apache-2.0
// Unit and loopback integration tests for IHttpClient, NetworkPolicy, and ConsentGate (FND-16).

#include <frost/io/file_system.hpp>
#include <frost/update/http_client.hpp>
#include <frost/update/network_policy.hpp>
#include <frost/update/verification.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sodium.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <unistd.h>
#endif

namespace frost::update {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) {
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) {
    if (s >= 0) {
        ::close(s);
    }
}
#endif

struct CapturedHttpRequest {
    std::string method;
    std::string target;
    std::string rawHeaders;
};

struct CannedHttpResponse {
    int statusCode = 200;
    std::string reason = "OK";
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::milliseconds chunkDelay{0};
    std::size_t chunkSize = 0;
};

class LoopbackHttpServer {
public:
    using Handler = std::function<CannedHttpResponse(const CapturedHttpRequest&)>;

    explicit LoopbackHttpServer(Handler handler) : m_handler(std::move(handler)) {
#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        m_listenSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        EXPECT_NE(m_listenSocket, kInvalidSocket);

        int opt = 1;
#ifdef _WIN32
        ::setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        ::setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(m_listenSocket, 16), 0);

#ifdef _WIN32
        int addrLen = sizeof(addr);
#else
        socklen_t addrLen = sizeof(addr);
#endif
        EXPECT_EQ(::getsockname(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen), 0);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread([this] { serveLoop(); });
    }

    ~LoopbackHttpServer() {
        m_stop.store(true, std::memory_order_release);
        if (m_port != 0) {
            SocketHandle wake = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (wake != kInvalidSocket) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(m_port);
                (void)::connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                closeSocket(wake);
            }
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        closeSocket(m_listenSocket);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }

    [[nodiscard]] std::string url(std::string_view path) const {
        std::string result = "http://127.0.0.1:" + std::to_string(m_port);
        if (path.empty() || path.front() != '/') {
            result.push_back('/');
        }
        result.append(path);
        return result;
    }

    [[nodiscard]] std::uint64_t acceptedConnections() const noexcept {
        return m_acceptedConnections.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<CapturedHttpRequest> capturedRequests() const {
        std::scoped_lock lock(m_mutex);
        return m_requests;
    }

private:
    void serveLoop() {
        while (!m_stop.load(std::memory_order_acquire)) {
            sockaddr_in clientAddr{};
#ifdef _WIN32
            int clientLen = sizeof(clientAddr);
#else
            socklen_t clientLen = sizeof(clientAddr);
#endif
            SocketHandle client = ::accept(m_listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (client == kInvalidSocket) {
                break;
            }
            if (m_stop.load(std::memory_order_acquire)) {
                closeSocket(client);
                break;
            }
            m_acceptedConnections.fetch_add(1, std::memory_order_acq_rel);
            handleClient(client);
            closeSocket(client);
        }
    }

    void handleClient(SocketHandle client) {
        std::string raw;
        char buf[4096]{};
        while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 65536) {
#ifdef _WIN32
            const int n = ::recv(client, buf, sizeof(buf), 0);
#else
            const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                return;
            }
            raw.append(buf, static_cast<std::size_t>(n));
        }

        CapturedHttpRequest captured;
        const auto lineEnd = raw.find("\r\n");
        if (lineEnd != std::string::npos) {
            std::istringstream iss(raw.substr(0, lineEnd));
            iss >> captured.method >> captured.target;
            captured.rawHeaders = raw.substr(lineEnd + 2);
        }
        {
            std::scoped_lock lock(m_mutex);
            m_requests.push_back(captured);
        }

        const CannedHttpResponse resp = m_handler(captured);
        std::ostringstream head;
        head << "HTTP/1.1 " << resp.statusCode << " " << resp.reason << "\r\n";
        bool hasContentLength = false;
        for (const auto& h : resp.headers) {
            head << h.name << ": " << h.value << "\r\n";
            if (h.name == "Content-Length") {
                hasContentLength = true;
            }
        }
        if (!hasContentLength) {
            head << "Content-Length: " << resp.body.size() << "\r\n";
        }
        head << "Connection: close\r\n\r\n";
        const std::string headerBytes = head.str();
        sendAll(client, headerBytes.data(), headerBytes.size());

        if (captured.method == "HEAD" || resp.body.empty()) {
            return;
        }
        if (resp.chunkSize > 0) {
            std::size_t offset = 0;
            while (offset < resp.body.size()) {
                const std::size_t n = std::min(resp.chunkSize, resp.body.size() - offset);
                if (!sendAll(client, resp.body.data() + offset, n)) {
                    break;
                }
                offset += n;
                if (resp.chunkDelay.count() > 0) {
                    std::this_thread::sleep_for(resp.chunkDelay);
                }
            }
        } else {
            sendAll(client, resp.body.data(), resp.body.size());
        }
    }

    static bool sendAll(SocketHandle s, const char* data, std::size_t len) {
        std::size_t sent = 0;
        while (sent < len) {
#ifdef _WIN32
            const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
#elif defined(MSG_NOSIGNAL)
            const ssize_t n = ::send(s, data + sent, len - sent, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(s, data + sent, len - sent, 0);
#endif
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    Handler m_handler;
    SocketHandle m_listenSocket = kInvalidSocket;
    std::uint16_t m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_acceptedConnections{0};
    mutable std::mutex m_mutex;
    std::vector<CapturedHttpRequest> m_requests;
    std::thread m_thread;
};

TEST(HttpClientTest, NoNetworkWhenOfflineByDefault) {
    LoopbackHttpServer server(
        [](const CapturedHttpRequest&) { return CannedHttpResponse{200, "OK", {}, "should-never-be-reached"}; });

    auto policy = std::make_shared<NetworkPolicy>();
    EXPECT_TRUE(policy->isOffline());
    EXPECT_FALSE(policy->hasConsent());

    bool promptCalled = false;
    policy->setConsentPrompt([&](const ConsentRequest&) {
        promptCalled = true;
        return ConsentDecision::Granted;
    });

    PolicyGatedHttpClient client(createNativeHttpClient(), policy, "update_check");

    HttpRequest req;
    req.url = server.url("/releases");
    req.allowHttpLoopbackForTests = true;

    const auto res = client.request(req);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(promptCalled);
    EXPECT_EQ(client.transportRequestCount(), 0u);
    EXPECT_EQ(server.acceptedConnections(), 0u);

    std::size_t streamedBytes = 0;
    const auto dlRes = client.download(req, [&](std::span<const std::byte> chunk) -> Result<void> {
        streamedBytes += chunk.size();
        return {};
    });
    ASSERT_FALSE(dlRes.has_value());
    EXPECT_EQ(dlRes.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(streamedBytes, 0u);
    EXPECT_EQ(client.transportRequestCount(), 0u);
    EXPECT_EQ(server.acceptedConnections(), 0u);
}

TEST(HttpClientTest, ConsentPromptGatesFirstRequestAndDenialBlocksSocketTraffic) {
    LoopbackHttpServer server([](const CapturedHttpRequest&) {
        return CannedHttpResponse{200, "OK", {{"Content-Type", "application/json"}}, R"({"status":"ok"})"};
    });

    auto policy = std::make_shared<NetworkPolicy>(false); // Online mode enabled, but no consent yet.
    EXPECT_FALSE(policy->isOffline());
    EXPECT_FALSE(policy->hasConsent());

    PolicyGatedHttpClient client(createNativeHttpClient(), policy, "check_github_releases");

    HttpRequest req;
    req.url = server.url("/api/releases");
    req.allowHttpLoopbackForTests = true;

    // 1. Without a consent callback registered, online mode still denies outbound traffic.
    {
        const auto res = client.request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
        EXPECT_EQ(client.transportRequestCount(), 0u);
        EXPECT_EQ(server.acceptedConnections(), 0u);
    }

    // 2. Register a consent callback that denies the first request.
    unsigned promptCalls = 0;
    ConsentRequest lastConsentReq;
    ConsentDecision nextDecision = ConsentDecision::Denied;
    policy->setConsentPrompt([&](const ConsentRequest& cReq) {
        ++promptCalls;
        lastConsentReq = cReq;
        return nextDecision;
    });

    {
        const auto res = client.request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::Cancelled);
        EXPECT_EQ(promptCalls, 1u);
        EXPECT_EQ(lastConsentReq.host, "127.0.0.1");
        EXPECT_EQ(lastConsentReq.purpose, "check_github_releases");
        EXPECT_EQ(client.transportRequestCount(), 0u);
        EXPECT_EQ(server.acceptedConnections(), 0u);
    }

    // 3. Grant session consent on the next prompt; verify the request succeeds and subsequent
    // requests in the same session do not re-prompt.
    nextDecision = ConsentDecision::Granted;
    {
        const auto res1 = client.request(req);
        ASSERT_TRUE(res1.has_value()) << res1.error().message();
        EXPECT_EQ(res1->statusCode, 200);
        EXPECT_EQ(res1->bodyText(), R"({"status":"ok"})");
        EXPECT_EQ(promptCalls, 2u);
        EXPECT_TRUE(policy->hasConsent());
        EXPECT_EQ(client.transportRequestCount(), 1u);
        EXPECT_EQ(server.acceptedConnections(), 1u);

        const auto res2 = client.request(req);
        ASSERT_TRUE(res2.has_value()) << res2.error().message();
        EXPECT_EQ(res2->statusCode, 200);
        EXPECT_EQ(promptCalls, 2u); // Not prompted again!
        EXPECT_EQ(client.transportRequestCount(), 2u);
        EXPECT_EQ(server.acceptedConnections(), 2u);
    }

    // 4. Switching back to offline revokes session consent and blocks further requests.
    policy->setOffline(true);
    EXPECT_TRUE(policy->isOffline());
    EXPECT_FALSE(policy->hasConsent());
    {
        const auto res3 = client.request(req);
        ASSERT_FALSE(res3.has_value());
        EXPECT_EQ(res3.error().code(), ErrorCode::PermissionDenied);
        EXPECT_EQ(client.transportRequestCount(), 2u);
        EXPECT_EQ(server.acceptedConnections(), 2u);
    }
}

TEST(HttpClientTest, RevokeConsentAndLiveOfflineReaderEnforcePolicy) {
    LoopbackHttpServer server([](const CapturedHttpRequest&) { return CannedHttpResponse{200, "OK", {}, "pong"}; });

    auto policy = std::make_shared<NetworkPolicy>(false);
    unsigned promptCalls = 0;
    policy->setConsentPrompt([&](const ConsentRequest&) {
        ++promptCalls;
        return ConsentDecision::Granted;
    });

    PolicyGatedHttpClient client(createNativeHttpClient(), policy, "ping");
    HttpRequest req;
    req.url = server.url("/ping");
    req.allowHttpLoopbackForTests = true;

    const auto r1 = client.request(req);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();
    EXPECT_TRUE(policy->hasConsent());
    EXPECT_EQ(promptCalls, 1u);

    // Revoking consent explicitly requires a fresh prompt on the next request.
    policy->revokeConsent();
    EXPECT_FALSE(policy->hasConsent());

    const auto r2 = client.request(req);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
    EXPECT_EQ(promptCalls, 2u);
    EXPECT_EQ(client.transportRequestCount(), 2u);

    // Live offline reader overrides static online state.
    bool liveOffline = true;
    policy->setOfflineReader([&]() { return liveOffline; });
    EXPECT_TRUE(policy->isOffline());
    const auto r3 = client.request(req);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(client.transportRequestCount(), 2u);
}

TEST(HttpClientTest, UrlValidationRejectsPlainHttpByDefaultAndMalformedUrls) {
    auto client = createNativeHttpClient();

    // Plain HTTP is rejected when allowHttpLoopbackForTests is false.
    {
        HttpRequest req;
        req.url = "http://127.0.0.1:8080/test";
        req.allowHttpLoopbackForTests = false;
        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    }

    // Plain HTTP to a non-loopback host is rejected even when allowHttpLoopbackForTests is true.
    {
        HttpRequest req;
        req.url = "http://example.com/test";
        req.allowHttpLoopbackForTests = true;
        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    }

    // URL with embedded credentials or fragment is rejected.
    {
        HttpRequest req;
        req.url = "https://user:pass@api.github.com/repos";
        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    }
    {
        HttpRequest req;
        req.url = "https://api.github.com/repos#section";
        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    }
}

TEST(HttpClientTest, NativeHttpClientSupportsHeadCustomHeadersAndRedirects) {
    LoopbackHttpServer server([](const CapturedHttpRequest& req) -> CannedHttpResponse {
        if (req.target == "/redirect-start") {
            return CannedHttpResponse{302, "Found", {{"Location", "/redirect-mid"}}, ""};
        }
        if (req.target == "/redirect-mid") {
            return CannedHttpResponse{301, "Moved Permanently", {{"Location", "/final-dest"}}, ""};
        }
        if (req.target == "/final-dest") {
            return CannedHttpResponse{
                200, "OK", {{"X-Frost-Server", "loopback-1"}}, req.method == "HEAD" ? "" : "final-payload"};
        }
        return CannedHttpResponse{404, "Not Found", {}, "missing"};
    });

    auto client = createNativeHttpClient();

    // 1. Follow relative redirects to /final-dest.
    {
        HttpRequest req;
        req.method = HttpMethod::Get;
        req.url = server.url("/redirect-start");
        req.userAgent = "FrostIDE-TestAgent/1.0";
        req.headers = {{"X-Custom-Header", "frost-test-value"}};
        req.allowHttpLoopbackForTests = true;

        const auto res = client->request(req);
        ASSERT_TRUE(res.has_value()) << res.error().message();
        EXPECT_EQ(res->statusCode, 200);
        EXPECT_EQ(res->finalUrl, server.url("/final-dest"));
        EXPECT_EQ(res->bodyText(), "final-payload");
        EXPECT_EQ(res->headerValue("x-frost-server"), "loopback-1");

        const auto captured = server.capturedRequests();
        ASSERT_GE(captured.size(), 3u);
        EXPECT_NE(captured[0].rawHeaders.find("FrostIDE-TestAgent/1.0"), std::string::npos);
        EXPECT_NE(captured[0].rawHeaders.find("frost-test-value"), std::string::npos);
    }

    // 2. HEAD request returns headers and 200 status with empty body.
    {
        HttpRequest req;
        req.method = HttpMethod::Head;
        req.url = server.url("/final-dest");
        req.allowHttpLoopbackForTests = true;

        const auto res = client->request(req);
        ASSERT_TRUE(res.has_value()) << res.error().message();
        EXPECT_EQ(res->statusCode, 200);
        EXPECT_TRUE(res->body.empty());
    }

    // 3. Redirect limit exceeded is rejected with OutOfRange.
    {
        HttpRequest req;
        req.method = HttpMethod::Get;
        req.url = server.url("/redirect-start");
        req.maxRedirects = 1; // Needs 2 hops
        req.allowHttpLoopbackForTests = true;

        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
    }

    // 4. Redirect validator rejecting a target stops the redirect chain.
    {
        HttpRequest req;
        req.method = HttpMethod::Get;
        req.url = server.url("/redirect-start");
        req.redirectValidator = [](std::string_view targetUrl) {
            return targetUrl.find("/final-dest") == std::string_view::npos;
        };
        req.allowHttpLoopbackForTests = true;

        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::PermissionDenied);
    }
}

TEST(HttpClientTest, EnforcesMaxResponseSizeStreamingDownloadAndCancellation) {
    ASSERT_GE(sodium_init(), 0);

    std::string largePayload(128 * 1024, '\0');
    for (std::size_t i = 0; i < largePayload.size(); ++i) {
        largePayload[i] = static_cast<char>('A' + (i % 26));
    }

    LoopbackHttpServer server([&](const CapturedHttpRequest& req) -> CannedHttpResponse {
        if (req.target == "/slow") {
            return CannedHttpResponse{200, "OK", {}, largePayload, std::chrono::milliseconds{15}, 4096};
        }
        return CannedHttpResponse{200, "OK", {}, largePayload, std::chrono::milliseconds{0}, 16384};
    });

    auto client = createNativeHttpClient();

    // 1. Oversized response is rejected with OutOfRange.
    {
        HttpRequest req;
        req.url = server.url("/payload.bin");
        req.maxResponseBytes = 32 * 1024; // 32 KiB limit < 128 KiB payload
        req.allowHttpLoopbackForTests = true;

        const auto res = client->request(req);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
    }

    // 2. Streaming downloadToFile writes the exact payload and passes SHA-256 verification.
    {
        const auto tempBase = std::filesystem::temp_directory_path() / "frost_http_client_test";
        std::filesystem::remove_all(tempBase);
        std::filesystem::create_directories(tempBase);
        const auto destFile = (tempBase / "downloaded_payload.bin").string();

        HttpRequest req;
        req.url = server.url("/payload.bin");
        req.maxResponseBytes = 256 * 1024;
        req.allowHttpLoopbackForTests = true;

        auto& fs = io::realFileSystem();
        const auto dlRes = client->downloadToFile(req, fs, destFile);
        ASSERT_TRUE(dlRes.has_value()) << dlRes.error().message();
        EXPECT_EQ(dlRes->statusCode, 200);

        const auto readBack = fs.readFile(destFile);
        ASSERT_TRUE(readBack.has_value()) << readBack.error().message();
        ASSERT_EQ(readBack->size(), largePayload.size());

        Sha256 expectedDigest{};
        crypto_hash_sha256(expectedDigest.data(), reinterpret_cast<const unsigned char*>(largePayload.data()),
                           largePayload.size());
        const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(readBack->data()), readBack->size());
        const auto verified = verifyPayload(span, largePayload.size(), expectedDigest);
        EXPECT_TRUE(verified.has_value()) << verified.error().message();

        std::filesystem::remove_all(tempBase);
    }

    // 3. Cancellation during streaming aborts the transfer with ErrorCode::Cancelled.
    {
        std::atomic<bool> cancelFlag{false};
        HttpRequest req;
        req.url = server.url("/slow");
        req.maxResponseBytes = 256 * 1024;
        req.allowHttpLoopbackForTests = true;
        req.isCancelled = [&]() { return cancelFlag.load(std::memory_order_relaxed); };

        std::size_t receivedBeforeCancel = 0;
        const auto res = client->download(req, [&](std::span<const std::byte> chunk) -> Result<void> {
            receivedBeforeCancel += chunk.size();
            if (receivedBeforeCancel >= 8192) {
                cancelFlag.store(true, std::memory_order_relaxed);
            }
            return {};
        });
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().code(), ErrorCode::Cancelled);
        EXPECT_LT(receivedBeforeCancel, largePayload.size());
    }
}

} // namespace
} // namespace frost::update
