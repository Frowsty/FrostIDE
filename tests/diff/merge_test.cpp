// SPDX-License-Identifier: Apache-2.0
//
// Three-way merge tests (ADR 0019, D10-D12 and D16): byte-exact regression goldens of chunks and
// diff3 markers (written by frost::diff and reviewed by hand, not an independent oracle), the
// invariants C1-C4 over random triples (checked by an independent checker as well as checkMerge),
// identities under every work cap, the constructive oracle with known ground truth, soundness of
// degraded merges, the marker round trip (also after custom resolutions), the marker size rule
// and its bound, the resolution model, OursPatch and line-break joining.

#include "frost/diff/merge.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/files.hpp"
#include "diff/support/generators.hpp"
#include <gtest/gtest.h>

#include <limits>
#include <optional>

namespace frost::diff::test {
namespace {

std::string_view kindName(ChunkKind kind) {
    switch (kind) {
    case ChunkKind::Stable: return "Stable";
    case ChunkKind::Ours: return "Ours";
    case ChunkKind::Theirs: return "Theirs";
    case ChunkKind::Both: return "Both";
    case ChunkKind::Conflict: return "Conflict";
    }
    return "?";
}

std::string chunksText(const Merge3& merge) {
    std::string out;
    for (const MergeChunk& c : merge.chunks) {
        out += std::format("{} {} {} {} {} {} {}\n", kindName(c.kind), c.base.begin.value(), c.base.end.value(),
                           c.ours.begin.value(), c.ours.end.value(), c.theirs.begin.value(), c.theirs.end.value());
    }
    out += std::format("conflicts {}\ndegraded {}\n", merge.conflicts, merge.degraded ? 1 : 0);
    return out;
}

std::string_view bytesOf(const LineSequence& sequence, LineRange range) {
    const text::ByteRange r = sequence.byteRange(range);
    return sequence.bytes().substr(r.begin.value(), r.size());
}

// C1-C4, checked independently of checkMerge.
::testing::AssertionResult invariantsHold(const LineSequence& base, const LineSequence& ours,
                                          const LineSequence& theirs, const Merge3& merge) {
    std::uint32_t nextBase = 0;
    std::uint32_t nextOurs = 0;
    std::uint32_t nextTheirs = 0;
    std::uint32_t conflicts = 0;
    for (std::size_t k = 0; k < merge.chunks.size(); ++k) {
        const MergeChunk& c = merge.chunks[k];
        if (c.base.begin.value() != nextBase || c.ours.begin.value() != nextOurs ||
            c.theirs.begin.value() != nextTheirs || c.base.end < c.base.begin || c.ours.end < c.ours.begin ||
            c.theirs.end < c.theirs.begin) {
            return ::testing::AssertionFailure() << "C1: chunk " << k << " does not continue the partition";
        }
        nextBase = c.base.end.value();
        nextOurs = c.ours.end.value();
        nextTheirs = c.theirs.end.value();
        if (c.base.empty() && c.ours.empty() && c.theirs.empty()) {
            return ::testing::AssertionFailure() << "C4: chunk " << k << " is empty on every side";
        }
        if (k > 0 && c.kind == ChunkKind::Stable && merge.chunks[k - 1].kind == ChunkKind::Stable) {
            return ::testing::AssertionFailure() << "C4: adjacent Stable chunks at " << k;
        }
        const std::string_view b = bytesOf(base, c.base);
        const std::string_view o = bytesOf(ours, c.ours);
        const std::string_view t = bytesOf(theirs, c.theirs);
        bool ok = false;
        switch (c.kind) {
        case ChunkKind::Stable: ok = b == o && b == t; break;
        case ChunkKind::Ours: ok = t == b && o != b; break;
        case ChunkKind::Theirs: ok = o == b && t != b; break;
        case ChunkKind::Both: ok = o == t && o != b; break;
        case ChunkKind::Conflict:
            ok = o != b && t != b && o != t;
            ++conflicts;
            break;
        }
        if (!ok) {
            return ::testing::AssertionFailure()
                   << "C2/C3: chunk " << k << " (" << kindName(c.kind) << ") does not match its sides";
        }
    }
    if (nextBase != base.size() || nextOurs != ours.size() || nextTheirs != theirs.size()) {
        return ::testing::AssertionFailure() << "C1: the chunks do not cover every side";
    }
    if (conflicts != merge.conflicts) {
        return ::testing::AssertionFailure()
               << "C4: conflicts " << merge.conflicts << " but " << conflicts << " Conflict chunks";
    }
    if (auto checked = checkMerge(base, ours, theirs, merge); !checked) {
        return ::testing::AssertionFailure() << "checkMerge: " << checked.error().describe();
    }
    return ::testing::AssertionSuccess();
}

MergeModel makeModel(const std::string& base, const std::string& ours, const std::string& theirs,
                     const MergeOptions& options = {}, LineSplit split = LineSplit::Lsp) {
    auto model =
        MergeModel::create(makeSequence(base, split), makeSequence(ours, split), makeSequence(theirs, split), options);
    EXPECT_TRUE(model.has_value()) << model.error().describe();
    if (!model) {
        std::abort(); // Every input here is within the limits.
    }
    return std::move(*model);
}

// renderWithMarkers' text, or a placeholder and a test failure when it refuses.
std::string withMarkers(const MergeModel& model, const MarkerOptions& options = {}) {
    auto text = model.renderWithMarkers(options);
    EXPECT_TRUE(text.has_value()) << text.error().describe();
    return text ? text->text : std::string("<no markers>");
}

constexpr std::uint64_t kCaps[] = {0, 1, 16, 1000, kDefaultMaxWork};

MergeOptions capped(std::uint64_t cap) {
    MergeOptions options;
    options.diff.limits.maxWork = cap;
    return options;
}

TEST(Merge, Goldens) {
    const std::string root = testDataPath("goldens/merge");
    auto names = listNames(root, true);
    ASSERT_TRUE(names.has_value()) << names.error().describe();
    ASSERT_GE(names->size(), 16u);
    bool updated = false;
    for (const std::string& name : *names) {
        const std::string dir = joinUtf8(root, name);
        auto base = readBinaryFile(joinUtf8(dir, "base"));
        auto ours = readBinaryFile(joinUtf8(dir, "ours"));
        auto theirs = readBinaryFile(joinUtf8(dir, "theirs"));
        ASSERT_TRUE(base && ours && theirs) << name;
        const LineSplit split = name.ends_with("-lf") ? LineSplit::Lf : LineSplit::Lsp;
        MergeOptions options;
        if (name == "degraded-tiny-cap") {
            options.diff.limits.maxWork = 20;
        }
        const MergeModel model = makeModel(*base, *ours, *theirs, options, split);
        ASSERT_TRUE(invariantsHold(model.base(), model.ours(), model.theirs(), model.merge())) << name;
        const std::string chunks = chunksText(model.merge());
        const std::string markers = withMarkers(model);
        if (updateGoldens()) {
            ASSERT_TRUE(writeBinaryFile(joinUtf8(dir, "chunks.txt"), chunks).has_value());
            ASSERT_TRUE(writeBinaryFile(joinUtf8(dir, "markers.diff3"), markers).has_value());
            updated = true;
            continue;
        }
        auto expectedChunks = readBinaryFile(joinUtf8(dir, "chunks.txt"));
        auto expectedMarkers = readBinaryFile(joinUtf8(dir, "markers.diff3"));
        ASSERT_TRUE(expectedChunks && expectedMarkers) << name;
        EXPECT_EQ(chunks, *expectedChunks) << name;
        EXPECT_EQ(markers, *expectedMarkers) << name << ": " << printable(markers);
    }
    EXPECT_FALSE(updated) << "FROST_DIFF_UPDATE_GOLDENS=1 rewrote the merge goldens; review and commit them";
    // The goldens cover what they claim.
    const MergeModel crlf =
        makeModel(*readBinaryFile(joinUtf8(joinUtf8(root, "crlf-file-lf"), "base")),
                  *readBinaryFile(joinUtf8(joinUtf8(root, "crlf-file-lf"), "ours")),
                  *readBinaryFile(joinUtf8(joinUtf8(root, "crlf-file-lf"), "theirs")), {}, LineSplit::Lf);
    EXPECT_EQ(crlf.eol(), "\r\n");
}

TEST(Merge, Invariants) {
    SplitMix64 rng(0xD1FF0040);
    const std::uint32_t cases = scaled(10000, 3000, 1500);
    std::uint32_t kinds[5] = {};
    for (std::uint32_t c = 0; c < cases; ++c) {
        const std::uint64_t seed = rng.next();
        SplitMix64 local(seed);
        const TextTriple triple = randomTriple(local);
        const LineSplit split = c % 2 == 0 ? LineSplit::Lsp : LineSplit::Lf;
        const LineSequence base = makeSequence(triple.base, split);
        const LineSequence ours = makeSequence(triple.ours, split);
        const LineSequence theirs = makeSequence(triple.theirs, split);
        auto merge = merge3(base, ours, theirs);
        ASSERT_TRUE(merge.has_value());
        ASSERT_TRUE(invariantsHold(base, ours, theirs, *merge)) << "seed " << seed;
        for (const MergeChunk& chunk : merge->chunks) {
            ++kinds[static_cast<int>(chunk.kind)];
        }
    }
    // The generator reaches every kind.
    for (const std::uint32_t count : kinds) {
        EXPECT_GT(count, 0u);
    }
}

std::string resolved(const MergeModel& model) {
    auto text = model.resolvedText();
    EXPECT_TRUE(text.has_value()) << text.error().describe();
    return text ? text->text : std::string("<unresolved>");
}

TEST(Merge, Identities) {
    SplitMix64 rng(0xD1FF0041);
    const std::uint32_t cases = scaled(1500, 400, 200);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const TextTriple triple = randomTriple(rng);
        for (const std::uint64_t cap : kCaps) {
            const MergeOptions options = capped(cap);
            // merge(O, A, A) is clean and equals A; merge(O, O, B) equals B; merge(O, A, O) equals A.
            const MergeModel same = makeModel(triple.base, triple.ours, triple.ours, options);
            ASSERT_EQ(same.merge().conflicts, 0u) << "cap " << cap;
            ASSERT_EQ(resolved(same), triple.ours) << "cap " << cap;
            const MergeModel onlyTheirs = makeModel(triple.base, triple.base, triple.theirs, options);
            ASSERT_EQ(onlyTheirs.merge().conflicts, 0u);
            ASSERT_EQ(resolved(onlyTheirs), triple.theirs) << "cap " << cap;
            const MergeModel onlyOurs = makeModel(triple.base, triple.ours, triple.base, options);
            ASSERT_EQ(onlyOurs.merge().conflicts, 0u);
            ASSERT_EQ(resolved(onlyOurs), triple.ours) << "cap " << cap;
            // Symmetry when neither result is degraded.
            const MergeModel forward = makeModel(triple.base, triple.ours, triple.theirs, options);
            const MergeModel backward = makeModel(triple.base, triple.theirs, triple.ours, options);
            if (!forward.merge().degraded && !backward.merge().degraded) {
                ASSERT_EQ(forward.merge().chunks.size(), backward.merge().chunks.size());
                for (std::size_t k = 0; k < forward.merge().chunks.size(); ++k) {
                    const MergeChunk& f = forward.merge().chunks[k];
                    const MergeChunk& b = backward.merge().chunks[k];
                    const ChunkKind swapped = f.kind == ChunkKind::Ours     ? ChunkKind::Theirs
                                              : f.kind == ChunkKind::Theirs ? ChunkKind::Ours
                                                                            : f.kind;
                    ASSERT_EQ(b.kind, swapped);
                    ASSERT_EQ(f.base, b.base);
                    ASSERT_EQ(f.ours, b.theirs);
                    ASSERT_EQ(f.theirs, b.ours);
                }
            }
        }
    }
}

int truthKind(ChunkKind kind) {
    switch (kind) {
    case ChunkKind::Stable: return 0;
    case ChunkKind::Ours: return 1;
    case ChunkKind::Theirs: return 2;
    case ChunkKind::Conflict: return 3;
    case ChunkKind::Both: return 4;
    }
    return -1;
}

TEST(Merge, ConstructiveOracle) {
    SplitMix64 rng(0xD1FF0042);
    const std::uint32_t cases = scaled(4000, 1500, 600);
    std::uint32_t clean = 0;
    std::uint32_t conflicted = 0;
    for (std::uint32_t c = 0; c < cases; ++c) {
        const std::uint64_t seed = rng.next();
        SplitMix64 local(seed);
        const ConstructiveMerge truth = constructiveMerge(local, c % 10 == 0 ? SizeClass::Large : SizeClass::Small);
        const MergeModel model = makeModel(truth.base, truth.ours, truth.theirs);
        const Merge3& merge = model.merge();
        ASSERT_FALSE(merge.degraded);
        ASSERT_EQ(merge.chunks.size(), truth.expected.size()) << "seed " << seed << "\n" << chunksText(merge);
        for (std::size_t k = 0; k < merge.chunks.size(); ++k) {
            const MergeChunk& got = merge.chunks[k];
            const ConstructiveMerge::Chunk& want = truth.expected[k];
            ASSERT_EQ(truthKind(got.kind), want.kind) << "seed " << seed << " chunk " << k;
            ASSERT_EQ(got.base, (LineRange{LineIndex{want.base0}, LineIndex{want.base1}})) << "seed " << seed;
            ASSERT_EQ(got.ours, (LineRange{LineIndex{want.ours0}, LineIndex{want.ours1}})) << "seed " << seed;
            ASSERT_EQ(got.theirs, (LineRange{LineIndex{want.theirs0}, LineIndex{want.theirs1}})) << "seed " << seed;
        }
        ASSERT_EQ(merge.conflicts, truth.conflicts);
        if (truth.conflicts == 0) {
            ASSERT_EQ(resolved(model), truth.cleanText) << "seed " << seed;
            ++clean;
        } else {
            ++conflicted;
        }
    }
    EXPECT_GT(clean, cases / 20);
    EXPECT_GT(conflicted, cases / 20);
}

TEST(Merge, DegradedSound) {
    SplitMix64 rng(0xD1FF0043);
    const std::uint32_t cases = scaled(2000, 600, 300);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const std::uint64_t seed = rng.next();
        SplitMix64 local(seed);
        const ConstructiveMerge truth = constructiveMerge(local);
        for (const std::uint64_t cap : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{16}, std::uint64_t{1000}}) {
            const MergeModel model = makeModel(truth.base, truth.ours, truth.theirs, capped(cap));
            const Merge3& merge = model.merge();
            ASSERT_TRUE(invariantsHold(model.base(), model.ours(), model.theirs(), merge)) << "seed " << seed;
            // Every base line of a true conflict lies inside a Conflict chunk.
            for (const ConstructiveMerge::Chunk& want : truth.expected) {
                if (want.kind != 3) {
                    continue;
                }
                for (std::uint32_t line = want.base0; line < want.base1; ++line) {
                    bool covered = false;
                    for (const MergeChunk& got : merge.chunks) {
                        covered = covered || (got.kind == ChunkKind::Conflict && got.base.begin.value() <= line &&
                                              line < got.base.end.value());
                    }
                    ASSERT_TRUE(covered) << "seed " << seed << " cap " << cap << ": base line " << line;
                }
            }
            // A true conflict is never merged cleanly, and a clean (possibly degraded) merge
            // produces exactly the true merge.
            if (merge.conflicts == 0) {
                ASSERT_EQ(truth.conflicts, 0u);
                ASSERT_EQ(resolved(model), truth.cleanText) << "seed " << seed << " cap " << cap;
            }
        }
    }
}

