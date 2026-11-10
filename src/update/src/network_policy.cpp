// SPDX-License-Identifier: Apache-2.0
// URL validation, offline-by-default NetworkPolicy, and PolicyGatedHttpClient (FND-16).
#include <frost/update/network_policy.hpp>

#include <algorithm>
#include <charconv>

namespace frost::update {
namespace {

constexpr char toLowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i]))
            return false;
    }
    return true;
}

std::string_view trimAsciiWhitespace(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

bool isLoopbackHost(std::string_view host) noexcept {
    return equalsIgnoreCaseAscii(host, "127.0.0.1") || equalsIgnoreCaseAscii(host, "localhost") || host == "[::1]" ||
           host == "::1";
}

} // namespace

std::string_view HttpResponse::headerValue(std::string_view name) const noexcept {
    for (const auto& header : headers) {
        if (equalsIgnoreCaseAscii(header.name, name))
            return header.value;
    }
    return {};
}

Result<HttpResponse> IHttpClient::downloadToFile(const HttpRequest& req, io::IFileSystem& fs,
                                                 std::string_view destinationPath) {
    if (destinationPath.empty())
        return fail(ErrorCode::InvalidArgument, "Download destination path is empty");
    std::string buffer;
    auto response =
        download(req, [&buffer, maxBytes = req.maxResponseBytes](std::span<const std::byte> chunk) -> Result<void> {
            if (chunk.size() > maxBytes - buffer.size())
                return fail(ErrorCode::OutOfRange, "Downloaded payload exceeds maximum response size");
            buffer.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return {};
        });
    if (!response)
        return std::unexpected(response.error());
    if (response->statusCode != 200)
        return fail(ErrorCode::IoError,
                    "HTTP download returned non-200 status " + std::to_string(response->statusCode));
    if (auto written = fs.writeFileAtomic(destinationPath, buffer); !written)
        return std::unexpected(written.error());
    return response;
}

Result<ParsedHttpUrl> parseAndValidateHttpUrl(std::string_view url, bool allowHttpLoopbackForTests) {
    if (url.empty() || url.size() > 4096)
        return fail(ErrorCode::InvalidArgument, "HTTP URL length is invalid");
    for (char ch : url) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20U || byte == 0x7FU)
            return fail(ErrorCode::InvalidArgument, "HTTP URL contains control characters or spaces");
        if (ch == '#')
            return fail(ErrorCode::PermissionDenied, "HTTP URL fragments are not permitted");
    }

    const auto schemePos = url.find("://");
    if (schemePos == std::string_view::npos || schemePos == 0)
        return fail(ErrorCode::InvalidArgument, "HTTP URL is missing a scheme");

    std::string scheme(url.substr(0, schemePos));
    std::ranges::transform(scheme, scheme.begin(), toLowerAscii);
    if (scheme != "https" && scheme != "http")
        return fail(ErrorCode::PermissionDenied, "Unsupported URL scheme: " + scheme);

    const std::string_view rest = url.substr(schemePos + 3);
    if (rest.empty())
        return fail(ErrorCode::InvalidArgument, "HTTP URL is missing a host");

    const auto pathStart = rest.find_first_of("/?");
    const std::string_view authority = (pathStart == std::string_view::npos) ? rest : rest.substr(0, pathStart);
    if (authority.empty())
        return fail(ErrorCode::InvalidArgument, "HTTP URL authority is empty");
    if (authority.find('@') != std::string_view::npos)
        return fail(ErrorCode::PermissionDenied, "HTTP URL userinfo credentials are not permitted");

    std::string_view hostPart = authority;
    std::string_view portPart{};
    if (authority.starts_with('[')) {
        const auto closeBracket = authority.find(']');
        if (closeBracket == std::string_view::npos)
            return fail(ErrorCode::InvalidArgument, "Malformed IPv6 host in HTTP URL");
        hostPart = authority.substr(1, closeBracket - 1);
        if (closeBracket + 1 < authority.size()) {
            if (authority[closeBracket + 1] != ':')
                return fail(ErrorCode::InvalidArgument, "Malformed authority after IPv6 host");
            portPart = authority.substr(closeBracket + 2);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            hostPart = authority.substr(0, colon);
            portPart = authority.substr(colon + 1);
        }
    }

    if (hostPart.empty())
        return fail(ErrorCode::InvalidArgument, "HTTP URL host is empty");

    std::string host(hostPart);
    std::ranges::transform(host, host.begin(), toLowerAscii);

    const bool https = (scheme == "https");
    if (!https) {
        if (!allowHttpLoopbackForTests || !isLoopbackHost(host))
            return fail(ErrorCode::PermissionDenied, "Plain HTTP is forbidden outside loopback tests");
    }

    std::uint16_t port = https ? 443 : 80;
    if (!portPart.empty()) {
        std::uint32_t parsedPort = 0;
        const auto [ptr, ec] = std::from_chars(portPart.data(), portPart.data() + portPart.size(), parsedPort);
        if (ec != std::errc{} || ptr != portPart.data() + portPart.size() || parsedPort == 0 || parsedPort > 65535)
            return fail(ErrorCode::InvalidArgument, "Invalid TCP port in HTTP URL");
        port = static_cast<std::uint16_t>(parsedPort);
    }

    std::string pathAndQuery = "/";
    if (pathStart != std::string_view::npos) {
        if (rest[pathStart] == '?')
            pathAndQuery = "/" + std::string(rest.substr(pathStart));
        else
            pathAndQuery = std::string(rest.substr(pathStart));
    }

    return ParsedHttpUrl{
        .scheme = std::move(scheme),
        .host = std::move(host),
        .port = port,
        .pathAndQuery = std::move(pathAndQuery),
        .https = https,
    };
}

