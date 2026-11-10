// SPDX-License-Identifier: Apache-2.0
// Windows WinHTTP implementation of IHttpClient (FND-16, ADR 0085).
#include <frost/io/utf.hpp>
#include <frost/update/http_client.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <winhttp.h>

namespace frost::update {
namespace {

struct WinHttpHandle {
    HINTERNET handle = nullptr;
    explicit WinHttpHandle(HINTERNET h = nullptr) noexcept : handle(h) {}
    ~WinHttpHandle() {
        if (handle)
            WinHttpCloseHandle(handle);
    }
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle)
                WinHttpCloseHandle(handle);
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return handle != nullptr; }
};

std::string_view trimAscii(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

bool isRedirectStatus(int statusCode) noexcept {
    return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308;
}

std::vector<HttpHeader> parseRawHeaders(std::string_view rawHeadersCrlf) {
    std::vector<HttpHeader> result;
    std::size_t lineStart = 0;
    bool firstLine = true;
    while (lineStart < rawHeadersCrlf.size()) {
        const auto lineEnd = rawHeadersCrlf.find("\r\n", lineStart);
        const std::string_view line = (lineEnd == std::string_view::npos)
                                          ? rawHeadersCrlf.substr(lineStart)
                                          : rawHeadersCrlf.substr(lineStart, lineEnd - lineStart);
        lineStart = (lineEnd == std::string_view::npos) ? rawHeadersCrlf.size() : lineEnd + 2;
        if (firstLine) {
            firstLine = false;
            continue;
        }
        if (line.empty())
            continue;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0)
            continue;
        result.push_back(HttpHeader{
            .name = std::string(trimAscii(line.substr(0, colon))),
            .value = std::string(trimAscii(line.substr(colon + 1))),
        });
    }
    return result;
}

class WinHttpClient final : public IHttpClient {
public:
    Result<HttpResponse> request(const HttpRequest& req) override {
        std::vector<std::byte> body;
        auto result =
            execute(req, [&body, maxBytes = req.maxResponseBytes](std::span<const std::byte> chunk) -> Result<void> {
                if (chunk.size() > maxBytes - body.size())
                    return fail(ErrorCode::OutOfRange, "HTTP response exceeded maxResponseBytes");
                body.insert(body.end(), chunk.begin(), chunk.end());
                return {};
            });
        if (!result)
            return std::unexpected(result.error());
        result->body = std::move(body);
        return result;
    }