TEST(Merge, MarkersRoundTrip) {
    SplitMix64 rng(0xD1FF0044);
    const std::uint32_t cases = scaled(3000, 800, 400);
    std::uint32_t conflicts = 0;
    for (std::uint32_t c = 0; c < cases; ++c) {
        TextTriple triple = randomTriple(rng);
        if (c % 5 == 0) {
            // Marker-like content forces a larger marker size.
            triple.ours += "=======\n<<<<<<<< x\n";
            triple.theirs += "|||||||||\n";
        }
        // Lsp only without lone CRs: a lone-CR break followed by an LF line would fuse into one CRLF
        // in the output (the documented Lsp caveat), which no line parser can split back.
        const bool loneCr = hasLoneCr(triple.base) || hasLoneCr(triple.ours) || hasLoneCr(triple.theirs);
        const LineSplit split = c % 2 == 0 && !loneCr ? LineSplit::Lsp : LineSplit::Lf;
        const MergeModel model = makeModel(triple.base, triple.ours, triple.theirs, {}, split);
        MarkerOptions labels;
        // Labels with LF and backslash stay on their marker line (escaped).
        labels.oursLabel = "mine\nfake";
        labels.baseLabel = "";
        labels.theirsLabel = "their\\s";
        labels.minSize = static_cast<std::uint8_t>(7 + c % 3);
        const auto checked = checkMarkersRoundTrip(model, labels);
        ASSERT_TRUE(checked.has_value()) << checked.error().describe();
        conflicts += model.merge().conflicts;
        // Custom resolutions of every other conflict, with marker-like lines, while the others
        // stay unresolved: their text counts when the marker size is chosen.
        MergeModel resolving = makeModel(triple.base, triple.ours, triple.theirs, {}, split);
        std::uint32_t seen = 0;
        for (std::size_t k = 0; k < resolving.merge().chunks.size(); ++k) {
            if (resolving.merge().chunks[k].kind == ChunkKind::Conflict && seen++ % 2 == 0) {
                const std::string custom = c % 3 == 0   ? "kept\n=======\nalso kept\n"
                                           : c % 3 == 1 ? std::string(8 + c % 5, '<') + " x\n>>>>>>>>\n"
                                                        : "|||||||";
                ASSERT_TRUE(resolving.resolve(k, Take::Custom, custom).has_value());
            }
        }
        const auto afterCustom = checkMarkersRoundTrip(resolving, labels);
        ASSERT_TRUE(afterCustom.has_value()) << afterCustom.error().describe();
    }
    EXPECT_GT(conflicts, 0u);
}

