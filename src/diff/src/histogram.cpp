// SPDX-License-Identifier: Apache-2.0
//
// The histogram diff (histogram.hpp; ADR 0019, D3).
/*
 * Source of the algorithm (CLAUDE.md: borrowed algorithms cite their MIT/Apache/BSD source):
 *   JGit, https://github.com/eclipse-jgit/jgit at master c43e21c9e8c1110af253a8c1aa262f156591f87b
 *   (2026-11-01), the histogram algorithm of org.eclipse.jgit/src/org/eclipse/jgit/diff/
 *   HistogramDiff.java and HistogramDiffIndex.java. "Copyright (C) 2010, Google Inc. and others",
 *   Eclipse Distribution License 1.0 (SPDX BSD-3-Clause). JGit credits Bram Cohen's patience diff
 *   for the idea of anchoring on rare lines.
 * Provenance: this file implements the behavioural rules below (ADR 0019, D3) with its own
 * structure: a per-region occurrence index built by one forward pass with a tail pointer per line
 * (and narrowed in place for a region that is a suffix of the indexed one), an anchor search
 * object that grows and ranks candidate matches, a cache of the runs enclosing regions grew, and a
 * driver over an explicit stack. It reproduces no JGit code. git's xdiff sources (GPL-2.0) were
 * never read; git's behaviour was established as a black box.
 *
 * The rules (ADR 0019, D3):
 *   - The whole input is one region, with no common prefix/suffix trimming (git does not trim;
 *     trimming changes 3-6 % of git's hunks). Regions wait on an explicit stack.
 *   - A queued region with one empty side, or of 1 x 1 lines, is a replace. A 1 x 1 region is
 *     always unequal: it borders a common run that was grown as far as the same bounds allowed
 *     (ADR 0019 has the proof).
 *   - Otherwise the region's A part is indexed: each line's occurrence count (saturated at 255)
 *     and its positions, earliest first. B's part is then walked from its start, keeping a rarity
 *     threshold (initially 65) and the best anchor so far:
 *       a B line absent from A's part is passed over;
 *       a B line present there means the region has a common line; if it occurs more often than
 *       the threshold it is passed over as well;
 *       otherwise each of its A positions, earliest first, is grown into a maximal common run
 *       around (A position, B position) inside the region. The run's rarity is the lowest
 *       occurrence count among its A lines. The run becomes the best anchor when there is none
 *       yet, when it is longer on A's side than the best, or when its rarity is below the
 *       threshold; the threshold then becomes its rarity. The next A position tried is the first
 *       later one at or beyond the end of the run just grown.
 *       The walk resumes after the furthest B end among the runs grown at this B position (at
 *       least one past it).
 *   - Then: no common line, the region is a replace; a threshold still above 64 (every common line
 *     repeats more than 64 times, or the best anchor's rarity is 65), the region goes to Myers (a
 *     "fallback"); otherwise the best anchor is a common run, and the regions before and after it
 *     are queued (before is handled first; the change flags make the order irrelevant to the
 *     result, but it fixes where a work cap strikes).
 *   - JGit also gives up when 64 distinct lines share one hash bucket; ids are exact, so there are
 *     no buckets and that rule never applies (no effect in the black-box comparison with git).
 *
 * Reuse (ADR 0019, D3 and D7). Three shortcuts compute exactly what the rules above compute, with
 * less work; they matter for edits at evenly spaced lines, where the best anchor is the first of
 * many equal runs, so each region's after part is nearly the whole region and the plain rules cost
 * O(N x D) (10k lines with every 10th edited needed 2.5 times the default cap):
 *   - Narrowing. When the next region's A part is a suffix of the indexed one (the after part of the
 *     region just searched, when the before part needed no index), the index drops the leading
 *     positions instead of being rebuilt: each dropped position is its line's earliest, so the
 *     line's earliest becomes its next position and its count falls by one. Counts are kept exact
 *     (and saturated only when read), so the narrowed index equals a rebuilt one.
 *   - Small regions. A region of at most kSmallRegion line pairs is first checked for a common line
 *     pair by direct comparison; with none it is a replace, as the full search would find, and the
 *     index stays untouched for the next region.
 *   - The run cache. The run grown from (A position i, B position j) is remembered per B position
 *     when its rarity is 1. A later region that tries the same (i, j) and contains the whole run
 *     reuses it: the regions whose B part holds j form a chain of nested regions, so the entry was
 *     stored by an enclosing region; a run maximal there and lying inside the smaller region is
 *     maximal there too, and its rarity stays 1 (counts only fall in a sub-region, and every line
 *     of the run occurs in it).
 *
 * Work: one unit per A position indexed or dropped, per B position walked, per A position tried,
 * per step of growing a run, per A position passed over inside a run just grown, and per line
 * pair compared by the small-region check. A reused run costs no growing steps.
 */

