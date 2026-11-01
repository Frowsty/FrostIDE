// SPDX-License-Identifier: Apache-2.0
//
// 64-bit FNV-1a, the line and token hash of frost::diff (ADR 0019, D13). FNV is public domain;
// the constants are the 64-bit offset basis and prime of draft-eastlake-fnv ("The FNV
// Non-Cryptographic Hash Algorithm", Fowler, Noll, Vo and Eastlake). Fast and simple, but not
// collision-resistant: the interner defends against crafted collisions by counting probe steps as
// work. TODO(M5): XXH3 once m1/storage brings the xxhash port.
#pragma once

#include <cstdint>
#include <string_view>

namespace frost::diff::detail {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x0000'0100'0000'01B3ULL;

// FNV-1a over `bytes`: for each byte, xor it in, then multiply by the prime (mod 2^64). O(n); pure.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

static_assert(fnv1a64("") == kFnvOffsetBasis, "FNV-1a of the empty string is the offset basis");
// Test vector from draft-eastlake-fnv: FNV-1a 64 of "a" is 0xAF63DC4C8601EC8C.
static_assert(fnv1a64("a") == 0xAF63'DC4C'8601'EC8CULL, "FNV-1a 64 test vector");

} // namespace frost::diff::detail