Result<std::string> resolveRedirectUrl(std::string_view baseUrl, std::string_view location,
                                       bool allowHttpLoopbackForTests) {
    const std::string_view trimmed = trimAsciiWhitespace(location);
    if (trimmed.empty())
        return fail(ErrorCode::Corrupt, "Redirect Location header is empty");
    if (trimmed.starts_with("//"))
        return fail(ErrorCode::PermissionDenied, "Protocol-relative redirects are not permitted");

    if (trimmed.find("://") != std::string_view::npos) {
        auto validated = parseAndValidateHttpUrl(trimmed, allowHttpLoopbackForTests);
        if (!validated)
            return std::unexpected(validated.error());
        return std::string(trimmed);
    }

    auto base = parseAndValidateHttpUrl(baseUrl, allowHttpLoopbackForTests);
    if (!base)
        return std::unexpected(base.error());

    std::string resolved = base->scheme + "://" + base->host;
    const std::uint16_t defaultPort = base->https ? 443 : 80;
    if (base->port != defaultPort)
        resolved += ":" + std::to_string(base->port);

    if (trimmed.starts_with('/')) {
        resolved.append(trimmed);
    } else {
        std::string_view basePath = base->pathAndQuery;
        const auto queryPos = basePath.find('?');
        if (queryPos != std::string_view::npos)
            basePath = basePath.substr(0, queryPos);
        const auto lastSlash = basePath.rfind('/');
        if (lastSlash == std::string_view::npos)
            resolved.push_back('/');
        else
            resolved.append(basePath.substr(0, lastSlash + 1));
        resolved.append(trimmed);
    }

    if (auto validated = parseAndValidateHttpUrl(resolved, allowHttpLoopbackForTests); !validated)
        return std::unexpected(validated.error());
    return resolved;
}

NetworkPolicy::NetworkPolicy(bool offline, ConsentPromptCallback prompt)
    : m_offline(offline), m_prompt(std::move(prompt)) {}

void NetworkPolicy::setOffline(bool offline) {
    std::lock_guard lock(m_mutex);
    m_offline = offline;
    if (offline)
        m_sessionConsentGranted = false;
}

void NetworkPolicy::setOfflineReader(std::function<bool()> reader) {
    std::lock_guard lock(m_mutex);
    m_offlineReader = std::move(reader);
}

bool NetworkPolicy::isOffline() const {
    std::function<bool()> reader;
    bool staticOffline = true;
    {
        std::lock_guard lock(m_mutex);
        staticOffline = m_offline;
        reader = m_offlineReader;
    }
    if (reader)
        return reader();
    return staticOffline;
}

