// SPDX-License-Identifier: Apache-2.0
//
// merge3 and checkMerge (frost/diff/merge.hpp): diff3 chunking over two diffs from base.
/*
 * The chunking follows S. Khanna, K. Kunal and B. C. Pierce, "A Formal Investigation of Diff3"
 * (FSTTCS 2007): with match maps mA (base line -> ours line) and mB (base line -> theirs line) from
 * the unchanged pairs of the two diffs, and cursors lo, la, lb:
 *   - a stable run is the longest i with mA[lo + j] == la + j and mB[lo + j] == lb + j for j < i;
 *     if i > 0 it is emitted and the cursors advance;
 *   - otherwise the least o >= lo with both mA[o] and mB[o] defined ends an unstable chunk
 *     (base [lo, o), ours [la, mA[o]), theirs [lb, mB[o])), after which the next stable run has
 *     i >= 1; with no such o the unstable chunk runs to the ends.
 * Unstable chunks are classified by comparing their sides' ids (see merge.hpp).
 */

#include "frost/diff/merge.hpp"

#include "frost/core/assert.hpp"

#include "engine.hpp"
#include "interner.hpp"
#include "work_budget.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace frost::diff {
namespace {

constexpr std::uint32_t kUnmatched = 0xFFFF'FFFFu;

LineRange range(std::uint32_t begin, std::uint32_t end) noexcept { return LineRange{LineIndex{begin}, LineIndex{end}}; }

// mA[i] for every base line i: its partner in the other side, or kUnmatched.
std::vector<std::uint32_t> matchMap(const detail::FlagDiff& diff) noexcept {
    std::vector<std::uint32_t> map(diff.changedA.size(), kUnmatched);
    std::size_t j = 0;
    for (std::size_t i = 0; i < diff.changedA.size(); ++i) {
        if (diff.changedA[i] != 0) {
            continue;
        }
        while (diff.changedB[j] != 0) {
            ++j;
        }
        map[i] = static_cast<std::uint32_t>(j++);
    }
    return map;
}

bool sameIds(std::span<const std::uint32_t> ids, LineRange r, std::span<const std::uint32_t> other,
             LineRange s) noexcept {
    return r.size() == s.size() &&
           std::equal(ids.begin() + r.begin.value(), ids.begin() + r.end.value(), other.begin() + s.begin.value());
}

// Appends `chunk`, merging it into a preceding Stable chunk when both are Stable.
void emit(std::vector<MergeChunk>& chunks, const MergeChunk& chunk) noexcept {
    if (chunk.base.empty() && chunk.ours.empty() && chunk.theirs.empty()) {
        return;
    }
    if (chunk.kind == ChunkKind::Stable && !chunks.empty() && chunks.back().kind == ChunkKind::Stable) {
        MergeChunk& last = chunks.back();
        last.base.end = chunk.base.end;
        last.ours.end = chunk.ours.end;
        last.theirs.end = chunk.theirs.end;
        return;
    }
    chunks.push_back(chunk);
}

ChunkKind classify(bool baseIsOurs, bool baseIsTheirs, bool oursIsTheirs) noexcept {
    if (baseIsOurs && baseIsTheirs) {
        return ChunkKind::Stable;
    }
    if (baseIsOurs) {
        return ChunkKind::Theirs;
    }
    if (baseIsTheirs) {
        return ChunkKind::Ours;
    }
    return oursIsTheirs ? ChunkKind::Both : ChunkKind::Conflict;
}

std::string_view bytesOf(const LineSequence& sequence, LineRange r) noexcept {
    const text::ByteRange bytes = sequence.byteRange(r);
    return sequence.bytes().substr(bytes.begin.value(), bytes.size());
}

const char* kindName(ChunkKind kind) noexcept {
    switch (kind) {
    case ChunkKind::Stable: return "Stable";
    case ChunkKind::Ours: return "Ours";
    case ChunkKind::Theirs: return "Theirs";
    case ChunkKind::Both: return "Both";
    case ChunkKind::Conflict: return "Conflict";
    }
    return "?";
}

} // namespace

