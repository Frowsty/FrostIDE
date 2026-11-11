// SPDX-License-Identifier: Apache-2.0
//
// The frost::diff input generators (generators.hpp). The pair classes reproduce the distributions of
// the black-box experiments recorded in ADR 0019 ("Measurements"), drawn from SplitMix64 instead of
// Python's Random, so the cases differ but the shapes match.

#include "diff/support/generators.hpp"

#include <algorithm>
#include <format>
#include <numeric>
#include <utility>

namespace frost::diff::test {
namespace {

using Lines = std::vector<std::string>;

std::uint64_t below(SplitMix64& rng, std::uint64_t bound) { return bound == 0 ? 0 : rng.below(bound); }

// A value in [lo, hi].
std::uint32_t between(SplitMix64& rng, std::uint32_t lo, std::uint32_t hi) {
    return lo + static_cast<std::uint32_t>(below(rng, std::uint64_t{hi} - lo + 1));
}

// A uniform double in [0, 1).
double unit(SplitMix64& rng) { return static_cast<double>(rng.next() >> 11) * 0x1.0p-53; }

std::string join(const Lines& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
    }
    return out;
}

// Applies `edits` random deletes, inserts (from `fresh`) and replaces to `lines`.
template<class Fresh, class Replace>
void mutate(Lines& lines, SplitMix64& rng, std::uint32_t edits, Fresh fresh, Replace replace) {
    for (std::uint32_t e = 0; e < edits; ++e) {
        const std::uint64_t op = below(rng, 3);
        if (op == 0 && !lines.empty()) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(below(rng, lines.size())));
        } else if (op == 1) {
            const std::uint64_t at = below(rng, lines.size() + 1);
            std::string line = fresh(); // A separate statement: one draw order on every compiler.
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
        } else if (!lines.empty()) {
            std::string line = replace(); // Drawn before the index, as C++17 orders E1 = E2.
            lines[below(rng, lines.size())] = std::move(line);
        }
    }
}

std::string codeLine(SplitMix64& rng) {
    const double r = unit(rng);
    if (r < 0.15) {
        return "}\n";
    }
    if (r < 0.25) {
        return "\n";
    }
    if (r < 0.30) {
        return "    return 0;\n";
    }
    if (r < 0.33) {
        return "{\n";
    }
    // Function arguments are evaluated in an unspecified order, so the draws come first.
    const std::uint64_t name = below(rng, 400);
    const std::uint64_t value = below(rng, 50);
    return std::format("    int v{} = f({});\n", name, value);
}

// Code-like edits: deletes, inserts, replaces and moves of blocks of up to 8 lines.
void mutateCode(Lines& lines, SplitMix64& rng, std::uint32_t edits) {
    for (std::uint32_t e = 0; e < edits; ++e) {
        const std::uint64_t op = below(rng, 4);
        if (op == 0 && !lines.empty()) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(below(rng, lines.size())));
        } else if (op == 1) {
            const std::uint64_t at = below(rng, lines.size() + 1);
            std::string line = codeLine(rng);
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
        } else if (op == 2 && !lines.empty()) {
            std::string line = codeLine(rng);
            lines[below(rng, lines.size())] = std::move(line);
        } else if (!lines.empty()) {
            const std::size_t i = below(rng, lines.size());
            const std::size_t j = std::min(lines.size(), i + between(rng, 1, 8));
            Lines block(lines.begin() + static_cast<std::ptrdiff_t>(i), lines.begin() + static_cast<std::ptrdiff_t>(j));
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i), lines.begin() + static_cast<std::ptrdiff_t>(j));
            const std::size_t k = below(rng, lines.size() + 1);
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(k), block.begin(), block.end());
        }
    }
}

std::string loneCrLine(SplitMix64& rng, bool allowInvalid) {
    static constexpr std::string_view kPool[] = {"a\r",     "b\r\n", "\r",    "c\n", "d\re\n", "\xC3\xA9\n",
                                                 "x\r\r\n", "\n",    "f\r\n", "g\n", "\r\n",   "h"};
    if (allowInvalid && below(rng, 40) == 0) {
        return "\xFF\n";
    }
    return std::string(kPool[below(rng, std::size(kPool))]);
}

