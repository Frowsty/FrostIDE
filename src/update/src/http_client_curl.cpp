// SPDX-License-Identifier: Apache-2.0
// Linux libcurl implementation of IHttpClient (FND-16, ADR 0085).
#include <curl/curl.h>
#include <frost/update/http_client.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::update {
namespace {

void ensureCurlInitialized() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlEasyHandle {
    CURL* handle = nullptr;
    explicit CurlEasyHandle(CURL* h = nullptr) noexcept : handle(h) {}
    ~CurlEasyHandle() {
        if (handle)
            curl_easy_cleanup(handle);
    }
    CurlEasyHandle(const CurlEasyHandle&) = delete;
    CurlEasyHandle& operator=(const CurlEasyHandle&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return handle != nullptr; }
};

struct CurlSlist {
    curl_slist* list = nullptr;
    ~CurlSlist() {
        if (list)
            curl_slist_free_all(list);
    }
    void append(const char* str) { list = curl_slist_append(list, str); }
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

struct TransferContext {
    const HttpRequest* request = nullptr;
    const HttpStreamSink* sink = nullptr;
    std::vector<HttpHeader> headers{};
    std::size_t receivedBytes = 0;
    std::size_t redirectBytes = 0;
    int lastStatusCode = 0;
    std::optional<Error> error{};
};

size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    const size_t total = size * nitems;
    auto* ctx = static_cast<TransferContext*>(userdata);
    if (ctx->request->isCancelled && ctx->request->isCancelled()) {
        ctx->error = Error(ErrorCode::Cancelled, "HTTP request cancelled");
        return 0;
    }
    const std::string_view line(buffer, total);
    if (line.starts_with("HTTP/")) {
        ctx->headers.clear();
        const auto firstSpace = line.find(' ');
        if (firstSpace != std::string_view::npos) {
            const auto rest = trimAscii(line.substr(firstSpace + 1));
            int code = 0;
            std::from_chars(rest.data(), rest.data() + std::min<std::size_t>(rest.size(), 3), code);
            ctx->lastStatusCode = code;
        }
        return total;
    }
    const auto colon = line.find(':');
    if (colon != std::string_view::npos && colon > 0) {
        ctx->headers.push_back(HttpHeader{
            .name = std::string(trimAscii(line.substr(0, colon))),
            .value = std::string(trimAscii(line.substr(colon + 1))),
        });
    }
    return total;
}

size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* ctx = static_cast<TransferContext*>(userdata);
    if (ctx->request->isCancelled && ctx->request->isCancelled()) {
        ctx->error = Error(ErrorCode::Cancelled, "HTTP request cancelled while reading body");
        return 0;
    }
    if (total == 0)
        return 0;

    if (isRedirectStatus(ctx->lastStatusCode)) {
        constexpr std::size_t kMaxRedirectBody = 64 * 1024;
        if (total > kMaxRedirectBody - ctx->redirectBytes) {
            ctx->error = Error(ErrorCode::OutOfRange, "Redirect response body exceeded 64 KiB limit");
            return 0;
        }
        ctx->redirectBytes += total;
        return total;
    }

    if (total > ctx->request->maxResponseBytes - ctx->receivedBytes) {
        ctx->error = Error(ErrorCode::OutOfRange, "HTTP response stream exceeded maxResponseBytes");
        return 0;
    }
    ctx->receivedBytes += total;

    const std::span<const std::byte> chunk(reinterpret_cast<const std::byte*>(ptr), total);
    if (auto written = (*ctx->sink)(chunk); !written) {
        ctx->error = written.error();
        return 0;
    }
    return total;
}

int xferInfoCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                     curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<TransferContext*>(clientp);
    if (ctx->request->isCancelled && ctx->request->isCancelled()) {
        ctx->error = Error(ErrorCode::Cancelled, "HTTP request cancelled");
        return 1;
    }
    return 0;
}

class CurlHttpClient final : public IHttpClient {
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
        ensureCurlInitialized();
        if (req.isCancelled && req.isCancelled())
            return fail(ErrorCode::Cancelled, "HTTP request cancelled before start");
        if (req.timeout.count() <= 0)
            return fail(ErrorCode::InvalidArgument, "HTTP timeout must be positive");

        const auto deadline = std::chrono::steady_clock::now() + req.timeout;
        std::string currentUrl = req.url;
        HttpMethod currentMethod = req.method;

        for (unsigned hop = 0;; ++hop) {
            if (req.isCancelled && req.isCancelled())
                return fail(ErrorCode::Cancelled, "HTTP request cancelled");

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return fail(ErrorCode::Timeout, "HTTP request timed out");
            const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const long timeoutMs = static_cast<long>(std::clamp<long long>(remainingMs, 1, 600000));

            auto parsed = parseAndValidateHttpUrl(currentUrl, req.allowHttpLoopbackForTests);
            if (!parsed)
                return std::unexpected(parsed.error());

            CurlEasyHandle curl(curl_easy_init());
            if (!curl)
                return fail(ErrorCode::Internal, "curl_easy_init failed");

            TransferContext ctx{.request = &req, .sink = &sink};

            curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl.handle, CURLOPT_URL, currentUrl.c_str());
            curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl.handle, CURLOPT_PROTOCOLS_STR,
                             req.allowHttpLoopbackForTests ? "http,https" : "https");
            curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS, timeoutMs);
            curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
            const std::string ua = req.userAgent.empty() ? "FrostIDE-Updater" : req.userAgent;
            curl_easy_setopt(curl.handle, CURLOPT_USERAGENT, ua.c_str());

            if (currentMethod == HttpMethod::Head)
                curl_easy_setopt(curl.handle, CURLOPT_NOBODY, 1L);
            else
                curl_easy_setopt(curl.handle, CURLOPT_HTTPGET, 1L);

            CurlSlist headerList;
            for (const auto& header : req.headers) {
                if (header.name.empty() || header.name.find_first_of("\r\n:") != std::string::npos ||
                    header.value.find_first_of("\r\n") != std::string::npos) {
                    return fail(ErrorCode::InvalidArgument, "Invalid HTTP request header");
                }
                const std::string formatted = header.name + ": " + header.value;
                headerList.append(formatted.c_str());
            }
            if (headerList.list)
                curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headerList.list);

            curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, &headerCallback);
            curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &ctx);
            curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, &writeCallback);
            curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, &xferInfoCallback);
            curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, &ctx);

            const CURLcode code = curl_easy_perform(curl.handle);
            if (ctx.error)
                return std::unexpected(*ctx.error);
            if (code == CURLE_OPERATION_TIMEDOUT)
                return fail(ErrorCode::Timeout, "libcurl request timed out");
            if (code != CURLE_OK)
                return fail(ErrorCode::IoError, std::string("libcurl error: ") + curl_easy_strerror(code));

            long responseCode = 0;
            curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &responseCode);

            HttpResponse response;
            response.statusCode = static_cast<int>(responseCode);
            response.finalUrl = currentUrl;
            response.headers = std::move(ctx.headers);

            if (isRedirectStatus(response.statusCode)) {
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

            return response;
        }
    }
};

} // namespace

std::unique_ptr<IHttpClient> createNativeHttpClient() { return std::make_unique<CurlHttpClient>(); }

} // namespace frost::update
