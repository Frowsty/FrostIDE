// SPDX-License-Identifier: Apache-2.0
// Cryptographic checks for release manifest bytes and downloaded artifacts.
#pragma once

#include <frost/core/error.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace frost::update {
using PublicKey = std::array<unsigned char, 32>;
using Signature = std::array<unsigned char, 64>;
using Sha256 = std::array<unsigned char, 32>;

// Verifies exactly the supplied manifest bytes. The caller owns the trust root and
// must verify before interpreting any field. Thread safe; no retained references.
// Rejects empty or over-64 KiB manifests and invalid signatures.
[[nodiscard]] Result<void> verifyManifest(std::span<const std::byte> bytes, const Signature& signature,
                                          const PublicKey& key);

// Verifies an already bounded artifact against the authenticated size and digest.
// The caller must keep the verified bytes immutable through installer handoff.
// Thread safe; no retained references. Returns Corrupt on any mismatch.
[[nodiscard]] Result<void> verifyPayload(std::span<const std::byte> bytes, std::size_t expectedSize,
                                         const Sha256& digest);
} // namespace frost::update