#include "histogram.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace frost::diff::detail {
namespace {

constexpr std::uint32_t kEnd = 0xFFFF'FFFFu; // No further position of the same line.
constexpr std::uint32_t kAnchorLimit = 64;   // A best rarity above this sends the region to Myers.
constexpr std::uint32_t kCountCeiling = 255; // Occurrence counts saturate here when read.
constexpr std::uint64_t kSmallRegion = 64;   // Line pairs up to which a region is checked directly.

/*
 * The occurrence index of one region's A part. For every line id seen in A[a0, a1) it holds the
 * earliest position, a link from each position to the next one of the same line (ascending) and
 * the exact occurrence count (count() saturates it at kCountCeiling). rebuild() makes one forward
 * pass and appends each position to its line's list through a per-line tail; narrow() drops the
 * leading positions when the next region's A part is a suffix of the indexed one. Entries of ids
 * absent from the current region are stale; a generation number per id tells them apart (0 marks
 * an id dropped by narrow()). Storage is allocated once per diff and reused by every region.
 */
class RegionIndex {
public:
    RegionIndex(std::uint32_t idCount, std::size_t aSize) noexcept
        : m_headOf(idCount), m_tailOf(idCount), m_generationOf(idCount, 0), m_countOf(idCount),
          m_successor(aSize, kEnd) {}

    // Indexes A[a0, a1) for the next region: narrows the current index when A[a0, a1) is a suffix
    // of it and dropping is cheaper than rebuilding (and `reuse` allows it), else rebuilds. One
    // unit per position indexed or dropped. Returns false when the budget ran out (the index is
    // then unusable until the next rebuild).
    [[nodiscard]] bool prepare(std::span<const std::uint32_t> a, std::uint32_t a0, std::uint32_t a1, bool reuse,
                               WorkBudget& budget) noexcept {
        if (reuse && m_valid && a1 == m_to && a0 >= m_from && a0 - m_from < a1 - a0) {
            return narrow(a, a0, budget);
        }
        return rebuild(a, a0, a1, budget);
    }

    // Whether line `id` occurs in the current region's A part. O(1).
    [[nodiscard]] bool holds(std::uint32_t id) const noexcept { return m_generationOf[id] == m_generation; }
    // Its occurrence count, saturated at kCountCeiling. Precondition: holds(id). O(1).
    [[nodiscard]] std::uint32_t count(std::uint32_t id) const noexcept {
        return std::min(m_countOf[id], kCountCeiling);
    }
    // Its earliest position. Precondition: holds(id). O(1).
    [[nodiscard]] std::uint32_t earliest(std::uint32_t id) const noexcept { return m_headOf[id]; }
    // The next position of the same line after `position` (inside the region), or kEnd. O(1).
    [[nodiscard]] std::uint32_t after(std::uint32_t position) const noexcept { return m_successor[position]; }

private:
    [[nodiscard]] bool rebuild(std::span<const std::uint32_t> a, std::uint32_t a0, std::uint32_t a1,
                               WorkBudget& budget) noexcept {
        m_valid = false;
        if (++m_generation == 0) {
            // 2^32 regions: the stale generations could alias; start over.
            std::ranges::fill(m_generationOf, 0u);
            m_generation = 1;
        }
        for (std::uint32_t position = a0; position < a1; ++position) {
            if (!budget.charge()) {
                return false;
            }
            const std::uint32_t id = a[position];
            m_successor[position] = kEnd;
            if (m_generationOf[id] == m_generation) {
                m_successor[m_tailOf[id]] = position;
                ++m_countOf[id];
            } else {
                m_generationOf[id] = m_generation;
                m_headOf[id] = position;
                m_countOf[id] = 1;
            }
            m_tailOf[id] = position;
        }
        m_from = a0;
        m_to = a1;
        m_valid = true;
        return true;
    }

