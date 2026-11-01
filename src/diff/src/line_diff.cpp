// SPDX-License-Identifier: Apache-2.0
//
// diffLines, toEdits and checkDiff (frost/diff/line_diff.hpp), and the id-level engine they share
// with merge3 (engine.hpp): interning, the algorithm, the slider pass and hunks from flags.

#include "frost/diff/line_diff.hpp"

#include "frost/core/assert.hpp"

#include "compaction.hpp"
#include "engine.hpp"
#include "histogram.hpp"
#include "interner.hpp"
#include "myers.hpp"
#include "work_budget.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace frost::diff {
namespace detail {

FlagDiff diffIds(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, std::uint32_t idCount,
                 Algorithm algorithm, Sliders sliders, WorkBudget& budget) noexcept {
    FlagDiff out;
    out.changedA.assign(a.size(), 0);
    out.changedB.assign(b.size(), 0);
    if (std::ranges::equal(a, b)) {
        return out;
    }
    if (a.empty() || b.empty()) {
        std::ranges::fill(out.changedA, std::uint8_t{1});
        std::ranges::fill(out.changedB, std::uint8_t{1});
        return out;
    }
    bool complete = false;
    if (algorithm == Algorithm::Histogram) {
        const HistogramResult result = histogramDiff(a, b, idCount, out.changedA, out.changedB, budget);
        complete = !result.exhausted;
        out.fallbacks = result.fallbacks;
    } else {
        const Region whole{0, static_cast<std::uint32_t>(a.size()), 0, static_cast<std::uint32_t>(b.size())};
        complete = myersDiff(a, b, whole, out.changedA, out.changedB, budget);
    }
    if (!complete) {
        out.degraded = true; // Compaction does not run on an exhausted budget.
        return out;
    }
    if (sliders == Sliders::GitClassic && !compactGitClassic(a, b, out.changedA, out.changedB, budget)) {
        out.degraded = true;
    }
    return out;
}

std::vector<Hunk> hunksFromFlags(std::span<const std::uint8_t> changedA,
                                 std::span<const std::uint8_t> changedB) noexcept {
    std::vector<Hunk> hunks;
    const std::size_t n = changedA.size();
    const std::size_t m = changedB.size();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
        if ((i < n && changedA[i] != 0) || (j < m && changedB[j] != 0)) {
            const std::size_t si = i;
            const std::size_t sj = j;
            while (i < n && changedA[i] != 0) {
                ++i;
            }
            while (j < m && changedB[j] != 0) {
                ++j;
            }
            hunks.push_back(
                Hunk{LineRange{LineIndex{static_cast<std::uint32_t>(si)}, LineIndex{static_cast<std::uint32_t>(i)}},
                     LineRange{LineIndex{static_cast<std::uint32_t>(sj)}, LineIndex{static_cast<std::uint32_t>(j)}}});
        } else {
            FROST_ASSERT(i < n && j < m); // Equal numbers of unchanged lines (a valid script).
            ++i;
            ++j;
        }
    }
    return hunks;
}

Result<LineDiff> diffLinesWith(const LineSequence& a, const LineSequence& b, const DiffOptions& options,
                               std::stop_token stop, LineHashes hashes) noexcept {
    FROST_PRE(a.split() == b.split());
    const std::uint32_t maxLines = options.limits.maxLines;
    if (a.size() > maxLines || b.size() > maxLines) {
        return fail(ErrorCode::OutOfRange, std::format("diff of {} and {} lines exceeds the limit of {} lines per side",
                                                       a.size(), b.size(), maxLines));
    }
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "diff cancelled");
    }
    LineDiff out;
    const Hunk whole{LineRange{LineIndex{0}, LineIndex{a.size()}}, LineRange{LineIndex{0}, LineIndex{b.size()}}};
    if (a.bytes() == b.bytes()) {
        return out;
    }
    if (a.size() == 0 || b.size() == 0) {
        out.hunks.push_back(whole);
        return out;
    }
    WorkBudget budget(options.limits.maxWork, stop);
    std::array<std::vector<std::uint32_t>, 2> ids;
    const std::array<const LineSequence*, 2> sequences{&a, &b};
    const std::uint32_t idCount = internSequences(sequences, ids, budget, hashes);
    if (budget.cancelled()) {
        return fail(ErrorCode::Cancelled, "diff cancelled");
    }
    if (idCount == Interner::kExhausted) {
        out.hunks.push_back(whole);
        out.degraded = true;
        out.work = budget.used();
        return out;
    }
    const FlagDiff flags = diffIds(ids[0], ids[1], idCount, options.algorithm, options.sliders, budget);
    if (budget.cancelled()) {
        return fail(ErrorCode::Cancelled, "diff cancelled");
    }
    out.hunks = hunksFromFlags(flags.changedA, flags.changedB);
    out.degraded = flags.degraded;
    out.fallbacks = flags.fallbacks;
    out.work = budget.used();
    return out;
}

} // namespace detail

std::uint64_t LineDiff::deletedLines() const noexcept {
    std::uint64_t total = 0;
    for (const Hunk& hunk : hunks) {
        total += hunk.a.size();
    }
    return total;
}

