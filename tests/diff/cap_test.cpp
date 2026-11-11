// SPDX-License-Identifier: Apache-2.0
//
/*
 * The default work cap on files of ordinary and middle size (ADR 0019, D7): files up to 10k lines
 * with scattered edits, including edits at evenly spaced lines (the histogram's O(N x D) shape,
 * cheap with reuse), diff exactly and merge cleanly under kDefaultMaxWork, and so do the review's
 * mid-size cases (a 200k-line diff with 10 edits, a 150k-line merge with 5 edits per side), which
 * collapsed into one whole-file hunk or conflict while interning cost 8 units for every line.
 * Inputs beyond the cap still degrade within it.
 */

#include "frost/diff/merge.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <initializer_list>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace frost::diff::test {
namespace {

/*
 * A source-like file: unique statements, with a blank line at every 7th line and a closing brace
 * at every 11th, so the file has repeated lines as real code does (never two equal lines in a
 * row). Variants replace chosen lines by splicing the base text, which keeps large inputs cheap to
 * build in Debug.
 */
class Source {
public:
    explicit Source(std::uint32_t lines) {
        m_starts.reserve(lines + 1);
        for (std::uint32_t i = 0; i < lines; ++i) {
            m_starts.push_back(m_text.size());
            if (i % 7 == 3) {
                m_text += '\n';
            } else if (i % 11 == 5) {
                m_text += "}\n";
            } else {
                m_text += std::format("    int value{} = compute({}, {});\n", i, i * 7 % 1000, i % 13);
            }
        }
        m_starts.push_back(m_text.size());
    }

    [[nodiscard]] std::uint32_t lines() const noexcept { return static_cast<std::uint32_t>(m_starts.size() - 1); }
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