    // Drops A[m_from, a0) from the front. Each dropped position is its line's earliest (the ones
    // before it were dropped already), so the line's earliest moves to the next position.
    [[nodiscard]] bool narrow(std::span<const std::uint32_t> a, std::uint32_t a0, WorkBudget& budget) noexcept {
        for (std::uint32_t position = m_from; position < a0; ++position) {
            if (!budget.charge()) {
                m_valid = false;
                return false;
            }
            const std::uint32_t id = a[position];
            FROST_ASSERT(m_generationOf[id] == m_generation && m_headOf[id] == position);
            const std::uint32_t next = m_successor[position];
            if (next == kEnd) {
                m_generationOf[id] = 0; // No occurrence left: absent from the narrowed region.
            } else {
                m_headOf[id] = next;
                --m_countOf[id];
            }
        }
        m_from = a0;
        return true;
    }

    std::vector<std::uint32_t> m_headOf;       // Per id: earliest position in the region's A part.
    std::vector<std::uint32_t> m_tailOf;       // Per id: latest position seen so far by rebuild().
    std::vector<std::uint32_t> m_generationOf; // Per id: the generation its entries belong to.
    std::vector<std::uint32_t> m_countOf;      // Per id: exact occurrences in the region's A part.
    std::vector<std::uint32_t> m_successor;    // Per A position: the next position of its line, or kEnd.
    std::uint32_t m_generation = 0;
    std::uint32_t m_from = 0; // The indexed A part, [m_from, m_to), while m_valid.
    std::uint32_t m_to = 0;
    bool m_valid = false;
};

/*
 * The runs of rarity 1 grown by earlier searches, one per B position (the last one grown from it).
 * find() returns the run grown from (i, j) when it lies inside `region`; by the argument in the
 * header comment it is then the maximal run through (i, j) inside `region`, of rarity 1. 12 bytes
 * per B line, allocated once per diff (only when `reuse`).
 */
class RunCache {
public:
    RunCache(std::size_t bSize, bool enabled) noexcept : m_entries(enabled ? bSize : 0) {}

    [[nodiscard]] std::optional<Region> find(std::uint32_t i, std::uint32_t j, const Region& region) const noexcept {
        if (m_entries.empty() || m_entries[j].i != i) {
            return std::nullopt;
        }
        const Entry& entry = m_entries[j];
        const Region run{i - (j - entry.b0), i + (entry.b1 - j), entry.b0, entry.b1};
        if (run.a0 < region.a0 || run.b0 < region.b0 || run.a1 > region.a1 || run.b1 > region.b1) {
            return std::nullopt;
        }
        return run;
    }

    // Remembers `run`, grown from (i, j) with rarity 1.
    void store(std::uint32_t i, std::uint32_t j, const Region& run) noexcept {
        if (!m_entries.empty()) {
            m_entries[j] = Entry{i, run.b0, run.b1};
        }
    }

private:
    struct Entry {
        std::uint32_t i = kEnd; // The A position the run was grown from; kEnd when none.
        std::uint32_t b0 = 0;
        std::uint32_t b1 = 0;
    };
    std::vector<Entry> m_entries;
};

// A common run tried as an anchor: A[span.a0, span.a1) equals B[span.b0, span.b1); rarity is the
// lowest occurrence count among its A lines.
struct Anchor {
    Region span;
    std::uint32_t rarity = 0;
};

enum class Outcome : std::uint8_t {
    Anchored,     // A best anchor with rarity <= kAnchorLimit.
    NoCommonLine, // No B line of the region occurs in its A part.
    TooFrequent,  // Common lines exist, but the best rarity stayed above kAnchorLimit.
    OutOfWork,    // The budget ran out.
};

// The anchor search of one region: walks B's part against the region's index (the rules in the
// header comment). One object per region; not reusable.
class AnchorSearch {
public:
    AnchorSearch(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, const RegionIndex& index,
                 RunCache& runs, const Region& region, WorkBudget& budget) noexcept
        : m_a(a), m_b(b), m_index(index), m_runs(runs), m_region(region), m_budget(budget) {}