// Two conflicts; ours and theirs change m0 and m1 differently.
MergeModel twoConflicts(const std::string& head = {}) {
    return makeModel(head + "s0\nm0\ns1\nm1\ns2\n", head + "s0\no0\ns1\no1\ns2\n", head + "s0\nt0\ns1\nt1\ns2\n");
}

TEST(Merge, MarkersAfterCustomResolution) {
    // A custom resolution with a line of seven '=' next to a remaining conflict: the size skips 7.
    MergeModel model = twoConflicts();
    ASSERT_EQ(model.merge().conflicts, 2u);
    ASSERT_EQ(model.merge().chunks[1].kind, ChunkKind::Conflict);
    ASSERT_TRUE(model.resolve(1, Take::Custom, "kept\n=======\nalso kept\n").has_value());
    EXPECT_EQ(model.customText(1), "kept\n=======\nalso kept\n");
    auto size = model.markerSize();
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, 8u);
    EXPECT_EQ(withMarkers(model),
              "s0\nkept\n=======\nalso kept\ns1\n<<<<<<<< ours\no1\n|||||||| base\nm1\n========\nt1\n"
              ">>>>>>>> theirs\ns2\n");
    const auto checked = checkMarkersRoundTrip(model, MarkerOptions{});
    EXPECT_TRUE(checked.has_value()) << checked.error().describe();
    // Resolving it otherwise frees size 7 again.
    ASSERT_TRUE(model.resolve(1, Take::Ours).has_value());
    EXPECT_EQ(model.customText(1), "");
    EXPECT_EQ(*model.markerSize(), 7u);
}

