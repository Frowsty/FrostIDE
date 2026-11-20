// SPDX-License-Identifier: Apache-2.0
// Qt-free C2/C3 policy: production auth gate, bounded framing, controls, tools and hook loops.
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace frost::agent {
using rpc::Json;
// Changes only through an ADR citing written provider approval. No runtime setting or auth override.
inline constexpr bool kSubscriptionApprovalRecorded = false;
enum class AuthRoute { ApiKey, ApiKeyHelper, Bedrock, Vertex, Subscription };
[[nodiscard]] Result<AuthRoute> nativeAuthRoute(const Json& status);
struct NativeLaunch {
    std::string executable, root, settingsFile, mcpConfig;
    std::string resume, resumeAt, model, effort;
    bool forkSession = false;
    std::uint16_t idePort = 0;
};
// Requires the freshly obtained auth status for this session. Never reads credentials or login state files.
[[nodiscard]] Result<io::ProcessSpec> nativeLaunch(const NativeLaunch& launch, const Json& freshAuthStatus);
class NdjsonFramer {
public:
    static constexpr std::size_t kMaximumLine = 1024 * 1024;
    // Stateful, single reader. On malformed/oversized input the entire framer fails closed.
    [[nodiscard]] Result<std::vector<Json>> feed(std::string_view bytes);
    [[nodiscard]] Result<void> finish() const;
private:
    std::string m_line;
    bool m_failed = false;
};
// Control traffic is serialized by the owner. Times use steady_clock so wall-clock changes cannot renew approval.
class NativeProtocol {
public:
    using Time = std::chrono::steady_clock::time_point;
    [[nodiscard]] Result<Json> control(std::string subtype, Json fields, Time now);
    [[nodiscard]] Result<void> receive(const Json& event);
    [[nodiscard]] Result<Json> userTurn(std::string text) const;
    [[nodiscard]] std::vector<Json> expire(Time now);
    [[nodiscard]] bool ready() const noexcept { return m_initialized && m_ide && m_mcp; }
    [[nodiscard]] bool supports(std::string_view subtype) const;
    [[nodiscard]] const std::string& sessionId() const noexcept { return m_session; }
private:
    std::uint64_t m_next = 0;
    std::map<std::string, Time> m_pending;
    std::set<std::string, std::less<>> m_capabilities;
    std::string m_session;
    bool m_initialized = false, m_ide = false, m_mcp = false;
};
// Returns tool descriptors only; providers must bind them to real IDE data or return unavailable.
[[nodiscard]] Json mcpToolCatalog();
// Maximum serialized page 12,000 bytes (also conservatively below 25k byte-fallback tokens).
// An individual value that cannot fit is rejected rather than silently truncated.
[[nodiscard]] Result<std::vector<Json>> paginate(const Json& result, std::size_t pageBytes = 12000);
class StopHookGuard {
public:
    // Only new real diagnostic error fingerprints block. Reentrant Stop hooks never recurse.
    // Returns {decision:block,reason:...} at most three times per unchanged error set, then warning.
    [[nodiscard]] Json evaluate(std::string errorFingerprint, std::string errorText, bool stopHookActive);
    void reset();
private:
    std::string m_fingerprint;
    unsigned m_blocks = 0;
};
}