    // Runs the walk; on Anchored, `anchor` receives the best anchor.
    [[nodiscard]] Outcome run(Anchor& anchor) noexcept {
        std::uint32_t j = m_region.b0;
        while (j < m_region.b1) {
            if (!m_budget.charge()) {
                return Outcome::OutOfWork;
            }
            const std::uint32_t line = m_b[j];
            std::uint32_t resume = j + 1;
            if (m_index.holds(line)) {
                m_sawCommon = true;
                if (m_index.count(line) <= m_threshold && !tryPositions(line, j, resume)) {
                    return Outcome::OutOfWork;
                }
            }
            j = resume;
        }
        if (!m_sawCommon) {
            return Outcome::NoCommonLine;
        }
        if (m_threshold > kAnchorLimit) {
            return Outcome::TooFrequent;
        }
        FROST_ASSERT(m_best.has_value()); // The threshold only drops when an anchor is kept.
        anchor = *m_best;
        return Outcome::Anchored;
    }

private:
    // Grows every A position of `line` (earliest first, skipping positions inside the run just
    // grown) against B position j; raises `resume` to the furthest B end. False: out of work.
    [[nodiscard]] bool tryPositions(std::uint32_t line, std::uint32_t j, std::uint32_t& resume) noexcept {
        std::uint32_t i = m_index.earliest(line);
        while (i != kEnd) {
            if (!m_budget.charge()) {
                return false;
            }
            Anchor candidate;
            if (!grow(i, j, candidate)) {
                return false;
            }
            resume = std::max(resume, candidate.span.b1);
            offer(candidate);
            std::uint32_t later = m_index.after(i);
            while (later != kEnd && later < candidate.span.a1) {
                if (!m_budget.charge()) {
                    return false;
                }
                later = m_index.after(later);
            }
            i = later;
        }
        return true;
    }

    // The maximal common run through A[i] == B[j] inside the region, with its rarity: from the run
    // cache when an enclosing region grew it, else grown here (one unit per line the run grows by).
    // False: out of work.
    [[nodiscard]] bool grow(std::uint32_t i, std::uint32_t j, Anchor& out) noexcept {
        if (const std::optional<Region> cached = m_runs.find(i, j, m_region)) {
            out = Anchor{*cached, 1};
            return true;
        }
        Region span{i, i + 1, j, j + 1};
        std::uint32_t rarity = m_index.count(m_a[i]);
        while (span.a0 > m_region.a0 && span.b0 > m_region.b0 && m_a[span.a0 - 1] == m_b[span.b0 - 1]) {
            if (!m_budget.charge()) {
                return false;
            }
            --span.a0;
            --span.b0;
            rarity = std::min(rarity, m_index.count(m_a[span.a0]));
        }
        while (span.a1 < m_region.a1 && span.b1 < m_region.b1 && m_a[span.a1] == m_b[span.b1]) {
            if (!m_budget.charge()) {
                return false;
            }
            rarity = std::min(rarity, m_index.count(m_a[span.a1]));
            ++span.a1;
            ++span.b1;
        }
        if (rarity == 1) {
            m_runs.store(i, j, span);
        }
        out = Anchor{span, rarity};
        return true;
    }

    // Keeps `candidate` when there is no best anchor yet, when it is longer on A's side, or when it
    // is rarer than the threshold; the threshold follows the kept anchor's rarity.
    void offer(const Anchor& candidate) noexcept {
        const std::uint32_t length = candidate.span.a1 - candidate.span.a0;
        const bool longer = !m_best || length > m_best->span.a1 - m_best->span.a0;
        if (longer || candidate.rarity < m_threshold) {
            m_best = candidate;
            m_threshold = candidate.rarity;
        }
    }

