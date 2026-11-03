// SPDX-License-Identifier: Apache-2.0
// Detached manifest signatures and SHA-256 payload validation.
#include <frost/update/verification.hpp>

#include <sodium.h>

namespace frost::update {
Result<void> verifyManifest(std::span<const std::byte> bytes, const Signature& signature, const PublicKey& key) {
    if (bytes.empty() || bytes.size() > 64 * 1024)
        return fail(ErrorCode::Corrupt, "Manifest size is invalid");
    if (sodium_init() < 0)
        return fail(ErrorCode::Internal, "Unable to initialize cryptography");
    if (crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(bytes.data()),
                                    bytes.size(), key.data()) != 0)
        return fail(ErrorCode::Corrupt, "Manifest signature is invalid");
    return {};
}

Result<void> verifyPayload(std::span<const std::byte> bytes, std::size_t expectedSize, const Sha256& digest) {
    if (bytes.size() != expectedSize)
        return fail(ErrorCode::Corrupt, "Update payload size is invalid");
    Sha256 actual{};
    if (sodium_init() < 0)
        return fail(ErrorCode::Internal, "Unable to initialize cryptography");
    crypto_hash_sha256(actual.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    if (sodium_memcmp(actual.data(), digest.data(), digest.size()) != 0)
        return fail(ErrorCode::Corrupt, "Update payload digest is invalid");
    return {};
}
} // namespace frost::update