// Drops the final LF of `text` with probability `p` (no-final-newline cases).
void maybeDropFinalNewline(std::string& text, SplitMix64& rng, double p) {
    if (!text.empty() && text.back() == '\n' && unit(rng) < p) {
        text.pop_back();
    }
}

} // namespace

std::string_view pairClassName(PairClass kind) noexcept {
    switch (kind) {
    case PairClass::Adversarial: return "Adversarial";
    case PairClass::MidFrequency: return "MidFrequency";
    case PairClass::SparseFallback: return "SparseFallback";
    case PairClass::DenseFallback: return "DenseFallback";
    case PairClass::CodeLike: return "CodeLike";
    case PairClass::LoneCr: return "LoneCr";
    }
    return "?";
}

namespace {

// A class's line source: the base text's lines and a mutation that edits a copy.
struct ClassSource {
    PairClass kind;
    bool large;
    std::uint32_t alphabet = 2; // Adversarial only.
    bool invalid = false;       // LoneCr only: allow the odd invalid byte.
};

ClassSource makeSource(PairClass kind, SplitMix64& rng, SizeClass size) {
    static constexpr std::uint32_t kAlphabets[] = {2, 3, 4, 6, 12};
    ClassSource source{kind, size == SizeClass::Large};
    source.alphabet = kAlphabets[below(rng, std::size(kAlphabets))];
    source.invalid = below(rng, 20) == 0;
    return source;
}

Lines baseLines(const ClassSource& c, SplitMix64& rng) {
    Lines a;
    const bool large = c.large;
    switch (c.kind) {
    case PairClass::Adversarial: {
        const std::uint32_t n = large ? between(rng, 0, 2000) : between(rng, 0, 30);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.push_back(std::format("L{}\n", below(rng, c.alphabet)));
        }
        break;
    }
    case PairClass::MidFrequency: {
        const std::uint32_t n = large ? between(rng, 500, 2000) : between(rng, 70, 160);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.push_back(unit(rng) < 0.9 ? std::string("x\n") : std::format("y{}\n", below(rng, 5)));
        }
        break;
    }
    case PairClass::SparseFallback: {
        const std::uint32_t n = large ? between(rng, 500, 2000) : between(rng, 70, 160);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.emplace_back(unit(rng) < 0.97 ? "x\n" : "z\n");
        }
        break;
    }
    case PairClass::DenseFallback: {
        const std::uint32_t n = large ? between(rng, 500, 2000) : between(rng, 140, 200);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.emplace_back(unit(rng) < 0.5 ? "x\n" : "y\n");
        }
        break;
    }
    case PairClass::CodeLike: {
        const std::uint32_t n = large ? between(rng, 2000, 10000) : between(rng, 0, 300);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.push_back(codeLine(rng));
        }
        break;
    }
    case PairClass::LoneCr: {
        const std::uint32_t n = large ? between(rng, 0, 2000) : between(rng, 0, 60);
        for (std::uint32_t i = 0; i < n; ++i) {
            a.push_back(loneCrLine(rng, c.invalid));
        }
        break;
    }
    }
    return a;
}