Result<Merge3> merge3(const LineSequence& base, const LineSequence& ours, const LineSequence& theirs,
                      const MergeOptions& options, std::stop_token stop) noexcept {
    FROST_PRE(base.split() == ours.split() && base.split() == theirs.split());
    const std::uint32_t maxLines = options.diff.limits.maxLines;
    if (base.size() > maxLines || ours.size() > maxLines || theirs.size() > maxLines) {
        return fail(ErrorCode::OutOfRange,
                    std::format("merge of {}, {} and {} lines exceeds the limit of {} lines per side", base.size(),
                                ours.size(), theirs.size(), maxLines));
    }
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "merge cancelled");
    }
    Merge3 out;
    const std::uint32_t nO = base.size();
    const std::uint32_t nA = ours.size();
    const std::uint32_t nB = theirs.size();
    detail::WorkBudget budget(options.diff.limits.maxWork, stop);
    std::array<std::vector<std::uint32_t>, 3> ids;
    const std::array<const LineSequence*, 3> sequences{&base, &ours, &theirs};
    const std::uint32_t idCount = detail::internSequences(sequences, ids, budget);
    if (budget.cancelled()) {
        return fail(ErrorCode::Cancelled, "merge cancelled");
    }
    if (idCount == detail::Interner::kExhausted) {
        // One chunk over all three sides, classified by byte equality.
        const bool baseIsOurs = base.bytes() == ours.bytes();
        const bool baseIsTheirs = base.bytes() == theirs.bytes();
        const bool oursIsTheirs = ours.bytes() == theirs.bytes();
        emit(out.chunks,
             MergeChunk{classify(baseIsOurs, baseIsTheirs, oursIsTheirs), range(0, nO), range(0, nA), range(0, nB)});
        out.conflicts = !out.chunks.empty() && out.chunks.front().kind == ChunkKind::Conflict ? 1 : 0;
        out.degraded = true;
        out.work = budget.used();
        return out;
    }
    // Equal halves of what interning left, so that both diffs see the same cap.
    const std::uint64_t half = budget.remaining() / 2;
    detail::WorkBudget budgetA(half, stop);
    detail::WorkBudget budgetB(half, std::move(stop));
    const detail::FlagDiff diffA =
        detail::diffIds(ids[0], ids[1], idCount, options.diff.algorithm, options.diff.sliders, budgetA);
    if (budgetA.cancelled()) {
        return fail(ErrorCode::Cancelled, "merge cancelled");
    }
    const detail::FlagDiff diffB =
        detail::diffIds(ids[0], ids[2], idCount, options.diff.algorithm, options.diff.sliders, budgetB);
    if (budgetB.cancelled()) {
        return fail(ErrorCode::Cancelled, "merge cancelled");
    }
    out.degraded = diffA.degraded || diffB.degraded;
    out.work = budget.used() + budgetA.used() + budgetB.used();
    const std::vector<std::uint32_t> mA = matchMap(diffA);
    const std::vector<std::uint32_t> mB = matchMap(diffB);

    const auto unstable = [&](std::uint32_t o0, std::uint32_t o1, std::uint32_t a0, std::uint32_t a1, std::uint32_t b0,
                              std::uint32_t b1) {
        const LineRange rO = range(o0, o1);
        const LineRange rA = range(a0, a1);
        const LineRange rB = range(b0, b1);
        const ChunkKind kind =
            classify(sameIds(ids[0], rO, ids[1], rA), sameIds(ids[0], rO, ids[2], rB), sameIds(ids[1], rA, ids[2], rB));
        emit(out.chunks, MergeChunk{kind, rO, rA, rB});
    };
    std::uint32_t lo = 0;
    std::uint32_t la = 0;
    std::uint32_t lb = 0;
    std::uint32_t scan = 0; // The least o >= lo not yet ruled out as a common match.
    for (;;) {
        std::uint32_t i = 0;
        while (lo + i < nO && mA[lo + i] == la + i && mB[lo + i] == lb + i) {
            ++i;
        }
        if (i > 0) {
            emit(out.chunks, MergeChunk{ChunkKind::Stable, range(lo, lo + i), range(la, la + i), range(lb, lb + i)});
            lo += i;
            la += i;
            lb += i;
            continue;
        }
        scan = std::max(scan, lo);
        while (scan < nO && (mA[scan] == kUnmatched || mB[scan] == kUnmatched)) {
            ++scan;
        }
        if (scan == nO) {
            if (lo < nO || la < nA || lb < nB) {
                unstable(lo, nO, la, nA, lb, nB);
            }
            break;
        }
        unstable(lo, scan, la, mA[scan], lb, mB[scan]);
        lo = scan;
        la = mA[scan];
        lb = mB[scan];
    }
    out.conflicts = static_cast<std::uint32_t>(
        std::ranges::count_if(out.chunks, [](const MergeChunk& chunk) { return chunk.kind == ChunkKind::Conflict; }));
    return out;
}