TEST(Merge, MarkerSizeBounded) {
    // A hostile line of 100000 '=' (or '<', '|', '>') in every side, plus conflicts: the run is
    // longer than kMaxMarkerSize, so it collides with no size, and the markers stay at 7.
    for (const char c : {'=', '<', '|', '>'}) {
        const std::string head = std::string(100000, c) + "\n";
        const MergeModel model = twoConflicts(head);
        ASSERT_EQ(model.merge().conflicts, 2u);
        auto size = model.markerSize();
        ASSERT_TRUE(size.has_value());
        EXPECT_EQ(*size, 7u);
        const std::string text = withMarkers(model);
        const std::size_t inputs = model.base().bytes().size() + model.ours().bytes().size();
        EXPECT_LE(text.size(), inputs + 2 * 4 * (kMaxMarkerSize + 16)) << "marker lines grew with the content";
        const auto checked = checkMarkersRoundTrip(model, MarkerOptions{});
        EXPECT_TRUE(checked.has_value()) << checked.error().describe();
    }
}

TEST(Merge, MarkerSizeSkipsTakenLengths) {
    const auto runs = [](std::initializer_list<std::uint32_t> lengths) {
        std::string lines;
        for (const std::uint32_t n : lengths) {
            lines += std::string(n, "<|=>"[n % 4]) + (n % 2 == 0 ? "\n" : " tail\n");
        }
        return lines;
    };
    // Runs of 7, 8 and 10 leave 9; the minimum size moves the search's start.
    EXPECT_EQ(*twoConflicts(runs({7, 8, 10})).markerSize(), 9u);
    MarkerOptions larger;
    larger.minSize = 11;
    EXPECT_EQ(*twoConflicts(runs({7, 8, 10})).markerSize(larger), 11u);
    // Shorter runs than the minimum never collide.
    EXPECT_EQ(*twoConflicts(runs({3, 6})).markerSize(), 7u);
    // Every size up to 63 taken: 64, the largest.
    std::vector<std::uint32_t> lengths;
    for (std::uint32_t n = 7; n <= 63; ++n) {
        lengths.push_back(n);
    }
    std::string allBut64;
    for (const std::uint32_t n : lengths) {
        allBut64 += std::string(n, '=') + "\n";
    }
    const MergeModel nearly = twoConflicts(allBut64);
    EXPECT_EQ(*nearly.markerSize(), 64u);
    EXPECT_TRUE(checkMarkersRoundTrip(nearly, MarkerOptions{}).has_value());
    // Every size taken: renderWithMarkers refuses instead of writing ambiguous markers.
    const MergeModel full = twoConflicts(allBut64 + std::string(64, '>') + "\n");
    auto refused = full.renderWithMarkers();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
    EXPECT_FALSE(full.markerSize().has_value());
    EXPECT_TRUE(checkMarkersRoundTrip(full, MarkerOptions{}).has_value()); // The refusal is the rule's.
    // resolvedText is unaffected.
    MergeModel resolvable = twoConflicts(allBut64 + std::string(64, '>') + "\n");
    for (std::size_t k = 0; k < resolvable.merge().chunks.size(); ++k) {
        if (resolvable.merge().chunks[k].kind == ChunkKind::Conflict) {
            ASSERT_TRUE(resolvable.resolve(k, Take::Theirs).has_value());
        }
    }
    EXPECT_EQ(resolved(resolvable), allBut64 + std::string(64, '>') + "\ns0\nt0\ns1\nt1\ns2\n");
}

