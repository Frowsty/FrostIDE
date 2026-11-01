// SPDX-License-Identifier: Apache-2.0
//
// XXH3 fingerprints over xxHash 0.8.4 (BSD-2-Clause; see frost/storage/fingerprint.hpp).

#include "frost/storage/fingerprint.hpp"

#include "frost/core/assert.hpp"

// XXH3_state_t's definition (for the stack state in xxh3Hash128Concat) is visible only with
// XXH_STATIC_LINKING_ONLY; the library is linked statically, so its layout matches.
#define XXH_STATIC_LINKING_ONLY
#include <format>
#include <new>
#include <utility>

#include <xxhash.h>

namespace frost::storage {

namespace {

// XXH3_state_t is declared XXH_ALIGN_MEMBER(64, ...): its first member is 64-byte aligned.
constexpr std::align_val_t kStateAlignment{64};

XXH3_state_t* newState() {
    void* memory = ::operator new(sizeof(XXH3_state_t), kStateAlignment);
    auto* state = static_cast<XXH3_state_t*>(memory);
    XXH3_INITSTATE(state);
    return state;
}

void deleteState(void* state) noexcept {
    if (state != nullptr) {
        ::operator delete(state, sizeof(XXH3_state_t), kStateAlignment);
    }
}

Fingerprint128 fromHash(const XXH128_hash_t& hash) noexcept { return Fingerprint128{hash.high64, hash.low64}; }

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

std::array<std::byte, 16> Fingerprint128::canonicalBytes() const noexcept {
    std::array<std::byte, 16> out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::byte>((high >> (56 - 8 * i)) & 0xFF);
        out[static_cast<std::size_t>(8 + i)] = static_cast<std::byte>((low >> (56 - 8 * i)) & 0xFF);
    }
    return out;
}

Result<Fingerprint128> Fingerprint128::fromCanonicalBytes(std::string_view bytes) {
    if (bytes.size() != 16) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("fingerprint: {} canonical bytes given, 16 expected", bytes.size()));
    }
    Fingerprint128 value;
    for (std::size_t i = 0; i < 8; ++i) {
        value.high = (value.high << 8) | static_cast<unsigned char>(bytes[i]);
        value.low = (value.low << 8) | static_cast<unsigned char>(bytes[8 + i]);
    }
    return value;
}

std::string Fingerprint128::toHex() const { return std::format("{:016x}{:016x}", high, low); }

Result<Fingerprint128> Fingerprint128::fromHex(std::string_view hex) {
    if (hex.size() != 32) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("fingerprint: {} hex digits given, 32 expected", hex.size()));
    }
    Fingerprint128 value;
    for (std::size_t i = 0; i < 32; ++i) {
        const int digit = hexDigit(hex[i]);
        if (digit < 0) {
            return fail(ErrorCode::InvalidArgument, std::format("fingerprint: non-hex character at offset {}", i));
        }
        std::uint64_t& half = i < 16 ? value.high : value.low;
        half = (half << 4) | static_cast<std::uint64_t>(digit);
    }
    return value;
}

std::uint64_t xxh3Hash64(std::string_view bytes, std::uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

Fingerprint128 xxh3Hash128(std::string_view bytes, std::uint64_t seed) noexcept {
    return fromHash(XXH3_128bits_withSeed(bytes.data(), bytes.size(), seed));
}

Fingerprint128 xxh3Hash128Concat(std::span<const std::string_view> pieces) noexcept {
    // The state is about 576 bytes; on the stack, so the one-shot form never allocates.
    XXH3_state_t state;
    XXH3_INITSTATE(&state);
    static_cast<void>(XXH3_128bits_reset(&state));
    for (const std::string_view piece : pieces) {
        static_cast<void>(XXH3_128bits_update(&state, piece.data(), piece.size()));
    }
    return fromHash(XXH3_128bits_digest(&state));
}

// ---------------------------------------------------------------------------------------------
// Xxh3Stream64

Xxh3Stream64::Xxh3Stream64(std::uint64_t seed) : m_state(newState()), m_seed(seed) { reset(); }

Xxh3Stream64::Xxh3Stream64(Xxh3Stream64&& other) noexcept
    : m_state(std::exchange(other.m_state, nullptr)), m_seed(other.m_seed) {}

Xxh3Stream64& Xxh3Stream64::operator=(Xxh3Stream64&& other) noexcept {
    if (this != &other) {
        deleteState(m_state);
        m_state = std::exchange(other.m_state, nullptr);
        m_seed = other.m_seed;
    }
    return *this;
}

Xxh3Stream64::~Xxh3Stream64() { deleteState(m_state); }

void Xxh3Stream64::update(std::string_view bytes) noexcept {
    FROST_PRE(m_state != nullptr);
    // XXH_ERROR only for a null state, which FROST_PRE ruled out.
    static_cast<void>(XXH3_64bits_update(static_cast<XXH3_state_t*>(m_state), bytes.data(), bytes.size()));
}

std::uint64_t Xxh3Stream64::digest() const noexcept {
    FROST_PRE(m_state != nullptr);
    return XXH3_64bits_digest(static_cast<const XXH3_state_t*>(m_state));
}

void Xxh3Stream64::reset() noexcept {
    FROST_PRE(m_state != nullptr);
    static_cast<void>(XXH3_64bits_reset_withSeed(static_cast<XXH3_state_t*>(m_state), m_seed));
}

// ---------------------------------------------------------------------------------------------
// Xxh3Stream128

Xxh3Stream128::Xxh3Stream128(std::uint64_t seed) : m_state(newState()), m_seed(seed) { reset(); }

Xxh3Stream128::Xxh3Stream128(Xxh3Stream128&& other) noexcept
    : m_state(std::exchange(other.m_state, nullptr)), m_seed(other.m_seed) {}

Xxh3Stream128& Xxh3Stream128::operator=(Xxh3Stream128&& other) noexcept {
    if (this != &other) {
        deleteState(m_state);
        m_state = std::exchange(other.m_state, nullptr);
        m_seed = other.m_seed;
    }
    return *this;
}

Xxh3Stream128::~Xxh3Stream128() { deleteState(m_state); }

void Xxh3Stream128::update(std::string_view bytes) noexcept {
    FROST_PRE(m_state != nullptr);
    static_cast<void>(XXH3_128bits_update(static_cast<XXH3_state_t*>(m_state), bytes.data(), bytes.size()));
}

Fingerprint128 Xxh3Stream128::digest() const noexcept {
    FROST_PRE(m_state != nullptr);
    return fromHash(XXH3_128bits_digest(static_cast<const XXH3_state_t*>(m_state)));
}

void Xxh3Stream128::reset() noexcept {
    FROST_PRE(m_state != nullptr);
    static_cast<void>(XXH3_128bits_reset_withSeed(static_cast<XXH3_state_t*>(m_state), m_seed));
}

} // namespace frost::storage