void mutateLines(const ClassSource& c, Lines& b, SplitMix64& rng) {
    const bool large = c.large;
    switch (c.kind) {
    case PairClass::Adversarial: {
        const auto line = [&] { return std::format("L{}\n", below(rng, c.alphabet)); };
        mutate(b, rng, large ? between(rng, 0, 60) : between(rng, 0, 6), line, line);
        break;
    }
    case PairClass::MidFrequency: {
        const auto rare = [&] { return std::format("y{}\n", below(rng, 5)); };
        mutate(
            b, rng, large ? between(rng, 1, 60) : between(rng, 1, 8),
            [&] { return unit(rng) < 0.5 ? std::string("x\n") : rare(); }, rare);
        break;
    }
    case PairClass::SparseFallback:
        mutate(
            b, rng, large ? between(rng, 1, 60) : between(rng, 1, 8),
            [&] { return std::string(unit(rng) < 0.7 ? "x\n" : "z\n"); }, [] { return std::string("z\n"); });
        break;
    case PairClass::DenseFallback: {
        const auto line = [&] { return std::string(unit(rng) < 0.5 ? "x\n" : "y\n"); };
        mutate(b, rng, large ? between(rng, 1, 60) : between(rng, 1, 8), line, line);
        break;
    }
    case PairClass::CodeLike: mutateCode(b, rng, large ? between(rng, 0, 100) : between(rng, 0, 12)); break;
    case PairClass::LoneCr: {
        const auto line = [&] { return loneCrLine(rng, c.invalid); };
        mutate(b, rng, large ? between(rng, 0, 60) : between(rng, 0, 8), line, line);
        break;
    }
    }
}

} // namespace

TextPair generatePair(PairClass kind, SplitMix64& rng, SizeClass size) {
    const ClassSource source = makeSource(kind, rng, size);
    const Lines a = baseLines(source, rng);
    Lines b = a;
    mutateLines(source, b, rng);
    TextPair pair{join(a), join(b)};
    maybeDropFinalNewline(pair.a, rng, 0.1);
    maybeDropFinalNewline(pair.b, rng, 0.1);
    return pair;
}

TextPair codeLikePair(SplitMix64& rng, std::uint32_t lines, std::uint32_t edits) {
    Lines a;
    a.reserve(lines);
    for (std::uint32_t i = 0; i < lines; ++i) {
        a.push_back(codeLine(rng));
    }
    Lines b = a;
    mutateCode(b, rng, edits);
    return TextPair{join(a), join(b)};
}

std::string_view pathologyName(Pathology kind) noexcept {
    switch (kind) {
    case Pathology::P1: return "P1";
    case Pathology::P2: return "P2";
    case Pathology::P3: return "P3";
    case Pathology::P4: return "P4";
    case Pathology::P5: return "P5";
    case Pathology::P6: return "P6";
    case Pathology::P7: return "P7";
    }
    return "?";
}

TextPair pathologicalPair(Pathology kind, SplitMix64& rng, std::uint32_t lines) {
    Lines a;
    Lines b;
    switch (kind) {
    case Pathology::P1:
        for (std::uint32_t i = 0; i < lines; ++i) {
            a.push_back(std::format("unique line {}\n", i));
        }
        b.assign(a.rbegin(), a.rend());
        break;
    case Pathology::P2: {
        for (std::uint32_t i = 0; i < lines; ++i) {
            a.emplace_back(below(rng, 2) == 0 ? "x\n" : "y\n");
        }
        b = a;
        const auto line = [&] { return std::string(below(rng, 2) == 0 ? "x\n" : "y\n"); };
        mutate(b, rng, std::max<std::uint32_t>(1, lines / 20), line, line);
        break;
    }
    case Pathology::P3:
        for (std::uint32_t i = 0; i < lines; ++i) {
            a.push_back(std::format("old {}\n", i));
            b.push_back(std::format("new {}\n", i));
        }
        break;
    case Pathology::P4: {
        a.assign(lines, "same\n");
        b = a;
        for (int k = 0; k < 10; ++k) {
            b.insert(b.begin() + static_cast<std::ptrdiff_t>(below(rng, b.size() + 1)), "same\n");
        }
        break;
    }
    case Pathology::P5: {
        const std::uint32_t blockLines = std::max<std::uint32_t>(1, lines / 100);
        for (std::uint32_t r = 0; r < 100; ++r) {
            for (std::uint32_t i = 0; i < blockLines; ++i) {
                a.push_back(std::format("block line {}\n", i));
            }
        }
        // Each repetition of the block is shuffled differently.
        b = a;
        for (std::uint32_t r = 0; r < 100; ++r) {
            const auto first = b.begin() + static_cast<std::ptrdiff_t>(std::size_t{r} * blockLines);
            for (std::uint32_t i = blockLines; i > 1; --i) {
                std::swap(first[i - 1], first[static_cast<std::ptrdiff_t>(below(rng, i))]);
            }
        }
        break;
    }
    case Pathology::P6:
    case Pathology::P7: {
        // `lines` is the line's size in KiB here.
        const std::size_t bytes = std::max<std::size_t>(std::size_t{lines} << 10, 64);
        std::string text;
        text.reserve(bytes + 16);
        while (text.size() < bytes) {
            text += std::format("w{} ", below(rng, 100000));
        }
        text += '\n';
        std::string changed = text;
        const auto replaceWordAt = [&](std::size_t at) {
            const std::size_t start = changed.rfind(' ', at);
            const std::size_t begin = start == std::string::npos ? 0 : start + 1;
            const std::size_t end = changed.find(' ', begin);
            changed.replace(begin, end - begin, "CHANGED");
        };
        if (kind == Pathology::P6) {
            replaceWordAt(bytes / 2);
        } else {
            replaceWordAt(bytes / 2);
            replaceWordAt(bytes / 2 + bytes / 8);
        }
        return TextPair{std::move(text), std::move(changed)};
    }
    }
    return TextPair{join(a), join(b)};
}

