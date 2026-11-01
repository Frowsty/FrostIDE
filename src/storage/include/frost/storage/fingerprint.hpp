// SPDX-License-Identifier: Apache-2.0
//
// XXH3 fingerprints for frost::storage (xxHash 0.8.4, BSD-2-Clause; ADR 0014): the 128-bit
// content fingerprints of the hot-exit journal and local history, and the 64-bit checksums that
// FSEG segments (M2) will use.
//
/*
 * XXH3 is NOT a cryptographic hash. Never use it for trust decisions, signatures or update
 * checks (the updater uses SHA-256 and Ed25519, PLAN.md section 2.2). An adversary can make
 * collisions at will, so a unique key built on a fingerprint must verify the content on a hit:
 * local history compares the bytes before it reuses a blob (local_history.hpp).
 *
 * Persisted fingerprints always use seed 0, and their stored form is the canonical one below,
 * so a vcpkg bump of xxHash that changed the output would be caught by the regression golden
 * tests/storage/goldens/xxh3_vectors.txt before it reached a user's files.
 *
 * xxhash.h is included only by fingerprint.cpp; this header stays free of it.
 */
#pragma once

#include "frost/core/error.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace frost::storage {

/*
 * An XXH3-128 value. The canonical form is 16 bytes: `high` big-endian, then `low` big-endian
 * (XXH128_canonicalFromHash, and the order xxhsum prints). operator<=> orders by (high, low),
 * which is the same order as memcmp of the canonical bytes, so a BLOB column holding canonical
 * bytes sorts like the values. A plain value type: copyable, thread-safe to read.
 */
struct Fingerprint128 {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend constexpr auto operator<=>(const Fingerprint128&, const Fingerprint128&) = default;

    // The 16 canonical bytes. O(1).
    [[nodiscard]] std::array<std::byte, 16> canonicalBytes() const noexcept;
    // Parses 16 canonical bytes. Errors: InvalidArgument unless `bytes` has exactly 16 bytes.
    [[nodiscard]] static Result<Fingerprint128> fromCanonicalBytes(std::string_view bytes);
    // 32 lowercase hex digits in canonical order.
    [[nodiscard]] std::string toHex() const;
    // Parses 32 hex digits of either case. Errors: InvalidArgument for any other length or character.
    [[nodiscard]] static Result<Fingerprint128> fromHex(std::string_view hex);
};

// One-shot hashes. Pure, thread-safe, O(n), never allocate. Budget: B4.
[[nodiscard]] std::uint64_t xxh3Hash64(std::string_view bytes, std::uint64_t seed = 0) noexcept;
// Budget: B4.
[[nodiscard]] Fingerprint128 xxh3Hash128(std::string_view bytes, std::uint64_t seed = 0) noexcept;

/*
 * xxh3Hash128 of the concatenation of `pieces`, seed 0, without concatenating them: the XXH3
 * state lives on the stack, so this never allocates. A distinct name rather than an overload,
 * so a braced argument list never picks a different overload on a standard library that has
 * C++26 span's initializer_list constructor. Pure, thread-safe, O(total size). Budget: B4.
 */
[[nodiscard]] Fingerprint128 xxh3Hash128Concat(std::span<const std::string_view> pieces) noexcept;

/*
 * Streaming XXH3-64: digest() equals xxh3Hash64 of everything passed to update() since
 * construction or reset(), for any split of the input. Move-only; not thread-safe per object
 * (different objects are independent). Construction makes one heap allocation, the 64-byte
 * aligned XXH3 state, with aligned operator new, so running out of memory behaves like any other
 * allocation. A moved-from stream may only be destroyed or assigned to; any other call is a
 * precondition violation (FROST_PRE).
 */
class Xxh3Stream64 {
public:
    explicit Xxh3Stream64(std::uint64_t seed = 0);
    Xxh3Stream64(Xxh3Stream64&& other) noexcept;
    Xxh3Stream64& operator=(Xxh3Stream64&& other) noexcept;
    Xxh3Stream64(const Xxh3Stream64&) = delete ("the XXH3 state is owned; move it instead");
    Xxh3Stream64& operator=(const Xxh3Stream64&) = delete ("the XXH3 state is owned; move it instead");
    ~Xxh3Stream64();

    // Appends `bytes`. O(bytes.size()). Budget: B4.
    void update(std::string_view bytes) noexcept;
    // The hash of the bytes so far; the stream can continue afterwards. O(1).
    [[nodiscard]] std::uint64_t digest() const noexcept;
    // Starts over with the construction seed. O(1).
    void reset() noexcept;

private:
    void* m_state = nullptr; // XXH3_state_t, 64-byte aligned (fingerprint.cpp).
    std::uint64_t m_seed = 0;
};

// Streaming XXH3-128; the same contract as Xxh3Stream64, with digest() equal to xxh3Hash128.
class Xxh3Stream128 {
public:
    explicit Xxh3Stream128(std::uint64_t seed = 0);
    Xxh3Stream128(Xxh3Stream128&& other) noexcept;
    Xxh3Stream128& operator=(Xxh3Stream128&& other) noexcept;
    Xxh3Stream128(const Xxh3Stream128&) = delete ("the XXH3 state is owned; move it instead");
    Xxh3Stream128& operator=(const Xxh3Stream128&) = delete ("the XXH3 state is owned; move it instead");
    ~Xxh3Stream128();

    // Appends `bytes`. O(bytes.size()). Budget: B4.
    void update(std::string_view bytes) noexcept;
    [[nodiscard]] Fingerprint128 digest() const noexcept;
    void reset() noexcept;

private:
    void* m_state = nullptr;
    std::uint64_t m_seed = 0;
};

} // namespace frost::storage

// Hashes a Fingerprint128 by its low half (already uniformly distributed). O(1), noexcept.
template<>
struct std::hash<frost::storage::Fingerprint128> {
    [[nodiscard]] std::size_t operator()(const frost::storage::Fingerprint128& value) const noexcept {
        return static_cast<std::size_t>(value.low);
    }
};
