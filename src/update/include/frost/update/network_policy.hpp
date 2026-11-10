// SPDX-License-Identifier: Apache-2.0
// Offline-by-default network policy and user consent gate for outbound HTTP requests (FND-16).
#pragma once

#include <frost/update/http_client.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace frost::update {

struct ConsentRequest {
    std::string url{};
    std::string host{};
    std::string purpose{};
};

enum class ConsentDecision : std::uint8_t {
    Granted,
    Denied,
};

using ConsentPromptCallback = std::function<ConsentDecision(const ConsentRequest&)>;

// Thread-safe network policy: offline by default, requiring explicit user consent before the
// first outbound request in a session when online mode is enabled.
class NetworkPolicy final {
public:
    NetworkPolicy() = default;
    explicit NetworkPolicy(bool offline, ConsentPromptCallback prompt = {});

    NetworkPolicy(const NetworkPolicy&) = delete;
    NetworkPolicy& operator=(const NetworkPolicy&) = delete;

    // Sets the static offline flag. Switching to offline revokes any prior session consent.
    void setOffline(bool offline);

    // Optional live settings reader (e.g. reading `network.offline` from SettingsStore).
    // When set, `isOffline()` returns true if either the static flag or the reader is true.
    void setOfflineReader(std::function<bool()> reader);

    [[nodiscard]] bool isOffline() const;

    void setConsentPrompt(ConsentPromptCallback prompt);

    [[nodiscard]] bool hasConsent() const;
    void grantSessionConsent();
    void revokeConsent();

    // Checks offline status and, on the first request while online, invokes the consent prompt.
    // Returns PermissionDenied when offline or when no consent prompt is configured, and
    // Cancelled when the user declines the consent prompt.
    [[nodiscard]] Result<void> checkAndRequestConsent(std::string_view url, std::string_view purpose = "update");

    [[nodiscard]] std::uint64_t promptCount() const noexcept { return m_promptCount.load(); }
    [[nodiscard]] std::uint64_t blockedRequestCount() const noexcept { return m_blockedCount.load(); }

private:
    mutable std::mutex m_mutex;
    bool m_offline = true;
    bool m_sessionConsentGranted = false;
    std::function<bool()> m_offlineReader{};
    ConsentPromptCallback m_prompt{};
    std::atomic<std::uint64_t> m_promptCount{0};
    std::atomic<std::uint64_t> m_blockedCount{0};
};

// Decorator around IHttpClient that enforces NetworkPolicy before any transport call is made.
// When offline or when consent is denied, 0 requests reach the underlying transport.
class PolicyGatedHttpClient final : public IHttpClient {
public:
    PolicyGatedHttpClient(IHttpClient& transport, NetworkPolicy& policy, std::string purpose = "update");
    PolicyGatedHttpClient(std::shared_ptr<IHttpClient> ownedTransport, std::shared_ptr<NetworkPolicy> ownedPolicy,
                          std::string purpose = "update");

    [[nodiscard]] Result<HttpResponse> request(const HttpRequest& req) override;
    [[nodiscard]] Result<HttpResponse> download(const HttpRequest& req, const HttpStreamSink& sink) override;

    [[nodiscard]] std::uint64_t transportRequestCount() const noexcept { return m_transportRequests.load(); }

private:
    [[nodiscard]] HttpRequest wrapRequestPolicy(const HttpRequest& req) const;

    std::shared_ptr<IHttpClient> m_ownedTransport{};
    std::shared_ptr<NetworkPolicy> m_ownedPolicy{};
    IHttpClient& m_transport;
    NetworkPolicy& m_policy;
    std::string m_purpose;
    std::atomic<std::uint64_t> m_transportRequests{0};
};

} // namespace frost::update