std::uint64_t LineDiff::insertedLines() const noexcept {
    std::uint64_t total = 0;
    for (const Hunk& hunk : hunks) {
        total += hunk.b.size();
    }
    return total;
}

Result<LineDiff> diffLines(const LineSequence& a, const LineSequence& b, const DiffOptions& options,
                           std::stop_token stop) noexcept {
    return detail::diffLinesWith(a, b, options, std::move(stop), detail::LineHashes::Fnv);
}

std::vector<text::Edit> toEdits(const LineSequence& a, const LineSequence& b, const LineDiff& diff) noexcept {
    std::vector<text::Edit> edits;
    edits.reserve(diff.hunks.size());
    const std::string_view bBytes = b.bytes();
    for (const Hunk& hunk : diff.hunks) {
        const text::ByteRange source = b.byteRange(hunk.b);
        edits.push_back(text::Edit{a.byteRange(hunk.a), bBytes.substr(source.begin.value(), source.size())});
    }
    return edits;
}

Result<void> checkDiff(const LineSequence& a, const LineSequence& b, const LineDiff& diff,
                       const DiffOptions& options) noexcept {
    const std::uint32_t n = a.size();
    const std::uint32_t m = b.size();
    std::uint32_t prevA = 0;
    std::uint32_t prevB = 0;
    // Checks the unchanged run before a hunk (or the tail): equal lengths and byte-equal pairs.
    const auto checkRun = [&](std::uint32_t endA, std::uint32_t endB, std::size_t index) -> Result<void> {
        const std::uint32_t gapA = endA - prevA;
        const std::uint32_t gapB = endB - prevB;
        if (gapA != gapB) {
            return fail(ErrorCode::Internal,
                        std::format("H2: before hunk {}: {} unchanged lines in A but {} in B", index, gapA, gapB));
        }
        for (std::uint32_t k = 0; k < gapA; ++k) {
            if (a.line(LineIndex{prevA + k}) != b.line(LineIndex{prevB + k})) {
                return fail(ErrorCode::Internal,
                            std::format("H3: unchanged A line {} differs from B line {}", prevA + k, prevB + k));
            }
        }
        return {};
    };
    for (std::size_t index = 0; index < diff.hunks.size(); ++index) {
        const Hunk& hunk = diff.hunks[index];
        const std::uint32_t a0 = hunk.a.begin.value();
        const std::uint32_t a1 = hunk.a.end.value();
        const std::uint32_t b0 = hunk.b.begin.value();
        const std::uint32_t b1 = hunk.b.end.value();
        if (a0 > a1 || b0 > b1 || a1 > n || b1 > m) {
            return fail(ErrorCode::Internal,
                        std::format("H1: hunk {} [{},{})x[{},{}) is out of bounds ({} and {} lines)", index, a0, a1, b0,
                                    b1, n, m));
        }
        if (a0 == a1 && b0 == b1) {
            return fail(ErrorCode::Internal, std::format("H1: hunk {} is empty on both sides", index));
        }
        if (a0 < prevA || b0 < prevB) {
            return fail(ErrorCode::Internal, std::format("H1: hunk {} is not sorted", index));
        }
        if (index > 0 && a0 - prevA < 1) {
            return fail(ErrorCode::Internal, std::format("H2: hunk {} touches the previous hunk", index));
        }
        if (auto run = checkRun(a0, b0, index); !run) {
            return run;
        }
        prevA = a1;
        prevB = b1;
    }
    if (auto tail = checkRun(n, m, diff.hunks.size()); !tail) {
        return tail;
    }
    if (options.sliders != Sliders::GitClassic || diff.degraded) {
        return {};
    }
    // H4: the side-B slider pass is a fixed point of the final flags.
    std::vector<std::uint8_t> changedA(n, 0);
    std::vector<std::uint8_t> changedB(m, 0);
    for (const Hunk& hunk : diff.hunks) {
        std::fill(changedA.begin() + hunk.a.begin.value(), changedA.begin() + hunk.a.end.value(), std::uint8_t{1});
        std::fill(changedB.begin() + hunk.b.begin.value(), changedB.begin() + hunk.b.end.value(), std::uint8_t{1});
    }
    detail::WorkBudget unlimited(std::numeric_limits<std::uint64_t>::max(), {});
    std::array<std::vector<std::uint32_t>, 2> ids;
    const std::array<const LineSequence*, 2> sequences{&a, &b};
    if (detail::internSequences(sequences, ids, unlimited) == detail::Interner::kExhausted) {
        return fail(ErrorCode::Internal, "H4: interning failed");
    }
    std::vector<std::uint8_t> again = changedB;
    if (!detail::slidePass(ids[1], again, changedA, unlimited)) {
        return fail(ErrorCode::Internal, "H4: the slider pass ran out of work");
    }
    if (again != changedB) {
        return fail(ErrorCode::Internal, "H4: re-running the side-B slider pass changed the diff");
    }
    return {};
}

} // namespace frost::diff
