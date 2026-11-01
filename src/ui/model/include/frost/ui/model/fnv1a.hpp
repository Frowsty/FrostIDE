// SPDX-License-Identifier: Apache-2.0
//
// FNV-1a 64-bit hashing for the theme and icon formats: the key-registry hash, the theme
// blob checksum, the icon atlas table checksum and the atlas source hash (ADR 0015).
//
/*
 * The algorithm is Fowler/Noll/Vo FNV-1a, which its authors placed in the public domain
 * (http://www.isthe.com/chongo/tech/comp/fnv/index.html): start from the 64-bit offset
 * basis, then for every byte XOR it into the state and multiply by the 64-bit FNV prime,
 * modulo 2^64. It is a checksum, not a cryptographic hash: it detects corruption, not
 * tampering. For a fixed byte, each step is a bijection of the state (XOR with a constant,
 * then multiplication by an odd number), so two inputs of equal length that differ in one
 * byte always hash differently; the blob and atlas tests rely on that.
 *
 * Every function here is constexpr, pure, thread-safe and O(n).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace frost::ui {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

// Continues an FNV-1a hash from `state` over `bytes`; pass kFnvOffsetBasis to start a new one.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::span<const std::byte> bytes,
                                              std::uint64_t state = kFnvOffsetBasis) noexcept {
    for (const std::byte b : bytes) {
        state ^= static_cast<std::uint64_t>(b);
        state *= kFnvPrime;
    }
    return state;
}

// The same over the bytes of a string (each char as an unsigned byte).
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text, std::uint64_t state = kFnvOffsetBasis) noexcept {
    for (const char c : text) {
        state ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        state *= kFnvPrime;
    }
    return state;
}

// Continues a hash over the little-endian bytes of an unsigned integer of `bytes` bytes (1..8).
[[nodiscard]] constexpr std::uint64_t fnv1a64Integer(std::uint64_t value, int bytes, std::uint64_t state) noexcept {
    for (int i = 0; i < bytes; ++i) {
        state ^= (value >> (8 * i)) & 0xffU;
        state *= kFnvPrime;
    }
    return state;
}

// Streaming form: feed bytes in pieces and read value() at any point. Same result as one call.
class Fnv1a64 {
public:
    constexpr void update(std::span<const std::byte> bytes) noexcept { m_state = fnv1a64(bytes, m_state); }
    constexpr void update(std::string_view text) noexcept { m_state = fnv1a64(text, m_state); }
    constexpr void updateInteger(std::uint64_t value, int bytes) noexcept {
        m_state = fnv1a64Integer(value, bytes, m_state);
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_state; }

private:
    std::uint64_t m_state = kFnvOffsetBasis;
};

} // namespace frost::ui