void NetworkPolicy::setConsentPrompt(ConsentPromptCallback prompt) {
    std::lock_guard lock(m_mutex);
    m_prompt = std::move(prompt);
}

bool NetworkPolicy::hasConsent() const {
    if (isOffline())
        return false;
    std::lock_guard lock(m_mutex);
    return m_sessionConsentGranted;
}

void NetworkPolicy::grantSessionConsent() {
    if (isOffline())
        return;
    std::lock_guard lock(m_mutex);
    m_sessionConsentGranted = true;
}

void NetworkPolicy::revokeConsent() {
    std::lock_guard lock(m_mutex);
    m_sessionConsentGranted = false;
}

Result<void> NetworkPolicy::checkAndRequestConsent(std::string_view url, std::string_view purpose) {
    if (isOffline()) {
        m_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::PermissionDenied, "FrostIDE is working offline; outbound network requests are disabled");
    }

    ConsentPromptCallback prompt;
    {
        std::lock_guard lock(m_mutex);
        if (m_sessionConsentGranted)
            return {};
        prompt = m_prompt;
    }

    if (!prompt) {
        m_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::PermissionDenied, "Outbound network request requires user consent");
    }

    std::string host{};
    if (const auto schemePos = url.find("://"); schemePos != std::string_view::npos) {
        const auto rest = url.substr(schemePos + 3);
        const auto endHost = rest.find_first_of(":/?#");
        host = std::string(rest.substr(0, endHost));
    }

    m_promptCount.fetch_add(1, std::memory_order_relaxed);
    const ConsentDecision decision = prompt(ConsentRequest{
        .url = std::string(url),
        .host = std::move(host),
        .purpose = std::string(purpose),
    });

    if (isOffline()) {
        revokeConsent();
        m_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::PermissionDenied, "FrostIDE switched to offline mode during consent prompt");
    }

    if (decision != ConsentDecision::Granted) {
        m_blockedCount.fetch_add(1, std::memory_order_relaxed);
        return fail(ErrorCode::Cancelled, "User declined consent for outbound network request");
    }

    {
        std::lock_guard lock(m_mutex);
        m_sessionConsentGranted = true;
    }
    return {};
}

PolicyGatedHttpClient::PolicyGatedHttpClient(IHttpClient& transport, NetworkPolicy& policy, std::string purpose)
    : m_transport(transport), m_policy(policy), m_purpose(std::move(purpose)) {}

PolicyGatedHttpClient::PolicyGatedHttpClient(std::shared_ptr<IHttpClient> ownedTransport,
                                             std::shared_ptr<NetworkPolicy> ownedPolicy, std::string purpose)
    : m_ownedTransport(std::move(ownedTransport)), m_ownedPolicy(std::move(ownedPolicy)),
      m_transport(*m_ownedTransport), m_policy(*m_ownedPolicy), m_purpose(std::move(purpose)) {}

HttpRequest PolicyGatedHttpClient::wrapRequestPolicy(const HttpRequest& req) const {
    HttpRequest wrapped = req;
    auto callerCancel = req.isCancelled;
    wrapped.isCancelled = [this, callerCancel = std::move(callerCancel)] {
        if (m_policy.isOffline() || !m_policy.hasConsent())
            return true;
        return callerCancel ? callerCancel() : false;
    };
    return wrapped;
}

Result<HttpResponse> PolicyGatedHttpClient::request(const HttpRequest& req) {
    if (auto permitted = m_policy.checkAndRequestConsent(req.url, m_purpose); !permitted)
        return std::unexpected(permitted.error());
    m_transportRequests.fetch_add(1, std::memory_order_relaxed);
    return m_transport.request(wrapRequestPolicy(req));
}

Result<HttpResponse> PolicyGatedHttpClient::download(const HttpRequest& req, const HttpStreamSink& sink) {
    if (auto permitted = m_policy.checkAndRequestConsent(req.url, m_purpose); !permitted)
        return std::unexpected(permitted.error());
    m_transportRequests.fetch_add(1, std::memory_order_relaxed);
    return m_transport.download(wrapRequestPolicy(req), sink);
}

} // namespace frost::update