    // The text with line p replaced by "    // <tag> edited line <p>" for every (sorted) p of each
    // edit set; a line in two sets takes the first set's tag.
    struct Edits {
        const std::vector<std::uint32_t>& at;
        std::string_view tag;
    };
    [[nodiscard]] std::string variant(std::initializer_list<Edits> edits) const {
        std::vector<std::pair<std::uint32_t, std::string_view>> all;
        for (const Edits& set : edits) {
            for (const std::uint32_t p : set.at) {
                all.emplace_back(p, set.tag);
            }
        }
        std::ranges::stable_sort(all, {}, &std::pair<std::uint32_t, std::string_view>::first);
        std::string out;
        out.reserve(m_text.size() + all.size() * 32);
        std::size_t copied = 0;
        for (std::size_t k = 0; k < all.size(); ++k) {
            const std::uint32_t p = all[k].first;
            if (k > 0 && all[k - 1].first == p) {
                continue;
            }
            out.append(m_text, copied, m_starts[p] - copied);
            out += std::format("    // {} edited line {}\n", all[k].second, p);
            copied = m_starts[p + 1];
        }
        out.append(m_text, copied);
        return out;
    }

private:
    std::string m_text;
    std::vector<std::size_t> m_starts;
};

// Every `period`-th line from `phase`.
std::vector<std::uint32_t> everyNth(std::uint32_t lines, std::uint32_t period, std::uint32_t phase) {
    std::vector<std::uint32_t> at;
    for (std::uint32_t p = phase; p < lines; p += period) {
        at.push_back(p);
    }
    return at;
}

// `count` random lines, sorted, at least `gap` apart and outside `avoid` (and its neighbours).
std::vector<std::uint32_t> scattered(SplitMix64& rng, std::uint32_t lines, std::uint32_t count, std::uint32_t gap,
                                     const std::vector<std::uint32_t>& avoid = {}) {
    std::set<std::uint32_t> taken;
    std::set<std::uint32_t> blocked(avoid.begin(), avoid.end());
    while (taken.size() < count) {
        const auto p = static_cast<std::uint32_t>(rng.below(lines));
        const auto within = [&](const std::set<std::uint32_t>& set, std::uint32_t distance) {
            const auto it = set.lower_bound(p >= distance ? p - distance : 0);
            return it != set.end() && *it <= p + distance;
        };
        if (!within(taken, gap - 1) && !within(blocked, 1)) {
            taken.insert(p);
        }
    }
    return {taken.begin(), taken.end()};
}

// The diff of `base` against its copy with the lines `at` replaced must be exact under the default
// cap: one 1 x 1 hunk per replaced line. `apply` also runs the three appliers of checkApplies.
void expectExactDiff(const Source& base, const std::vector<std::uint32_t>& at, std::string_view what,
                     bool apply = true) {
    const LineSequence a = makeSequence(base.text());
    const LineSequence b = makeSequence(base.variant({{at, "diff"}}));
    auto diff = diffLines(a, b);
    ASSERT_TRUE(diff.has_value()) << diff.error().describe();
    EXPECT_FALSE(diff->degraded) << what << ": " << diff->work << " units";
    EXPECT_LE(diff->work, kDefaultMaxWork);
    ASSERT_EQ(diff->hunks.size(), at.size()) << what;
    for (std::size_t k = 0; k < at.size(); ++k) {
        const LineRange line{LineIndex{at[k]}, LineIndex{at[k] + 1}};
        EXPECT_EQ(diff->hunks[k], (Hunk{line, line})) << what << ": hunk " << k;
    }
    if (apply) {
        EXPECT_TRUE(appliesCleanly(a, b, *diff)) << what;
    }
    std::printf("[ diff ] %s: %u lines, %zu edits: %llu units (%.2fx the default cap)\n", std::string(what).c_str(),
                a.size(), at.size(), static_cast<unsigned long long>(diff->work),
                static_cast<double>(diff->work) / static_cast<double>(kDefaultMaxWork));
}

// The merge of disjoint, non-adjacent replacements must be clean under the default cap, with the
// text both edits make.
void expectCleanMerge(const Source& base, const std::vector<std::uint32_t>& oursAt,
                      const std::vector<std::uint32_t>& theirsAt, std::string_view what) {
    auto model = MergeModel::create(makeSequence(base.text()), makeSequence(base.variant({{oursAt, "ours"}})),
                                    makeSequence(base.variant({{theirsAt, "theirs"}})));
    ASSERT_TRUE(model.has_value()) << model.error().describe();
    const Merge3& merge = model->merge();
    EXPECT_FALSE(merge.degraded) << what << ": " << merge.work << " units";
    EXPECT_LE(merge.work, kDefaultMaxWork);
    EXPECT_EQ(merge.conflicts, 0u) << what;
    EXPECT_TRUE(checkMerge(model->base(), model->ours(), model->theirs(), merge).has_value()) << what;
    auto text = model->resolvedText();
    ASSERT_TRUE(text.has_value()) << what << ": " << text.error().describe();
    EXPECT_TRUE(text->text == base.variant({{oursAt, "ours"}, {theirsAt, "theirs"}}))
        << what << ": the merged text differs";
    std::printf("[ diff ] %s: %u lines, %zu + %zu edits: %llu units (%.2fx the default cap)\n",
                std::string(what).c_str(), model->base().size(), oursAt.size(), theirsAt.size(),
                static_cast<unsigned long long>(merge.work),
                static_cast<double>(merge.work) / static_cast<double>(kDefaultMaxWork));
}

TEST(DefaultCap, OrdinaryFilesDiffExactly) {
    SplitMix64 rng(0xCA90001);
    const Source base(10'000);
    expectExactDiff(base, scattered(rng, 10'000, 10, 2), "10 scattered edits");
    expectExactDiff(base, scattered(rng, 10'000, 300, 2), "300 scattered edits");
    expectExactDiff(base, everyNth(10'000, 40, 0), "every 40th line");
    // Needed 2.5 times the cap before the histogram reused work across regions.
    expectExactDiff(base, everyNth(10'000, 10, 0), "every 10th line");
    expectExactDiff(Source(1'000), everyNth(1'000, 3, 1), "every 3rd line of 1000");
}

TEST(DefaultCap, OrdinaryFilesMergeCleanly) {
    SplitMix64 rng(0xCA90002);
    const Source base(10'000);
    const std::vector<std::uint32_t> ours = scattered(rng, 10'000, 200, 3);
    expectCleanMerge(base, ours, scattered(rng, 10'000, 200, 3, ours), "200 scattered edits per side");
    // One conflict covering most of the file before the histogram reused work across regions.
    expectCleanMerge(base, everyNth(10'000, 40, 0), everyNth(10'000, 40, 20), "every 40th line per side");
    expectCleanMerge(base, everyNth(10'000, 20, 0), everyNth(10'000, 20, 10), "every 20th line per side");
}

TEST(DefaultCap, MidSizeFilesStayExact) {
    // The review's cases: one whole-file hunk and one whole-file conflict while every interned
    // line cost 8 units; the partner path makes the edited copies cheap to intern. The exact hunks
    // pin the diff, so the appliers are skipped at this size.
    expectExactDiff(Source(200'000), everyNth(200'000, 20'000, 8'695), "200k lines, 10 edits", false);
    expectCleanMerge(Source(150'000), everyNth(150'000, 30'000, 6'521), everyNth(150'000, 30'000, 13'636),
                     "150k lines, 5 edits per side");
}

TEST(DefaultCap, BeyondTheCapStillDegrades) {
    // Every other line edited: the histogram's O(N x D) shape with D = N/2 needs far more than the
    // cap. The diff and the merge stay within it, valid and flagged.
    const Source base(10'000);
    const LineSequence a = makeSequence(base.text());
    const LineSequence b = makeSequence(base.variant({{everyNth(10'000, 2, 0), "dense"}}));
    auto diff = diffLines(a, b);
    ASSERT_TRUE(diff.has_value());
    EXPECT_TRUE(diff->degraded);
    EXPECT_LE(diff->work, kDefaultMaxWork);
    EXPECT_TRUE(appliesCleanly(a, b, *diff));
    const LineSequence theirs = makeSequence(base.variant({{everyNth(10'000, 2, 1), "theirs"}}));
    auto merge = merge3(a, b, theirs);
    ASSERT_TRUE(merge.has_value());
    EXPECT_TRUE(merge->degraded);
    EXPECT_LE(merge->work, kDefaultMaxWork);
    EXPECT_TRUE(checkMerge(a, b, theirs, *merge).has_value());
}

} // namespace
} // namespace frost::diff::test
