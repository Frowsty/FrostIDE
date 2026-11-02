// SPDX-License-Identifier: Apache-2.0
//
// Interner: maps byte strings (lines or refinement tokens) to dense uint32 ids, so the diff
// algorithms compare and count integers (ADR 0019, D7 and D13). Ids are assigned in first-seen
// order; numeric id values never affect results, only equality and counts do.
#pragma once

#include "frost/diff/line_sequence.hpp"

#include "work_budget.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace frost::diff::detail {

/*
 * Work units charged per line interned through the hash table by internSequences. On REF-W1
 * (Release) interning a line costs 12-27 ns at 100k lines per side and about 46 ns at 2^21 (the
 * slot table and the key bytes outgrow the caches), while a unit of the histogram, Myers or
 * compaction costs 1.1-5.4 ns (ADR 0019, Measurements). Eight units per line put interning on the
 * same time scale, so that maxWork bounds the time of a call on any input within the line limits
 * (ADR 0019, D7; Budget: B21). A line that takes the partner path (internSequences) costs one unit
 * plus one per 64 bytes compared instead: two sequential reads, no table access.
 */
inline constexpr std::uint64_t kInternUnitsPerLine = 8;

/*
 * An open-addressing hash table keyed by (hash, bytes): the 64-bit hash is compared first (the
 * slot keeps its upper 32 bits for a quick reject), then the length, then the bytes. Capacity is
 * fixed at construction: the next power of two >= 2 * expectedKeys, so the load factor stays at
 * most 1/2. Linear probing.
 *
 * Work: unitsPerCall units per intern() call, one per extra slot probed and one per 64 bytes
 * compared, so a flood of keys that share a hash (FNV-1a is not collision-resistant) exhausts the
 * budget instead of stalling (PLAN.md 3.5, a malicious repository).
 *
 * The interned strings are viewed, not copied: they must outlive the interner. Memory: 8 bytes per
 * slot (two to four slots per expected key) plus 24 bytes per expected key, reserved up front for
 * the keys. Not thread-safe (one per call).
 */
class Interner {
public:
    // Returned by intern() when the budget ran out.
    static constexpr std::uint32_t kExhausted = 0xFFFF'FFFFu;

    // Precondition: at most expectedKeys distinct keys are interned (more keys than that would
    // overfill the table; checked by FROST_PRE in intern()), and unitsPerCall >= 1.
    Interner(std::size_t expectedKeys, std::uint64_t unitsPerCall) noexcept;

    // The id of `bytes` (whose hash the caller computed with fnv1a64, or a test override), assigning
    // the next id when it is new; kExhausted when the budget ran out. Amortised O(1 + |bytes|/64).
    [[nodiscard]] std::uint32_t intern(std::string_view bytes, std::uint64_t hash, WorkBudget& budget) noexcept;

    // The number of distinct ids assigned.
    [[nodiscard]] std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(m_keys.size()); }

    // Heap bytes held (for the memory report of the perf tests).
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    struct Key {
        std::string_view bytes;
        std::uint64_t hash;
    };

    std::vector<std::uint64_t> m_slots; // 0 = empty; else (hash >> 32) << 32 | (id + 1).
    std::vector<Key> m_keys;            // Indexed by id.
    std::size_t m_mask = 0;
    std::uint64_t m_unitsPerCall = 1;
};

// Hash source for internSequences: the sequences' FNV-1a hashes, or (white-box tests only) one
// constant hash for every line, to model a collision flood.
enum class LineHashes : std::uint8_t { Fnv, ConstantForTests };

// Whether internSequences takes the partner path (On, the default) or interns every line through
// the hash table (Off: the plain definition, kept as the oracle of Interner.PartnerPathIsExact).
enum class PartnerPath : std::uint8_t { On, Off };

/*
 * Interns every line of every sequence into one Interner (ids shared across the sequences, assigned
 * in order: the first sequence, then the second, ...), charging kInternUnitsPerLine per line plus
 * the probe and compare units. ids[i] receives sequence i's ids. Returns the number of distinct
 * ids, or Interner::kExhausted when the budget ran out (ids are then incomplete).
 *
 * The partner path (ADR 0019, D7). The later sequences are mostly edited copies of the first (the
 * B of a diff, ours and theirs of a merge), so each of their lines is first compared with its
 * expected partner in the first sequence: the line after the last one it matched. Equal hashes
 * and bytes give the partner's id for one unit plus the compare units, without the table; any
 * other line goes through the table, and when its id occurs in the first sequence the expected
 * partner moves to the line after that id's first position there. The ids are the ones the table
 * would give (equal bytes, equal id), so only the work differs: about 9 instead of 16 units per
 * line pair of a lightly edited diff, and 10 instead of 24 per line triple of a merge. A line that
 * fails the comparison costs at most 1 + compare units more than through the table alone.
 *
 * The table and the id arrays are sized for the lines the budget can pay for (at most
 * budget.remaining() / kInternUnitsPerLine), not for the whole input, so a huge input under a
 * small cap neither allocates nor clears memory it will never use: the interner is built for
 * min(total lines, affordable) keys, and sequence i's ids reserve min(its lines, what the earlier
 * sequences left of affordable); they grow past that only when the cheaper partner path lets more
 * lines in. A non-null `internerBytes` receives the interner's heap bytes
 * (Interner::memoryBytes, for the white-box tests of that sizing). O(total lines + probes),
 * bounded by the budget.
 */
[[nodiscard]] std::uint32_t internSequences(std::span<const LineSequence* const> sequences,
                                            std::span<std::vector<std::uint32_t>> ids, WorkBudget& budget,
                                            LineHashes hashes = LineHashes::Fnv, std::size_t* internerBytes = nullptr,
                                            PartnerPath partners = PartnerPath::On) noexcept;

} // namespace frost::diff::detail
