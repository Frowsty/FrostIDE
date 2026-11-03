// SPDX-License-Identifier: Apache-2.0
// Authenticated release manifest parsing and platform/channel policy.
#pragma once

#include <frost/update/verification.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::update {
struct Release {
    std::string channel;
    std::string version;
    std::uint64_t build{};
    std::string url;
    std::uint64_t size{};
    Sha256 digest{};
};
// Exact authenticated envelope retained for final handoff revalidation.
struct AuthenticatedRelease {
    Release release;
    std::vector<std::byte> manifest;
    Signature signature;
};
struct ReleasePolicy {
    std::string_view channel;
    std::string_view platform;
    std::string_view architecture;
    std::uint64_t nowUnix{};
    std::uint64_t installedBuild{};
    std::uint64_t acceptedBuild{};
    std::string_view installedVersion;
    std::string_view acceptedVersion;
    std::string_view acceptedDigest;
};
// Per-user anti-replay floor. Zero is permitted only for an absent state file;
// an encoded record must have a nonzero build, valid SemVer and lowercase SHA-256.
struct ReleaseFloor {
    std::string version;
    std::uint64_t build{};
    std::string digest;
};
// Bounded strict codec, independent of signature verification. Rejects duplicate
// or unknown keys and malformed fields. Neither function retains caller storage.
[[nodiscard]] Result<ReleaseFloor> parseReleaseFloor(std::string_view bytes);
[[nodiscard]] Result<std::string> encodeReleaseFloor(const ReleaseFloor& floor);
// Verifies the detached signature before parsing. Rejects malformed or duplicate
// fields, expired releases, other channels/platforms, replay and non-GitHub assets.
// Caller provides an independently configured key and a trusted monotonic floor.
// Does not retain input; thread safe. A returned release is not an installed update.
[[nodiscard]] Result<Release> verifyRelease(std::span<const std::byte> manifest, const Signature& signature,
                                            const PublicKey& key, const ReleasePolicy& policy);
// Discovery variant: validates the same signature, identity, expiry and schema,
// but returns an empty optional for an authentic release older than the current
// installed/accepted floor. Errors still identify invalid evidence. Never persist
// a floor or start installation from an empty result. Same ownership/thread rules.
[[nodiscard]] Result<std::optional<Release>> eligibleRelease(std::span<const std::byte> manifest,
                                                             const Signature& signature, const PublicKey& key,
                                                             const ReleasePolicy& policy);
} // namespace frost::update
