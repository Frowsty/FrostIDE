// SPDX-License-Identifier: Apache-2.0
// Retained Windows process identity for an installer helper's parent-exit barrier.
#pragma once

#include <frost/core/error.hpp>

#include <cstdint>
#include <memory>

namespace frost::update {
struct ParentIdentity {
    std::uint32_t pid{};
    std::uint64_t creationTime{}; // Windows FILETIME ticks, not wall-clock Unix time.
};

// Query the calling process identity for a private helper handshake. Windows only;
// other platforms return Unsupported. No retained storage; thread safe, O(1).
[[nodiscard]] Result<ParentIdentity> currentParentIdentity();

// Keeps the exact process object alive as an identity even after it exits. Opening
// and comparing the creation time must happen before the caller authorizes exit.
// This is an exit barrier, not authentication of an arbitrary request: the helper
// must first receive this identity over its directly owned private parent pipe.
// Move-only; methods require external synchronization with moves/destruction.
class ParentProcess final {
public:
    ~ParentProcess();
    ParentProcess(ParentProcess&&) noexcept;
    ParentProcess& operator=(ParentProcess&&) noexcept;
    ParentProcess(const ParentProcess&) = delete;
    ParentProcess& operator=(const ParentProcess&) = delete;

    // O(1). Rejects zero identities, exited processes and creation-time mismatch.
    // Returns Unsupported outside Windows, IoError on query failure, or
    // PermissionDenied for an identity mismatch. Never terminates the process.
    [[nodiscard]] static Result<ParentProcess> acquire(ParentIdentity identity);
    // O(1), nonblocking. Queries the retained object rather than looking up a PID.
    // An empty moved-from guard returns InvalidArgument.
    [[nodiscard]] Result<bool> exited() const;

private:
    struct Impl;
    explicit ParentProcess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::update