Result<void> checkMerge(const LineSequence& base, const LineSequence& ours, const LineSequence& theirs,
                        const Merge3& merge) noexcept {
    std::uint32_t o = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t conflicts = 0;
    for (std::size_t k = 0; k < merge.chunks.size(); ++k) {
        const MergeChunk& chunk = merge.chunks[k];
        if (chunk.base.begin.value() != o || chunk.ours.begin.value() != a || chunk.theirs.begin.value() != b ||
            chunk.base.end < chunk.base.begin || chunk.ours.end < chunk.ours.begin ||
            chunk.theirs.end < chunk.theirs.begin || chunk.base.end.value() > base.size() ||
            chunk.ours.end.value() > ours.size() || chunk.theirs.end.value() > theirs.size()) {
            return fail(ErrorCode::Internal, std::format("C1: chunk {} does not continue the partition", k));
        }
        if (chunk.base.empty() && chunk.ours.empty() && chunk.theirs.empty()) {
            return fail(ErrorCode::Internal, std::format("C4: chunk {} is empty on every side", k));
        }
        if (k > 0 && chunk.kind == ChunkKind::Stable && merge.chunks[k - 1].kind == ChunkKind::Stable) {
            return fail(ErrorCode::Internal, std::format("C4: chunks {} and {} are adjacent Stable chunks", k - 1, k));
        }
        const std::string_view sO = bytesOf(base, chunk.base);
        const std::string_view sA = bytesOf(ours, chunk.ours);
        const std::string_view sB = bytesOf(theirs, chunk.theirs);
        const ChunkKind expected = classify(sO == sA, sO == sB, sA == sB);
        if (chunk.kind == ChunkKind::Stable && expected != ChunkKind::Stable) {
            return fail(ErrorCode::Internal, std::format("C2: Stable chunk {} differs between the sides", k));
        }
        if (chunk.kind != expected) {
            return fail(ErrorCode::Internal, std::format("C3: chunk {} is {} but its sides make it {}", k,
                                                         kindName(chunk.kind), kindName(expected)));
        }
        conflicts += chunk.kind == ChunkKind::Conflict ? 1 : 0;
        o = chunk.base.end.value();
        a = chunk.ours.end.value();
        b = chunk.theirs.end.value();
    }
    if (o != base.size() || a != ours.size() || b != theirs.size()) {
        return fail(ErrorCode::Internal, "C1: the chunks do not cover every side");
    }
    if (conflicts != merge.conflicts) {
        return fail(ErrorCode::Internal,
                    std::format("C4: conflicts is {} but {} chunks are conflicts", merge.conflicts, conflicts));
    }
    return {};
}

} // namespace frost::diff
