// SPDX-License-Identifier: Apache-2.0
// Qt-free HTTP/HTTPS client interface for bounded requests and streaming downloads (FND-16).
#pragma once

#include <frost/core/error.hpp>
#include <frost/io/file_system.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::update {

enum class HttpMethod : std::uint8_t {
    Get,
    Head,
};

struct HttpHeader {
    std::string name;
    std::string value;

    friend bool operator==(const HttpHeader&, const HttpHeader&) = default;
};

struct HttpRequest {
    std::string url{};
    HttpMethod method = HttpMethod::Get;
    std::vector<HttpHeader> headers{};
    std::string userAgent = "FrostIDE-Updater";
    std::chrono::milliseconds timeout{30000};
    std::size_t maxResponseBytes = 1024 * 1024;
    unsigned maxRedirects = 5;
    // Production requests require https://. Unit tests against a local 127.0.0.1 / localhost
    // server may set allowHttpLoopbackForTests = true to permit http:// on loopback only.
    bool allowHttpLoopbackForTests = false;
    // Optional cancellation predicate; when it returns true the request aborts with Cancelled.
    std::function<bool()> isCancelled{};
    // Optional redirect target validator; when set and returning false, the redirect is refused
    // with PermissionDenied before any connection to the redirect target is made.
    std::function<bool(std::string_view targetUrl)> redirectValidator{};
};

struct HttpResponse {
    int statusCode = 0;
    std::string finalUrl{};
    std::vector<std::byte> body{};
    std::vector<HttpHeader> headers{};

    [[nodiscard]] std::string_view bodyText() const noexcept {
        return std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
    }

    [[nodiscard]] std::string_view headerValue(std::string_view name) const noexcept;
};

// Called sequentially for each received body chunk during a streaming download.
// Returning an error aborts the transfer immediately and propagates that error.
using HttpStreamSink = std::function<Result<void>(std::span<const std::byte> chunk)>;

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    IHttpClient(const IHttpClient&) = delete;
    IHttpClient& operator=(const IHttpClient&) = delete;

    // Executes a bounded GET or HEAD request and buffers the body up to req.maxResponseBytes.
    [[nodiscard]] virtual Result<HttpResponse> request(const HttpRequest& req) = 0;

    // Executes a bounded streaming GET request, invoking `sink` for each body chunk without
    // retaining the full body in memory. Redirect body bytes are never passed to `sink`.
    [[nodiscard]] virtual Result<HttpResponse> download(const HttpRequest& req, const HttpStreamSink& sink) = 0;

    // Streams the response body and writes it atomically to `destinationPath` via `fs`.
    [[nodiscard]] Result<HttpResponse> downloadToFile(const HttpRequest& req, io::IFileSystem& fs,
                                                      std::string_view destinationPath);

protected:
    IHttpClient() = default;
};

// Parsed components of an HTTP/HTTPS URL, validated against updater URL rules.
struct ParsedHttpUrl {
    std::string scheme{};
    std::string host{};
    std::uint16_t port = 0;
    std::string pathAndQuery{};
    bool https = false;
};

// Parses and validates `url`. Rejects credentials, fragments, control characters, and non-HTTPS
// schemes unless `allowHttpLoopbackForTests` is true and the host is 127.0.0.1 or localhost.
[[nodiscard]] Result<ParsedHttpUrl> parseAndValidateHttpUrl(std::string_view url, bool allowHttpLoopbackForTests);

// Resolves a `location` header value (absolute or relative) against `baseUrl`.
[[nodiscard]] Result<std::string> resolveRedirectUrl(std::string_view baseUrl, std::string_view location,
                                                     bool allowHttpLoopbackForTests);

// Creates the platform-native HTTP transport (WinHTTP on Windows, libcurl on Linux).
[[nodiscard]] std::unique_ptr<IHttpClient> createNativeHttpClient();

} // namespace frost::update