    Result<HttpResponse> download(const HttpRequest& req, const HttpStreamSink& sink) override {
        if (!sink)
            return fail(ErrorCode::InvalidArgument, "HTTP download sink callback is empty");
        return execute(req, sink);
    }

private:
    Result<HttpResponse> execute(const HttpRequest& req, const HttpStreamSink& sink) {
        if (req.isCancelled && req.isCancelled())
            return fail(ErrorCode::Cancelled, "HTTP request cancelled before start");
        if (req.timeout.count() <= 0)
            return fail(ErrorCode::InvalidArgument, "HTTP timeout must be positive");

        const auto deadline = std::chrono::steady_clock::now() + req.timeout;
        const std::string userAgentUtf8 = req.userAgent.empty() ? "FrostIDE-Updater" : req.userAgent;
        auto userAgentWide = io::toWide(userAgentUtf8);
        if (!userAgentWide)
            return std::unexpected(userAgentWide.error());

        WinHttpHandle session(WinHttpOpen(userAgentWide->c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session)
            return fail(ErrorCode::IoError, "WinHttpOpen failed with error " + std::to_string(GetLastError()));

        std::string currentUrl = req.url;
        HttpMethod currentMethod = req.method;

        for (unsigned hop = 0;; ++hop) {
            if (req.isCancelled && req.isCancelled())
                return fail(ErrorCode::Cancelled, "HTTP request cancelled");

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return fail(ErrorCode::Timeout, "HTTP request timed out");
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int timeoutMs = static_cast<int>(std::clamp<long long>(remainingMs, 100, 600000));

            if (!WinHttpSetTimeouts(session.handle, timeoutMs, timeoutMs, timeoutMs, timeoutMs))
                return fail(ErrorCode::IoError, "WinHttpSetTimeouts failed");

            auto parsed = parseAndValidateHttpUrl(currentUrl, req.allowHttpLoopbackForTests);
            if (!parsed)
                return std::unexpected(parsed.error());

            auto hostWide = io::toWide(parsed->host);
            auto pathWide = io::toWide(parsed->pathAndQuery);
            if (!hostWide || !pathWide)
                return fail(ErrorCode::InvalidArgument, "HTTP URL contains invalid UTF-8");

            WinHttpHandle connection(
                WinHttpConnect(session.handle, hostWide->c_str(), static_cast<INTERNET_PORT>(parsed->port), 0));
            if (!connection)
                return fail(ErrorCode::IoError, "WinHttpConnect failed with error " + std::to_string(GetLastError()));

            const wchar_t* verb = (currentMethod == HttpMethod::Head) ? L"HEAD" : L"GET";
            const DWORD openFlags = parsed->https ? WINHTTP_FLAG_SECURE : 0;
            WinHttpHandle requestHandle(WinHttpOpenRequest(connection.handle, verb, pathWide->c_str(), nullptr,
                                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                           openFlags));
            if (!requestHandle)
                return fail(ErrorCode::IoError,
                            "WinHttpOpenRequest failed with error " + std::to_string(GetLastError()));

            DWORD disableFeature = WINHTTP_DISABLE_REDIRECTS;
            if (!WinHttpSetOption(requestHandle.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disableFeature,
                                  sizeof(disableFeature))) {
                return fail(ErrorCode::IoError, "WinHttpSetOption(WINHTTP_DISABLE_REDIRECTS) failed");
            }

            for (const auto& header : req.headers) {
                if (header.name.empty() || header.name.find_first_of("\r\n:") != std::string::npos ||
                    header.value.find_first_of("\r\n") != std::string::npos) {
                    return fail(ErrorCode::InvalidArgument, "Invalid HTTP request header");
                }
                const std::string line = header.name + ": " + header.value + "\r\n";
                auto wideLine = io::toWide(line);
                if (!wideLine)
                    return std::unexpected(wideLine.error());
                if (!WinHttpAddRequestHeaders(requestHandle.handle, wideLine->c_str(),
                                              static_cast<DWORD>(wideLine->size()),
                                              WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
                    return fail(ErrorCode::IoError, "WinHttpAddRequestHeaders failed");
                }
            }

            if (req.isCancelled && req.isCancelled())
                return fail(ErrorCode::Cancelled, "HTTP request cancelled before send");

            if (!WinHttpSendRequest(requestHandle.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                    0, 0)) {
                const DWORD err = GetLastError();
                if (err == ERROR_WINHTTP_TIMEOUT)
                    return fail(ErrorCode::Timeout, "WinHttpSendRequest timed out");
                return fail(ErrorCode::IoError, "WinHttpSendRequest failed with error " + std::to_string(err));
            }

            if (req.isCancelled && req.isCancelled())
                return fail(ErrorCode::Cancelled, "HTTP request cancelled while awaiting response");

            if (!WinHttpReceiveResponse(requestHandle.handle, nullptr)) {
                const DWORD err = GetLastError();
                if (err == ERROR_WINHTTP_TIMEOUT)
                    return fail(ErrorCode::Timeout, "WinHttpReceiveResponse timed out");
                return fail(ErrorCode::IoError, "WinHttpReceiveResponse failed with error " + std::to_string(err));
            }

            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(requestHandle.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
                return fail(ErrorCode::IoError, "WinHttpQueryHeaders(status) failed");
            }

            std::vector<HttpHeader> responseHeaders;
            DWORD rawHeaderBytes = 0;
            WinHttpQueryHeaders(requestHandle.handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &rawHeaderBytes, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawHeaderBytes > sizeof(wchar_t) &&
                rawHeaderBytes <= 256 * 1024) {
                std::wstring rawWide(rawHeaderBytes / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(requestHandle.handle, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                        WINHTTP_HEADER_NAME_BY_INDEX, rawWide.data(), &rawHeaderBytes,
                                        WINHTTP_NO_HEADER_INDEX)) {
                    if (!rawWide.empty() && rawWide.back() == L'\0')
                        rawWide.pop_back();
                    if (auto rawUtf8 = io::fromWide(rawWide))
                        responseHeaders = parseRawHeaders(*rawUtf8);
                }
            }

            HttpResponse response;
            response.statusCode = static_cast<int>(statusCode);
            response.finalUrl = currentUrl;
            response.headers = std::move(responseHeaders);

            if (isRedirectStatus(response.statusCode)) {
                // Drain at most 64 KiB of redirect body without forwarding to sink.
                constexpr std::size_t kMaxRedirectBodyBytes = 64 * 1024;
                std::size_t redirectBytes = 0;
                std::array<std::byte, 8192> discardBuffer{};
                while (currentMethod != HttpMethod::Head) {
                    if (req.isCancelled && req.isCancelled())
                        return fail(ErrorCode::Cancelled, "HTTP request cancelled during redirect");
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(requestHandle.handle, &available))
                        return fail(ErrorCode::IoError, "WinHttpQueryDataAvailable failed on redirect");
                    if (available == 0)
                        break;
                    const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(available, discardBuffer.size()));
                    DWORD readCount = 0;
                    if (!WinHttpReadData(requestHandle.handle, discardBuffer.data(), toRead, &readCount))
                        return fail(ErrorCode::IoError, "WinHttpReadData failed on redirect");
                    if (readCount == 0)
                        break;
                    if (readCount > kMaxRedirectBodyBytes - redirectBytes)
                        return fail(ErrorCode::OutOfRange, "Redirect response body exceeded 64 KiB limit");
                    redirectBytes += readCount;
                }

                if (hop >= req.maxRedirects)
                    return fail(ErrorCode::OutOfRange, "HTTP redirect limit exceeded");

                const std::string_view location = response.headerValue("Location");
                auto nextUrl = resolveRedirectUrl(currentUrl, location, req.allowHttpLoopbackForTests);
                if (!nextUrl)
                    return std::unexpected(nextUrl.error());
                if (req.redirectValidator && !req.redirectValidator(*nextUrl))
                    return fail(ErrorCode::PermissionDenied, "Redirect target URL rejected by policy");

                currentUrl = std::move(*nextUrl);
                if (response.statusCode == 303 && currentMethod != HttpMethod::Head)
                    currentMethod = HttpMethod::Get;
                continue;
            }

            const std::string_view contentLengthStr = response.headerValue("Content-Length");
            if (!contentLengthStr.empty()) {
                std::uint64_t declaredLength = 0;
                const auto [ptr, ec] = std::from_chars(
                    contentLengthStr.data(), contentLengthStr.data() + contentLengthStr.size(), declaredLength);
                if (ec == std::errc{} && ptr == contentLengthStr.data() + contentLengthStr.size()) {
                    if (declaredLength > req.maxResponseBytes)
                        return fail(ErrorCode::OutOfRange, "HTTP Content-Length exceeds maxResponseBytes");
                }
            }

            if (currentMethod != HttpMethod::Head) {
                std::size_t totalReceived = 0;
                std::array<std::byte, 32768> chunkBuffer{};
                for (;;) {
                    if (req.isCancelled && req.isCancelled())
                        return fail(ErrorCode::Cancelled, "HTTP request cancelled while reading body");
                    if (std::chrono::steady_clock::now() >= deadline)
                        return fail(ErrorCode::Timeout, "HTTP request timed out while reading body");

                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(requestHandle.handle, &available)) {
                        const DWORD err = GetLastError();
                        if (err == ERROR_WINHTTP_TIMEOUT)
                            return fail(ErrorCode::Timeout, "WinHttpQueryDataAvailable timed out");
                        return fail(ErrorCode::IoError,
                                    "WinHttpQueryDataAvailable failed with error " + std::to_string(err));
                    }
                    if (available == 0)
                        break;

                    const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(available, chunkBuffer.size()));
                    DWORD readCount = 0;
                    if (!WinHttpReadData(requestHandle.handle, chunkBuffer.data(), toRead, &readCount)) {
                        const DWORD err = GetLastError();
                        if (err == ERROR_WINHTTP_TIMEOUT)
                            return fail(ErrorCode::Timeout, "WinHttpReadData timed out");
                        return fail(ErrorCode::IoError, "WinHttpReadData failed with error " + std::to_string(err));
                    }
                    if (readCount == 0)
                        break;

                    if (readCount > req.maxResponseBytes - totalReceived)
                        return fail(ErrorCode::OutOfRange, "HTTP response stream exceeded maxResponseBytes");
                    totalReceived += readCount;

                    if (req.isCancelled && req.isCancelled())
                        return fail(ErrorCode::Cancelled, "HTTP request cancelled after reading chunk");

                    if (auto written = sink(std::span<const std::byte>(chunkBuffer.data(), readCount)); !written)
                        return std::unexpected(written.error());
                }
            }

            return response;
        }
    }
};

} // namespace

std::unique_ptr<IHttpClient> createNativeHttpClient() { return std::make_unique<WinHttpClient>(); }

} // namespace frost::update
