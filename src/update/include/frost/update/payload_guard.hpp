// SPDX-License-Identifier: Apache-2.0
// Immutable Windows payload ownership for a future installer handoff.
#pragma once

#include <frost/update/manifest.hpp>

#include <filesystem>
#include <memory>

namespace frost::update {
// Windows-only guard for a local NTFS, regular, single-link payload. A read oplock
// excludes existing writable mappings; restrictive sharing excludes new writes
// and deletion. Physical directory handles keep the launch path from being renamed;
// held children keep ancestors occupied, preventing NTFS junction conversion.
// The caller must supply an authenticated Release and retain this guard until its
// installer has completed. This class does not authorize or launch an installer.
// Acquire/destruction can wait for local filesystem I/O and cancellation: use a
// helper/background thread, never the GUI thread. No hostile administrator model.
// Move-only; synchronize all access with moves/destruction externally.
class PayloadGuard final {
public:
    ~PayloadGuard();
    PayloadGuard(PayloadGuard&&) noexcept;
    PayloadGuard& operator=(PayloadGuard&&) noexcept;
    PayloadGuard(const PayloadGuard&) = delete;
    PayloadGuard& operator=(const PayloadGuard&) = delete;

    // Streams at most the signed size (1..2 GiB), using bounded 64 KiB buffers.
    // Rejects nonphysical drive aliases, reparses, multiple links, unsupported
    // oplocks, any existing writer/mapping, and size/digest mismatches. Other
    // platforms return Unsupported. No file mutation is performed.
    [[nodiscard]] static Result<PayloadGuard> acquire(const std::filesystem::path& path, const Release& release);
    // Recheck before installer launch and after its terminal result. O(path depth).
    // Reports a broken oplock or changed identity/size. Oplock breaks are terminal;
    // other failures are observations, not latched state. Stop using a failed guard.
    [[nodiscard]] Result<void> validate() const;
    // UTF-8 physical path for the fixed installer argument. Valid only while this
    // guard is held; empty on a moved-from object. Never use the original alias.
    [[nodiscard]] std::string_view installerPath() const noexcept;

private:
    struct Impl;
    explicit PayloadGuard(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::update
