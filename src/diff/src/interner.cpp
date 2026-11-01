// SPDX-License-Identifier: Apache-2.0
//
// Interner (interner.hpp): open addressing with linear probing over a power-of-two table, with
// every probe step charged to the work budget (ADR 0019, D7 and D13).

#include "interner.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace frost::diff::detail {
namespace {

constexpr std::uint64_t kLowMask = 0xFFFF'FFFFULL;

// Units for comparing `bytes` bytes: one per 64.
constexpr std::uint64_t compareUnits(std::size_t bytes) noexcept { return bytes >> 6; }

} // namespace

Interner::Interner(std::size_t expectedKeys, std::uint64_t unitsPerCall) noexcept : m_unitsPerCall(unitsPerCall) {
    FROST_PRE(unitsPerCall >= 1);
    const std::size_t want = expectedKeys < 1 ? 2 : expectedKeys * 2;
    const std::size_t capacity = std::bit_ceil(want);
    m_slots.assign(capacity, 0);
    m_mask = capacity - 1;
    m_keys.reserve(expectedKeys);
}

std::uint32_t Interner::intern(std::string_view bytes, std::uint64_t hash, WorkBudget& budget) noexcept {
    if (!budget.charge(m_unitsPerCall)) {
        return kExhausted;
    }
    const std::uint64_t tag = hash & ~kLowMask;
    std::size_t slot = static_cast<std::size_t>(hash) & m_mask;
    for (;;) {
        const std::uint64_t entry = m_slots[slot];
        if (entry == 0) {
            FROST_PRE(m_keys.size() < m_slots.size() / 2 + 1);
            const auto id = static_cast<std::uint32_t>(m_keys.size());
            m_keys.push_back(Key{bytes, hash});
            m_slots[slot] = tag | (std::uint64_t{id} + 1);
            return id;
        }
        if ((entry & ~kLowMask) == tag) {
            const auto id = static_cast<std::uint32_t>((entry & kLowMask) - 1);
            const Key& key = m_keys[id];
            if (key.hash == hash && key.bytes.size() == bytes.size()) {
                if (!budget.charge(compareUnits(bytes.size()))) {
                    return kExhausted;
                }
                if (bytes.empty() || std::memcmp(key.bytes.data(), bytes.data(), bytes.size()) == 0) {
                    return id;
                }
            }
        }
        // Another key: one unit per extra slot probed.
        if (!budget.charge()) {
            return kExhausted;
        }
        slot = (slot + 1) & m_mask;
    }
}

std::size_t Interner::memoryBytes() const noexcept {
    return m_slots.capacity() * sizeof(std::uint64_t) + m_keys.capacity() * sizeof(Key);
}

std::uint32_t internSequences(std::span<const LineSequence* const> sequences, std::span<std::vector<std::uint32_t>> ids,
                              WorkBudget& budget, LineHashes hashes, std::size_t* internerBytes,
                              PartnerPath partners) noexcept {
    FROST_PRE(sequences.size() == ids.size());
    std::uint64_t total = 0;
    for (const LineSequence* sequence : sequences) {
        total += sequence->size();
    }
    // Every intern() call charges kInternUnitsPerLine first, so at most `affordable` calls (and as
    // many distinct keys) can succeed before the budget runs out.
    const std::uint64_t affordable = budget.remaining() / kInternUnitsPerLine;
    Interner interner(static_cast<std::size_t>(std::min(total, affordable)), kInternUnitsPerLine);
    if (internerBytes != nullptr) {
        *internerBytes = interner.memoryBytes(); // Fixed at construction: the keys are reserved.
    }
    const auto hashOf = [hashes](const LineSequence& sequence, LineIndex line) {
        return hashes == LineHashes::Fnv ? sequence.lineHash(line) : 0x5EED'5EED'5EED'5EEDULL;
    };
    // Per id first seen in the first sequence: that position (where the partner path re-aligns).
    std::vector<std::uint32_t> firstPosition;
    std::uint64_t left = affordable;
    for (std::size_t s = 0; s < sequences.size(); ++s) {
        const LineSequence& sequence = *sequences[s];
        std::vector<std::uint32_t>& out = ids[s];
        out.clear();
        out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(sequence.size(), left)));
        const bool aligned = s > 0 && partners == PartnerPath::On;
        const LineSequence& first = *sequences[0];
        std::uint32_t partner = 0; // The first sequence's line expected to equal the next line.
        for (std::uint32_t i = 0; i < sequence.size(); ++i) {
            const LineIndex line{i};
            const std::uint64_t hash = hashOf(sequence, line);
            const std::string_view bytes = sequence.line(line);
            if (aligned && partner < first.size() && hashOf(first, LineIndex{partner}) == hash) {
                // The partner path: one unit plus the byte comparison (interner.hpp).
                if (!budget.charge(1 + compareUnits(bytes.size()))) {
                    return Interner::kExhausted;
                }
                if (first.line(LineIndex{partner}) == bytes) {
                    out.push_back(ids[0][partner]);
                    ++partner;
                    continue;
                }
            }
            const std::uint32_t id = interner.intern(bytes, hash, budget);
            if (id == Interner::kExhausted) {
                return Interner::kExhausted;
            }
            if (s == 0 && id == firstPosition.size()) {
                firstPosition.push_back(i);
            } else if (aligned && id < firstPosition.size()) {
                partner = firstPosition[id] + 1; // Re-align after the first sequence's copy of the line.
            }
            out.push_back(id);
        }
        left -= std::min<std::uint64_t>(left, sequence.size());
    }
    return interner.size();
}

} // namespace frost::diff::detail