ConstructiveMerge constructiveMerge(SplitMix64& rng, SizeClass size) {
    const std::uint32_t n = size == SizeClass::Large ? between(rng, 0, 400) : between(rng, 0, 40);
    struct Region {
        int side = 0; // 0 ours, 1 theirs.
        std::uint32_t s = 0;
        std::uint32_t e = 0;
        Lines lines;
    };
    std::uint32_t fresh = 0;
    std::vector<Region> regions[2];
    for (int side = 0; side < 2; ++side) {
        const double density = 0.05 + 0.3 * unit(rng);
        std::uint32_t pos = 0;
        while (pos <= n) {
            if (unit(rng) >= density) {
                ++pos;
                continue;
            }
            Region region;
            region.side = side;
            region.s = pos;
            const std::uint64_t op = pos < n ? below(rng, 3) : 1; // 0 delete, 1 insert, 2 replace.
            region.e = op == 1 ? pos : std::min(n, pos + between(rng, 1, 3));
            if (op != 0) {
                const std::uint32_t count = between(rng, 1, 3);
                for (std::uint32_t k = 0; k < count; ++k) {
                    region.lines.push_back(std::format("{}{}\n", side == 0 ? 'o' : 't', fresh++));
                }
            }
            pos = region.e + 1; // One kept base line before this side's next edit.
            regions[side].push_back(std::move(region));
        }
    }
    // Builds a side's text and, per base position p, the side's line index before (pre) and after
    // (post) the side's insertion at p.
    const auto build = [&](int side, std::vector<std::uint32_t>& pre, std::vector<std::uint32_t>& post) {
        Lines out;
        pre.assign(n + 1, 0);
        post.assign(n + 1, 0);
        std::size_t r = 0;
        const std::vector<Region>& own = regions[side];
        for (std::uint32_t p = 0; p <= n; ++p) {
            pre[p] = static_cast<std::uint32_t>(out.size());
            if (r < own.size() && own[r].s == p && own[r].e == p) {
                out.insert(out.end(), own[r].lines.begin(), own[r].lines.end());
                ++r;
            }
            post[p] = static_cast<std::uint32_t>(out.size());
            if (p == n) {
                break;
            }
            if (r < own.size() && own[r].s <= p && p < own[r].e) {
                if (own[r].s == p) {
                    out.insert(out.end(), own[r].lines.begin(), own[r].lines.end());
                }
                if (p + 1 == own[r].e) {
                    ++r;
                }
                continue; // Deleted or replaced.
            }
            out.push_back(std::format("b{}\n", p));
        }
        return out;
    };
    ConstructiveMerge merge;
    Lines base;
    for (std::uint32_t p = 0; p < n; ++p) {
        base.push_back(std::format("b{}\n", p));
    }
    std::vector<std::uint32_t> preA, postA, preB, postB;
    const Lines ours = build(0, preA, postA);
    const Lines theirs = build(1, preB, postB);
    merge.base = join(base);
    merge.ours = join(ours);
    merge.theirs = join(theirs);

    // Components of touching or overlapping regions ([s1, e1] and [s2, e2] meet as closed intervals).
    std::vector<const Region*> all;
    for (const auto& side : regions) {
        for (const Region& region : side) {
            all.push_back(&region);
        }
    }
    std::sort(all.begin(), all.end(),
              [](const Region* x, const Region* y) { return x->s != y->s ? x->s < y->s : x->e < y->e; });
    struct Component {
        std::uint32_t s = 0;
        std::uint32_t e = 0;
        bool sides[2] = {false, false};
    };
    std::vector<Component> components;
    for (const Region* region : all) {
        if (!components.empty() && region->s <= components.back().e) {
            components.back().e = std::max(components.back().e, region->e);
            components.back().sides[region->side] = true;
        } else {
            Component c;
            c.s = region->s;
            c.e = region->e;
            c.sides[region->side] = true;
            components.push_back(c);
        }
    }
    std::uint32_t stableFrom = 0; // Base position where the current stable run starts.
    std::string clean;
    const auto stable = [&](std::uint32_t s, std::uint32_t e) {
        if (s < e) {
            merge.expected.push_back(ConstructiveMerge::Chunk{0, s, e, postA[s], preA[e], postB[s], preB[e]});
            for (std::uint32_t p = s; p < e; ++p) {
                clean += base[p];
            }
        }
    };
    for (const Component& c : components) {
        stable(stableFrom, c.s);
        ConstructiveMerge::Chunk chunk{0, c.s, c.e, preA[c.s], postA[c.e], preB[c.s], postB[c.e]};
        const bool sameText = std::equal(ours.begin() + chunk.ours0, ours.begin() + chunk.ours1,
                                         theirs.begin() + chunk.theirs0, theirs.begin() + chunk.theirs1);
        if (c.sides[0] && c.sides[1] && sameText) {
            // Fresh lines are unique, so equal sides mean the same pure deletion on both.
            chunk.kind = 4;
            for (std::uint32_t i = chunk.ours0; i < chunk.ours1; ++i) {
                clean += ours[i];
            }
        } else if (c.sides[0] && c.sides[1]) {
            chunk.kind = 3;
            ++merge.conflicts;
        } else if (c.sides[0]) {
            chunk.kind = 1;
            for (std::uint32_t i = chunk.ours0; i < chunk.ours1; ++i) {
                clean += ours[i];
            }
        } else {
            chunk.kind = 2;
            for (std::uint32_t i = chunk.theirs0; i < chunk.theirs1; ++i) {
                clean += theirs[i];
            }
        }
        merge.expected.push_back(chunk);
        stableFrom = c.e;
    }
    stable(stableFrom, n);
    if (merge.conflicts == 0) {
        merge.cleanText = std::move(clean);
    }
    return merge;
}

TextTriple randomTriple(SplitMix64& rng, SizeClass size) {
    const PairClass kind = kAllPairClasses[below(rng, std::size(kAllPairClasses))];
    const ClassSource source = makeSource(kind, rng, size);
    const Lines base = baseLines(source, rng);
    Lines ours = base;
    mutateLines(source, ours, rng);
    Lines theirs;
    const double r = unit(rng);
    if (r < 0.1) {
        theirs = ours; // The same change on both sides.
    } else if (r < 0.35) {
        theirs = ours; // Theirs builds on ours: shared changes become Both chunks.
        mutateLines(source, theirs, rng);
    } else {
        theirs = base; // An independent edit of base.
        mutateLines(source, theirs, rng);
    }
    TextTriple triple{join(base), join(ours), join(theirs)};
    maybeDropFinalNewline(triple.base, rng, 0.1);
    maybeDropFinalNewline(triple.ours, rng, 0.1);
    maybeDropFinalNewline(triple.theirs, rng, 0.1);
    return triple;
}

} // namespace frost::diff::test