TEST(Merge, ResolutionModel) {
    // base a b c d e; ours changes b (Ours), both change d differently (Conflict), theirs adds f.
    MergeModel model = makeModel("a\nb\nc\nd\ne\n", "a\nB\nc\nD1\ne\n", "a\nb\nc\nD2\ne\nf\n");
    const Merge3& merge = model.merge();
    ASSERT_EQ(chunksText(merge), "Stable 0 1 0 1 0 1\nOurs 1 2 1 2 1 2\nStable 2 3 2 3 2 3\nConflict 3 4 3 4 3 4\n"
                                 "Stable 4 5 4 5 4 5\nTheirs 5 5 5 5 5 6\nconflicts 1\ndegraded 0\n");
    // Defaults.
    EXPECT_EQ(model.take(0), Take::Ours);
    EXPECT_EQ(model.take(1), Take::Ours);
    EXPECT_EQ(model.take(3), Take::Unresolved);
    EXPECT_EQ(model.take(5), Take::Theirs);
    EXPECT_EQ(model.unresolvedCount(), 1u);
    // Errors.
    auto unresolved = model.resolvedText();
    ASSERT_FALSE(unresolved.has_value());
    EXPECT_EQ(unresolved.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(unresolved.error().message(), "chunk 3: conflict unresolved");
    EXPECT_FALSE(model.editsForOurs().has_value());
    EXPECT_EQ(model.resolve(6, Take::Ours).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(model.resolve(0, Take::Theirs).error().code(), ErrorCode::InvalidArgument); // Stable.
    EXPECT_EQ(model.resolve(1, Take::Unresolved).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(model.resolve(3, Take::Ours, "text").error().code(), ErrorCode::InvalidArgument);
    // Every take on the conflict, with the output chunk ranges.
    struct Case {
        Take take;
        std::string custom;
        std::string text;
    };
    const Case cases[] = {
        {Take::Ours, "", "a\nB\nc\nD1\ne\nf\n"},
        {Take::Theirs, "", "a\nB\nc\nD2\ne\nf\n"},
        {Take::Base, "", "a\nB\nc\nd\ne\nf\n"},
        {Take::OursThenTheirs, "", "a\nB\nc\nD1\nD2\ne\nf\n"},
        {Take::TheirsThenOurs, "", "a\nB\nc\nD2\nD1\ne\nf\n"},
        {Take::Custom, "mine\n", "a\nB\nc\nmine\ne\nf\n"},
        {Take::Custom, "", "a\nB\nc\ne\nf\n"},
        {Take::Custom, "joined", "a\nB\nc\njoined\ne\nf\n"}, // A custom text without a break is joined.
    };
    for (const Case& c : cases) {
        ASSERT_TRUE(model.resolve(3, c.take, c.custom).has_value());
        EXPECT_EQ(model.unresolvedCount(), 0u);
        auto output = model.resolvedText();
        ASSERT_TRUE(output.has_value());
        EXPECT_EQ(output->text, c.text);
        ASSERT_EQ(output->chunks.size(), merge.chunks.size());
        std::uint64_t at = 0;
        for (const text::ByteRange range : output->chunks) {
            EXPECT_EQ(range.begin.value(), at);
            at = range.end.value();
        }
        EXPECT_EQ(at, output->text.size());
        EXPECT_EQ(output->chunks[0].size(), 2u); // "a\n"
    }
    // Overriding non-conflict chunks: reject ours' change and theirs' addition.
    ASSERT_TRUE(model.resolve(1, Take::Base).has_value());
    ASSERT_TRUE(model.resolve(5, Take::Base).has_value());
    ASSERT_TRUE(model.resolve(3, Take::Unresolved).has_value());
    EXPECT_EQ(model.unresolvedCount(), 1u);
    ASSERT_TRUE(model.resolve(3, Take::Base).has_value());
    EXPECT_EQ(resolved(model), "a\nb\nc\nd\ne\n");
    EXPECT_EQ(withMarkers(model), "a\nb\nc\nd\ne\n"); // Resolved chunks render as resolved.
    // Moving the model keeps its state.
    MergeModel moved = std::move(model);
    EXPECT_EQ(resolved(moved), "a\nb\nc\nd\ne\n");
}

TEST(Merge, EditsForOursApply) {
    SplitMix64 rng(0xD1FF0045);
    const std::uint32_t cases = scaled(3000, 800, 400);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const TextTriple triple = randomTriple(rng);
        if (!validUtf8(triple.ours)) {
            continue;
        }
        std::optional<OursPatch> patch;
        std::string expected;
        {
            MergeModel model = makeModel(triple.base, triple.ours, triple.theirs);
            for (std::size_t k = 0; k < model.merge().chunks.size(); ++k) {
                if (model.merge().chunks[k].kind == ChunkKind::Conflict) {
                    const Take takes[] = {Take::Theirs, Take::OursThenTheirs, Take::Custom, Take::Base};
                    const Take take = takes[(c + k) % 4];
                    ASSERT_TRUE(
                        model.resolve(k, take, take == Take::Custom ? std::string("custom\n") : "").has_value());
                }
            }
            auto text = model.resolvedText();
            ASSERT_TRUE(text.has_value());
            expected = text->text;
            auto made = model.editsForOurs();
            ASSERT_TRUE(made.has_value());
            patch.emplace(std::move(*made));
        } // The model is gone; the patch is self-contained.
        OursPatch moved = std::move(*patch);
        patch.reset();
        if (!validUtf8(expected)) {
            continue;
        }
        auto buffer = text::TextBuffer::fromUtf8(triple.ours);
        ASSERT_TRUE(buffer.has_value());
        const std::span<const text::Edit> edits = moved.edits();
        for (std::size_t k = 1; k < edits.size(); ++k) {
            ASSERT_LT(edits[k - 1].range.end.value(), edits[k].range.begin.value()) << "edits are not non-adjacent";
        }
        auto applied = buffer->applyEdits(edits);
        ASSERT_TRUE(applied.has_value()) << applied.error().describe();
        ASSERT_EQ(buffer->snapshot().text(), expected);
    }
}

TEST(Merge, EolJoin) {
    // The final chunk's pieces without a break get eol() when more output follows.
    MergeModel lf = makeModel("a\nb", "a\nB", "a\nC");
    EXPECT_EQ(lf.eol(), "\n");
    ASSERT_TRUE(lf.resolve(1, Take::OursThenTheirs).has_value());
    EXPECT_EQ(resolved(lf), "a\nB\nC");
    ASSERT_TRUE(lf.resolve(1, Take::TheirsThenOurs).has_value());
    EXPECT_EQ(resolved(lf), "a\nC\nB");
    ASSERT_TRUE(lf.resolve(1, Take::Ours).has_value());
    EXPECT_EQ(resolved(lf), "a\nB"); // Nothing follows: no break is added.
    ASSERT_TRUE(lf.resolve(1, Take::Unresolved).has_value());
    EXPECT_EQ(withMarkers(lf), "a\n<<<<<<< ours\nB\n||||||| base\nb\n=======\nC\n>>>>>>> theirs\n");
    // eol() detection order: ours, theirs, base, LF.
    EXPECT_EQ(makeModel("x\r\n", "y", "z\r").eol(), "\r");
    EXPECT_EQ(makeModel("x\r\n", "y", "z").eol(), "\r\n");
    EXPECT_EQ(makeModel("x", "y", "z").eol(), "\n");
    EXPECT_EQ(makeModel("x\r", "y", "z", {}, LineSplit::Lf).eol(), "\n");
    // MergeOptions::eol overrides detection.
    MergeOptions crlf;
    crlf.eol = text::LineBreak::CrLf;
    MergeModel forced = makeModel("a\nb", "a\nB", "a\nC", crlf);
    EXPECT_EQ(forced.eol(), "\r\n");
    ASSERT_TRUE(forced.resolve(1, Take::OursThenTheirs).has_value());
    EXPECT_EQ(resolved(forced), "a\nB\r\nC");
    EXPECT_TRUE(forced.resolve(1, Take::Unresolved).has_value());
    EXPECT_EQ(withMarkers(forced), "a\n<<<<<<< ours\r\nB\r\n||||||| base\r\nb\r\n=======\r\nC\r\n>>>>>>> theirs\r\n");
    // A custom text without a final break is joined to what follows it.
    MergeModel custom = makeModel("1\n2\n3\n", "1\nX\n3\n", "1\nY\n3\n");
    ASSERT_TRUE(custom.resolve(1, Take::Custom, "Z").has_value());
    EXPECT_EQ(resolved(custom), "1\nZ\n3\n");
}

} // namespace
} // namespace frost::diff::test