    std::span<const std::uint32_t> m_a;
    std::span<const std::uint32_t> m_b;
    const RegionIndex& m_index;
    RunCache& m_runs;
    Region m_region;
    WorkBudget& m_budget;
    std::optional<Anchor> m_best;
    std::uint32_t m_threshold = kAnchorLimit + 1;
    bool m_sawCommon = false;
};

// Whether some line of B[region] equals some line of A[region], by direct comparison; nullopt
// when the budget ran out (one unit per line pair compared). For regions of few line pairs.
[[nodiscard]] std::optional<bool> sharesLine(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b,
                                             const Region& region, WorkBudget& budget) noexcept {
    for (std::uint32_t j = region.b0; j < region.b1; ++j) {
        if (!budget.charge(region.a1 - region.a0)) {
            return std::nullopt;
        }
        for (std::uint32_t i = region.a0; i < region.a1; ++i) {
            if (a[i] == b[j]) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

HistogramResult histogramDiff(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b, std::uint32_t idCount,
                              std::span<std::uint8_t> changedA, std::span<std::uint8_t> changedB, WorkBudget& budget,
                              HistogramReuse reuse) noexcept {
    FROST_PRE(!a.empty() && !b.empty());
    FROST_PRE(changedA.size() == a.size() && changedB.size() == b.size());
    const bool reusing = reuse == HistogramReuse::On;
    HistogramResult result;
    RegionIndex index(idCount, a.size());
    RunCache runs(b.size(), reusing);
    std::vector<Region> pending;
    pending.push_back(Region{0, static_cast<std::uint32_t>(a.size()), 0, static_cast<std::uint32_t>(b.size())});
    // Out of work: the region in hand and every pending one become whole replaces. Valid, because
    // the regions are disjoint and the runs between them are true matches.
    const auto giveUp = [&](const Region& inHand) {
        markReplace(inHand, changedA, changedB);
        for (const Region& region : pending) {
            markReplace(region, changedA, changedB);
        }
        pending.clear();
        result.exhausted = true;
    };
    bool whole = true; // The whole input is always searched, whatever its shape.
    while (!pending.empty()) {
        const Region region = pending.back();
        pending.pop_back();
        if (!std::exchange(whole, false)) {
            const std::uint32_t linesA = region.a1 - region.a0;
            const std::uint32_t linesB = region.b1 - region.b0;
            if (linesA == 0 && linesB == 0) {
                continue;
            }
            if (linesA == 0 || linesB == 0 || (linesA == 1 && linesB == 1)) {
                markReplace(region, changedA, changedB);
                continue;
            }
            if (reusing && std::uint64_t{linesA} * linesB <= kSmallRegion) {
                const std::optional<bool> shared = sharesLine(a, b, region, budget);
                if (!shared) {
                    giveUp(region);
                    break;
                }
                if (!*shared) {
                    markReplace(region, changedA, changedB); // What the search would find: no common line.
                    continue;
                }
            }
        }
        if (!index.prepare(a, region.a0, region.a1, reusing, budget)) {
            giveUp(region);
            break;
        }
        AnchorSearch search(a, b, index, runs, region, budget);
        Anchor anchor;
        switch (search.run(anchor)) {
        case Outcome::OutOfWork: giveUp(region); break;
        case Outcome::NoCommonLine: markReplace(region, changedA, changedB); break;
        case Outcome::TooFrequent:
            ++result.fallbacks;
            if (!myersDiff(a, b, region, changedA, changedB, budget)) {
                giveUp(Region{}); // Myers marked its own unfinished parts.
            }
            break;
        case Outcome::Anchored:
            // After first, so that the part before the anchor is handled next.
            pending.push_back(Region{anchor.span.a1, region.a1, anchor.span.b1, region.b1});
            pending.push_back(Region{region.a0, anchor.span.a0, region.b0, anchor.span.b0});
            break;
        }
    }
    return result;
}

} // namespace frost::diff::detail
