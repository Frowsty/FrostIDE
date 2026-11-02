// SPDX-License-Identifier: Apache-2.0
//
// refineHunk (frost/diff/refine.hpp): tokenising, prefix/suffix trimming, the token-level Myers diff
// and the slide rule (ADR 0019, D8; our own formulation).

#include "frost/diff/refine.hpp"

#include "frost/core/assert.hpp"

#include "engine.hpp"
#include "fnv.hpp"
#include "interner.hpp"
#include "myers.hpp"
#include "work_budget.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace frost::diff {
namespace {

bool isWordByte(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

bool isBlank(unsigned char c) noexcept { return c == ' ' || c == '\t'; }

// The length of the well-formed UTF-8 sequence at s[i] (Unicode table 3-7), or 0 if none starts there.
std::size_t utf8Length(std::string_view s, std::size_t i) noexcept {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = at(i);
    if (lead < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        if (lead == 0xE0) {
            lo = 0xA0;
        } else if (lead == 0xED) {
            hi = 0x9F;
        }
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        if (lead == 0xF0) {
            lo = 0x90;
        } else if (lead == 0xF4) {
            hi = 0x8F;
        }
    } else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    if (at(i + 1) < lo || at(i + 1) > hi) {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
        if (at(i + k) < 0x80 || at(i + k) > 0xBF) {
            return 0;
        }
    }
    return length;
}

// The end of the token that starts at s[i]. Precondition: i < s.size() and a token starts there.
std::size_t tokenEnd(std::string_view s, std::size_t i, Granularity granularity) noexcept {
    const std::size_t n = s.size();
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '\r') {
        return (i + 1 < n && s[i + 1] == '\n') ? i + 2 : i + 1;
    }
    if (granularity == Granularity::Char) {
        const std::size_t length = utf8Length(s, i);
        return i + (length == 0 ? 1 : length);
    }
    if (isWordByte(c)) {
        while (i < n && isWordByte(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        return i;
    }
    if (isBlank(c)) {
        while (i < n && isBlank(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        return i;
    }
    return i + 1;
}

// The start offset of every token of s[from, s.size()), plus s.size() at the end. Precondition: a
// token starts at s[from].
std::vector<std::uint32_t> tokenize(std::string_view s, Granularity granularity, std::size_t from = 0) noexcept {
    std::vector<std::uint32_t> starts;
    for (std::size_t i = from; i < s.size(); i = tokenEnd(s, i, granularity)) {
        starts.push_back(static_cast<std::uint32_t>(i));
    }
    starts.push_back(static_cast<std::uint32_t>(s.size()));
    return starts;
}

std::string_view token(std::string_view s, const std::vector<std::uint32_t>& starts, std::size_t t) noexcept {
    return s.substr(starts[t], starts[t + 1] - starts[t]);
}

text::ByteRange shifted(text::ByteRange base, std::uint32_t begin, std::uint32_t end) noexcept {
    return text::ByteRange{base.begin + begin, base.begin + end};
}

/*
 * Whether a token surely starts at s[i], judged from the bytes s[lo, i] alone (the caller passes
 * the lowest offset whose bytes it trusts, for example the first byte of a common suffix): never
 * at an LF after a CR; for Word, wherever s[i - 1] and s[i] are not both word bytes or both
 * blanks; for Char, wherever s[i] is no UTF-8 continuation byte (0x80-0xBF), and at a
 * continuation byte whose three predecessors, all at or after lo, hold no lead byte (0xC2-0xF4):
 * a token that covers s[i] from further left is a sequence led from at most three bytes before,
 * so a run of continuation bytes (one token each) has certain starts too. False where more
 * context would be needed. Precondition: lo < i < s.size().
 */
bool certainStart(std::string_view s, std::size_t i, Granularity granularity, std::size_t lo) noexcept {
    const auto previous = static_cast<unsigned char>(s[i - 1]);
    const auto c = static_cast<unsigned char>(s[i]);
    if (previous == '\r' && c == '\n') {
        return false;
    }
    if (granularity == Granularity::Char) {
        if (c < 0x80 || c > 0xBF) {
            return true;
        }
        if (i - lo < 3) {
            return false;
        }
        for (std::size_t back = 1; back <= 3; ++back) {
            const auto before = static_cast<unsigned char>(s[i - back]);
            if (before >= 0xC2 && before <= 0xF4) {
                return false;
            }
        }
        return true;
    }
    return !((isWordByte(previous) && isWordByte(c)) || (isBlank(previous) && isBlank(c)));
}

// The length of the common prefix of x and y; memcmp over 4 KiB blocks, then bytes.
std::size_t commonPrefix(std::string_view x, std::string_view y) noexcept {
    constexpr std::size_t kBlock = 4096;
    const std::size_t n = std::min(x.size(), y.size());
    std::size_t i = 0;
    while (n - i >= kBlock && std::memcmp(x.data() + i, y.data() + i, kBlock) == 0) {
        i += kBlock;
    }
    while (i < n && x[i] == y[i]) {
        ++i;
    }
    return i;
}

// The length of the common suffix of x and y, at most `limit`; as commonPrefix.
std::size_t commonSuffix(std::string_view x, std::string_view y, std::size_t limit) noexcept {
    constexpr std::size_t kBlock = 4096;
    std::size_t k = 0;
    while (limit - k >= kBlock &&
           std::memcmp(x.data() + x.size() - k - kBlock, y.data() + y.size() - k - kBlock, kBlock) == 0) {
        k += kBlock;
    }
    while (k < limit && x[x.size() - 1 - k] == y[y.size() - 1 - k]) {
        ++k;
    }
    return k;
}

/*
 * The token middle of window strings sa and sb, found by tokenising only near its two edges (the
 * coarse case: a byte middle larger than maxBytes, so the token diff would not run anyway).
 * Returns {P, S}: the common token prefix ends at P on both sides, and the common token suffix
 * holds S bytes on both sides, with the full scan's clamp (the suffix never reaches back into the
 * prefix). `suffixBound` is an upper bound on S (see the caller).
 *
 * P: tokens are compared in lockstep from offset 0, which is a token start; this stops at the
 * first pair that differs, so it reads the prefix tokens plus one token per side.
 * S: on each side, tokenising starts at the latest certain token start (or P, itself a start on
 * both sides) at or before size - suffixBound, and the tokens are compared pairwise from the end.
 * Every token start at or after P lies in that range for the suffix S can reach, so the
 * comparison sees exactly the pairs the full scan compares; running out of scanned tokens on a
 * side means either that side is exhausted down to P (the clamp) or S has reached suffixBound.
 */
struct Edges {
    std::size_t prefixEnd = 0;   // P, a window offset on both sides.
    std::size_t suffixBytes = 0; // S.
};

Edges middleEdges(std::string_view sa, std::string_view sb, Granularity granularity, std::size_t suffixBound) noexcept {
    Edges edges;
    std::size_t& p = edges.prefixEnd;
    while (p < sa.size() && p < sb.size()) {
        const std::size_t endA = tokenEnd(sa, p, granularity);
        const std::size_t endB = tokenEnd(sb, p, granularity);
        if (endA != endB || sa.substr(p, endA - p) != sb.substr(p, endB - p)) {
            break;
        }
        p = endA;
    }
    const std::size_t bound = std::min({suffixBound, sa.size() - p, sb.size() - p});
    const auto scanFrom = [&](std::string_view s) {
        std::size_t q = s.size() - bound;
        while (q > p && q < s.size() && !certainStart(s, q, granularity, 0)) {
            --q;
        }
        return tokenize(s, granularity, q);
    };
    const std::vector<std::uint32_t> startsA = scanFrom(sa);
    const std::vector<std::uint32_t> startsB = scanFrom(sb);
    std::size_t ka = startsA.size() - 1; // Scanned tokens not yet matched, per side.
    std::size_t kb = startsB.size() - 1;
    while (ka > 0 && kb > 0 && token(sa, startsA, ka - 1) == token(sb, startsB, kb - 1)) {
        edges.suffixBytes += startsA[ka] - startsA[ka - 1];
        --ka;
        --kb;
    }
    return edges;
}

} // namespace

Result<Refinement> refineHunk(const LineSequence& a, const LineSequence& b, const Hunk& hunk, Granularity granularity,
                              const RefineLimits& limits, std::stop_token stop) noexcept {
    return detail::refineHunkWith(a, b, hunk, granularity, limits, std::move(stop), detail::RefineScan::Window);
}

Result<Refinement> detail::refineHunkWith(const LineSequence& a, const LineSequence& b, const Hunk& hunk,
                                          Granularity granularity, const RefineLimits& limits, std::stop_token stop,
                                          RefineScan scan) noexcept {
    FROST_PRE(a.split() == b.split());
    FROST_PRE(hunk.a.begin <= hunk.a.end && hunk.a.end.value() <= a.size());
    FROST_PRE(hunk.b.begin <= hunk.b.end && hunk.b.end.value() <= b.size());
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "refinement cancelled");
    }
    const text::ByteRange rangeA = a.byteRange(hunk.a);
    const text::ByteRange rangeB = b.byteRange(hunk.b);
    Refinement out;
    if (hunk.a.empty() || hunk.b.empty()) {
        out.changes.push_back(RangePair{rangeA, rangeB});
        return out;
    }
    if (rangeA.size() > limits.maxScanBytes || rangeB.size() > limits.maxScanBytes) {
        out.changes.push_back(RangePair{rangeA, rangeB});
        out.coarse = true;
        return out;
    }
    const std::string_view fullA = a.bytes().substr(rangeA.begin.value(), rangeA.size());
    const std::string_view fullB = b.bytes().substr(rangeB.begin.value(), rangeB.size());
    /*
     * The window: only the bytes from a certain token start before the first differing byte to one
     * after the last differing byte are tokenised. Tokenising never reads past the next certain
     * start to decide a token, so the tokens before the window start (from the window end on) are
     * made of, and decided by, common prefix (suffix) bytes: the same tokens on both sides, which
     * the token trimming below would remove anyway. The window holds the remaining tokens of each
     * side unchanged, so trimming inside it gives the same middle as trimming whole sides
     * (Refine.WindowEqualsFullScan checks this against RefineScan::FullScan). An 8 MB line with
     * one change then costs a memcmp (Budget: B21, P6), and so does a line whose common parts are
     * runs of continuation bytes, one token per byte (RefinePerf.ContinuationRuns).
     */
    std::size_t windowBegin = 0;
    std::size_t windowEndA = fullA.size();
    std::size_t windowEndB = fullB.size();
    bool certainlyCoarse = false;
    std::size_t suffixBound = std::numeric_limits<std::size_t>::max();
    if (scan == RefineScan::Window) {
        const std::size_t prefixBytes = commonPrefix(fullA, fullB);
        const std::size_t suffixBytes = commonSuffix(fullA, fullB, std::min(fullA.size(), fullB.size()) - prefixBytes);
        // Starts judged from common bytes only: below the prefix's end, and from the suffix's first
        // byte on (certainStart's lo), so a start found on A is one on B.
        windowBegin = prefixBytes == 0 ? 0 : prefixBytes - 1;
        while (windowBegin > 0 && !certainStart(fullA, windowBegin, granularity, 0)) {
            --windowBegin;
        }
        std::size_t tail = suffixBytes == 0 ? 0 : suffixBytes - 1; // Bytes after the window.
        while (tail > 0 && !certainStart(fullA, fullA.size() - tail, granularity, fullA.size() - suffixBytes)) {
            --tail;
        }
        windowEndA = fullA.size() - tail;
        windowEndB = fullB.size() - tail;
        /*
         * The size guard before any tokenising. Common tokens are made of common bytes, so the
         * token middle of each side contains its byte middle (what the common byte prefix and
         * suffix leave): when a byte middle exceeds maxBytes, so does that token middle, and the
         * result is the coarse pair (or, if the other side's token middle is empty, the pure
         * insertion or deletion), which needs only the middle's edges (middleEdges). With both
         * byte middles non-empty the byte suffix was not clamped, so it bounds the token suffix;
         * otherwise the full scan's clamp does. A line differing near both ends then costs two
         * memcmp passes and a few tokens (Budget: B21, P8).
         */
        const std::size_t middleBytesA = fullA.size() - prefixBytes - suffixBytes;
        const std::size_t middleBytesB = fullB.size() - prefixBytes - suffixBytes;
        certainlyCoarse = middleBytesA > limits.maxBytes || middleBytesB > limits.maxBytes;
        if (middleBytesA > 0 && middleBytesB > 0) {
            suffixBound = suffixBytes - tail;
        }
    }
    const std::string_view sa = fullA.substr(windowBegin, windowEndA - windowBegin);
    const std::string_view sb = fullB.substr(windowBegin, windowEndB - windowBegin);
    const text::ByteRange windowA =
        shifted(rangeA, static_cast<std::uint32_t>(windowBegin), static_cast<std::uint32_t>(windowEndA));
    const text::ByteRange windowB =
        shifted(rangeB, static_cast<std::uint32_t>(windowBegin), static_cast<std::uint32_t>(windowEndB));
    if (certainlyCoarse) {
        const Edges edges = middleEdges(sa, sb, granularity, suffixBound);
        const auto from = static_cast<std::uint32_t>(edges.prefixEnd);
        const text::ByteRange middleA =
            shifted(windowA, from, static_cast<std::uint32_t>(sa.size() - edges.suffixBytes));
        const text::ByteRange middleB =
            shifted(windowB, from, static_cast<std::uint32_t>(sb.size() - edges.suffixBytes));
        FROST_ASSERT(middleA.size() > 0 || middleB.size() > 0); // One byte middle exceeds maxBytes >= 0.
        out.changes.push_back(RangePair{middleA, middleB});
        out.coarse = middleA.size() > 0 && middleB.size() > 0;
        return out;
    }
    const std::vector<std::uint32_t> startsA = tokenize(sa, granularity);
    const std::vector<std::uint32_t> startsB = tokenize(sb, granularity);
    const std::size_t ta = startsA.size() - 1;
    const std::size_t tb = startsB.size() - 1;
    std::size_t prefix = 0;
    while (prefix < ta && prefix < tb && token(sa, startsA, prefix) == token(sb, startsB, prefix)) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < ta - prefix && suffix < tb - prefix &&
           token(sa, startsA, ta - 1 - suffix) == token(sb, startsB, tb - 1 - suffix)) {
        ++suffix;
    }
    const std::size_t na = ta - prefix - suffix; // Middle tokens.
    const std::size_t nb = tb - prefix - suffix;
    const text::ByteRange middleA = shifted(windowA, startsA[prefix], startsA[ta - suffix]);
    const text::ByteRange middleB = shifted(windowB, startsB[prefix], startsB[tb - suffix]);
    if (na == 0 && nb == 0) {
        return out; // Equal sides (possible in a degraded diff).
    }
    if (na == 0 || nb == 0) {
        out.changes.push_back(RangePair{middleA, middleB});
        return out;
    }
    const auto coarseMiddle = [&]() -> Result<Refinement> {
        Refinement coarse;
        coarse.changes.push_back(RangePair{middleA, middleB});
        coarse.coarse = true;
        return coarse;
    };
    if (middleA.size() > limits.maxBytes || middleB.size() > limits.maxBytes) {
        return coarseMiddle();
    }

    detail::WorkBudget budget(limits.maxWork, stop);
    // Tokens are short and their table stays small (at most 2 * maxBytes keys), so a token costs
    // one unit, not the line rate.
    detail::Interner interner(na + nb, 1);
    std::array<std::vector<std::uint32_t>, 2> ids;
    ids[0].resize(na);
    ids[1].resize(nb);
    bool interned = true;
    for (std::size_t t = 0; t < na && interned; ++t) {
        const std::string_view bytes = token(sa, startsA, prefix + t);
        ids[0][t] = interner.intern(bytes, detail::fnv1a64(bytes), budget);
        interned = ids[0][t] != detail::Interner::kExhausted;
    }
    for (std::size_t t = 0; t < nb && interned; ++t) {
        const std::string_view bytes = token(sb, startsB, prefix + t);
        ids[1][t] = interner.intern(bytes, detail::fnv1a64(bytes), budget);
        interned = ids[1][t] != detail::Interner::kExhausted;
    }
    if (budget.cancelled()) {
        return fail(ErrorCode::Cancelled, "refinement cancelled");
    }
    if (!interned) {
        return coarseMiddle();
    }
    std::vector<std::uint8_t> changedA(na, 0);
    std::vector<std::uint8_t> changedB(nb, 0);
    const detail::Region whole{0, static_cast<std::uint32_t>(na), 0, static_cast<std::uint32_t>(nb)};
    const bool complete = detail::myersDiff(ids[0], ids[1], whole, changedA, changedB, budget);
    if (budget.cancelled()) {
        return fail(ErrorCode::Cancelled, "refinement cancelled");
    }
    if (!complete) {
        return coarseMiddle();
    }

    // Slide each pure deletion or insertion run down while the token after it equals its first
    // token, keeping at least one unchanged token before the next change.
    std::vector<Hunk> runs = detail::hunksFromFlags(changedA, changedB);
    for (std::size_t r = 0; r < runs.size(); ++r) {
        Hunk& run = runs[r];
        const bool deletion = run.b.empty();
        const bool insertion = run.a.empty();
        if (!deletion && !insertion) {
            continue;
        }
        const std::vector<std::uint32_t>& side = deletion ? ids[0] : ids[1];
        const LineRange& moving = deletion ? run.a : run.b;
        // The run's end may advance while it stays below `limit`: the side's end for the last run,
        // else one token before the next run, so an unchanged token always separates them.
        const std::size_t limit = r + 1 < runs.size()
                                      ? (deletion ? runs[r + 1].a.begin.value() : runs[r + 1].b.begin.value()) - 1
                                      : (deletion ? na : nb);
        while (moving.end.value() < limit && side[moving.end.value()] == side[moving.begin.value()]) {
            if (!budget.charge()) {
                if (budget.cancelled()) {
                    return fail(ErrorCode::Cancelled, "refinement cancelled");
                }
                return coarseMiddle();
            }
            // Both sides move by one: the run on its side, the empty insertion point on the other.
            ++run.a.begin;
            ++run.a.end;
            ++run.b.begin;
            ++run.b.end;
        }
    }
    for (const Hunk& run : runs) {
        const auto tokenA = [&](LineIndex t) { return startsA[prefix + t.value()]; };
        const auto tokenB = [&](LineIndex t) { return startsB[prefix + t.value()]; };
        out.changes.push_back(RangePair{shifted(windowA, tokenA(run.a.begin), tokenA(run.a.end)),
                                        shifted(windowB, tokenB(run.b.begin), tokenB(run.b.end))});
    }
    return out;
}

} // namespace frost::diff
