// SPDX-License-Identifier: Apache-2.0
//
// Micro-benchmarks for frost::text::Document (label `perf`, ADR 0017). Like TextPerf, Debug and
// sanitizer builds run a small scale and only report; optimised unsanitised builds run the full
// scale and assert B5's text-side sub-budget (decision 12).
/*
 * DocumentPerf.TypingLatency: a 16 MiB code-like document (half CRLF) with 10k registry anchors
 * (half Left, half Right) and one caret. 100k ops, 85 % one-byte Typing and 15 % DeleteBackward,
 * with the ManualClock advancing 50 ms per op; one op is Document::replace, then snapshot(),
 * pointAt(caret), a 60-line LineCursor and resolveRange over those lines. Reports p50/p99/max and
 * asserts p99 <= 0.25 ms. Budget: B5. Debug: 1 MiB, 2k ops, report only.
 *
 * DocumentPerf.UndoRedoLatency: the same setup, then 200 coalesced typing runs of 1000 keystrokes
 * and 50 coalesced backspace runs of 1000 keystrokes (each ended by breakCoalescing()), a 64 KiB
 * paste and a 4 MiB deletion; then every undo and every redo, each followed by snapshot() and
 * pointAt. Reports p50/p99/max over all of them and asserts p99 <= 0.25 ms. Budget: B5. Reports
 * the paste's and the deletion's undo and redo separately with the number of anchors each restore
 * moved (anchors in the restored ranges whose offset differs from plain mapping), and the node
 * bytes the history retains beyond the live text and anchors (unique SumTree nodes of slices and
 * anchor sets, plus node and region records) per transaction, projected to 1000 transactions
 * against B19's 80 MB: for the typing runs, for the backspace runs, for the whole history as
 * recorded, and after every node was undone and redone (when each also holds its anchorsAfter).
 * Debug: 1 MiB, 20 typing and 5 backspace runs of 100 keystrokes, a 4 KiB paste and a 256 KiB
 * deletion, report only.
 *
 * DocumentPerf.AnchorsAndCarets (report only): resolve p50/p99 with 100k anchors, one edit
 * collapsing 1000 anchors at distinct offsets and its undo, the undo of a 64 KiB paste holding 10k
 * anchors created after it (the restore scan's newer-id rejection), the keystroke whose history
 * cap drops a 999-transaction redo branch, a backspace next to 10k anchors at one offset (the
 * first-gap shortcut), relabel counts under 100k adversarial inserts, and a 10k-caret Typing
 * batch (applyEdits, anchors and SelectionSet::transformed) with its undo. Debug and sanitizer
 * builds: 10k anchors, 10k inserts and 2k carets.
 *
 * The tests include src/text/src/undo_tree.hpp (private) for the history's nodes.
 */

#include "frost/core/clock.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/document.hpp"
#include "frost/text/selection.hpp"

#include "support/test_env.hpp"
#include "undo_tree.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ByteOffset;
using frost::LineIndex;
using frost::ManualClock;
using frost::Result;
using frost::sched::detail::SplitMix64;
using frost::test::kAssertBudgets;
using frost::text::AnchorId;
using frost::text::AnchorSet;
using frost::text::Bias;
using frost::text::ByteRange;
using frost::text::ChangeSet;
using frost::text::Document;
using frost::text::DocumentSnapshot;
using frost::text::Edit;
using frost::text::EditContext;
using frost::text::EditKind;
using frost::text::LineCursor;
using frost::text::LineInfo;
using frost::text::Point;
using frost::text::RawEdit;
using frost::text::ResolvedAnchor;
using frost::text::Selection;
using frost::text::SelectionSet;
using Clock = std::chrono::steady_clock;

struct Percentiles {
    double p50us = 0;
    double p99us = 0;
    double maxUs = 0;
};

Percentiles percentiles(std::vector<std::int64_t> samplesNs) {
    std::sort(samplesNs.begin(), samplesNs.end());
    const auto at = [&](double q) {
        const auto index = static_cast<std::size_t>(q * static_cast<double>(samplesNs.size() - 1));
        return static_cast<double>(samplesNs[index]) / 1000.0;
    };
    return {at(0.50), at(0.99), static_cast<double>(samplesNs.back()) / 1000.0};
}

std::int64_t elapsedNs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

double microseconds(std::int64_t ns) { return static_cast<double>(ns) / 1000.0; }

// TextPerf's code-like document: indented lines of identifiers, punctuation and comments, half of
// them ending in CRLF, a few with non-ASCII characters.
std::string codeLike(std::size_t bytes) {
    constexpr std::string_view kWords[] = {
        "auto", "const", "return", "if", "for", "value", "index", "std::size_t", "result", "(",
        ")",    "{",     "}",      ";",  "=",   "+",     "->",    "// note",     "\"é\"",  "u8\"\xE2\x82\xAC\""};
    SplitMix64 rng(16);
    std::string text;
    text.reserve(bytes + 256);
    while (text.size() < bytes) {
        text.append(4 * static_cast<std::size_t>(rng.below(5)), ' ');
        for (std::uint64_t words = 2 + rng.below(10); words > 0; --words) {
            text += kWords[rng.below(std::size(kWords))];
            text += ' ';
        }
        text += rng.below(2) == 0 ? "\r\n" : "\n";
    }
    return text;
}

constexpr bool kFullScale = kAssertBudgets;
constexpr std::size_t kDocumentBytes = kFullScale ? std::size_t{16} << 20 : std::size_t{1} << 20;
constexpr std::uint32_t kAnchors = 10'000;

SelectionSet caret(ByteOffset at) { return SelectionSet::caret(at); }

// A loaded document with kAnchors registry anchors at random char boundaries, half Left and half
// Right. The clock must outlive the document.
Document loadDocument(const ManualClock& clock, std::size_t bytes, std::uint32_t anchors, std::uint64_t seed) {
    Document doc = Document::fromUtf8(codeLike(bytes), {}, {}, clock).value();
    SplitMix64 rng(seed);
    const std::uint64_t size = doc.text().size().value();
    for (std::uint32_t i = 0; i < anchors; ++i) {
        const ByteOffset at = doc.text().clip(ByteOffset{rng.below(size + 1)});
        (void)doc.createAnchor(at, i % 2 == 0 ? Bias::Left : Bias::Right).value();
    }
    return doc;
}

ByteOffset lineStart(const Document& doc, std::uint32_t line) {
    return doc.text().lineInfo(LineIndex{line})->content.begin;
}

// What the editor does after each keystroke: pointAt, 60 visible lines and the anchors on them.
std::uint64_t paintQueries(const DocumentSnapshot& snapshot, ByteOffset caretAt) {
    const Point point = snapshot.text.pointAt(caretAt);
    const std::uint32_t first = point.line.value() >= 30 ? point.line.value() - 30 : 0;
    Result<LineCursor> lines = snapshot.text.linesFrom(LineIndex{first});
    LineInfo line;
    ByteOffset begin{0};
    ByteOffset end{0};
    for (int visible = 0; visible < 60 && lines->next(line); ++visible) {
        if (visible == 0) {
            begin = line.content.begin;
        }
        end = line.content.end;
    }
    return end.value() + snapshot.anchors.resolveRange(ByteRange{begin, end}).size();
}

TEST(DocumentPerf, TypingLatency) {
    const int ops = kFullScale ? 100'000 : 2'000;
    ManualClock clock;
    Document doc = loadDocument(clock, kDocumentBytes, kAnchors, 7);
    SplitMix64 rng(5);
    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(ops));
    ByteOffset cursor = lineStart(doc, static_cast<std::uint32_t>(rng.below(doc.text().lineCount())));
    std::uint64_t checksum = 0;
    for (int op = 0; op < ops; ++op) {
        const std::uint64_t roll = rng.below(100);
        if (roll == 0) {
            cursor = lineStart(doc, static_cast<std::uint32_t>(rng.below(doc.text().lineCount())));
        }
        const bool backspace = roll >= 85 && cursor.value() > 0;
        const char typed[2] = {static_cast<char>('a' + rng.below(26)), 0};
        clock.advance(50ms);
        const auto start = Clock::now();
        ByteOffset next = cursor;
        if (backspace) {
            next = doc.text().clip(ByteOffset{cursor.value() - 1});
            const auto change = doc.replace(ByteRange{next, cursor}, "",
                                            EditContext{EditKind::DeleteBackward, caret(cursor), caret(next)});
            if (!change) {
                FAIL() << change.error().describe();
            }
        } else {
            next = cursor + 1;
            const auto change = doc.replace(ByteRange{cursor, cursor}, std::string_view(typed, 1),
                                            EditContext{EditKind::Typing, caret(cursor), caret(next)});
            if (!change) {
                FAIL() << change.error().describe();
            }
        }
        cursor = next;
        checksum += paintQueries(doc.snapshot(), cursor);
        samples.push_back(elapsedNs(start));
    }
    const Percentiles p = percentiles(samples);
    std::printf("[text perf] DocumentPerf.TypingLatency (%s, %zu-byte document, %u anchors, %d ops, %u transactions "
                "kept): p50 %.1f us, p99 %.1f us, max %.1f us (checksum %llu)\n",
                kAssertBudgets ? "budgeted" : "report-only", kDocumentBytes, kAnchors, ops,
                doc.undoState().transactions, p.p50us, p.p99us, p.maxUs, static_cast<unsigned long long>(checksum));
    ASSERT_TRUE(doc.checkInvariants().has_value());
    if (kAssertBudgets) {
        // Budget: B5 (text-side sub-budget, ADR 0013 D9 extended by ADR 0017 decision 12).
        EXPECT_LE(p.p99us, 250.0) << "keystroke p99 above the 0.25 ms text sub-budget of B5";
    }
}

// Unique SumTree nodes reachable from `tree` and not yet in `seen`, added to `seen`; their bytes.
template<class Tree>
std::size_t addNodes(const Tree& tree, std::unordered_set<const void*>& seen) {
    std::size_t bytes = 0;
    std::vector<const typename Tree::Node*> stack;
    if (tree.rootNode() != nullptr) {
        stack.push_back(tree.rootNode());
    }
    while (!stack.empty()) {
        const typename Tree::Node* node = stack.back();
        stack.pop_back();
        if (!seen.insert(node).second) {
            continue; // Shared: its subtree is counted already.
        }
        if (node->height == 0) {
            bytes += sizeof(typename Tree::Leaf);
            continue;
        }
        bytes += sizeof(typename Tree::Internal);
        const auto* internal = static_cast<const typename Tree::Internal*>(node);
        for (std::size_t i = 0; i < internal->count; ++i) {
            stack.push_back(internal->children[i].get());
        }
    }
    return bytes;
}

std::size_t addAnchorNodes(const AnchorSet& set, std::unordered_set<const void*>& seen) {
    return addNodes(set.classTreeForTesting(Bias::Left), seen) + addNodes(set.classTreeForTesting(Bias::Right), seen) +
           addNodes(set.indexForTesting(), seen);
}

// The bytes the history retains beyond the live text and anchors: every node (and any dropped node
// still waiting to be freed) and the open transaction's compose base.
std::size_t historyBytes(const Document& doc) {
    std::unordered_set<const void*> seen;
    (void)addNodes(doc.text().pieceTree(), seen);
    (void)addAnchorNodes(doc.anchors(), seen);
    const frost::text::detail::UndoTree& u = doc.undoTreeForTesting();
    std::size_t bytes = addNodes(u.composeBase, seen);
    const auto addNode = [&](const frost::text::detail::UndoNode& node) {
        bytes += sizeof(node) + node.children.capacity() * sizeof(std::uint64_t) +
                 node.regions.capacity() * sizeof(frost::text::detail::Region);
        for (const frost::text::detail::Region& r : node.regions) {
            bytes += addNodes(r.removed, seen) + addNodes(r.inserted, seen);
        }
        bytes += addAnchorNodes(node.anchorsBefore, seen);
        if (node.hasAnchorsAfter) {
            bytes += addAnchorNodes(node.anchorsAfter, seen);
        }
    };
    for (const auto& [id, node] : u.nodes) {
        addNode(node);
    }
    for (const auto& handle : u.dropped) {
        addNode(handle.mapped());
    }
    return bytes;
}

// The anchors in the closed `ranges` of `before` whose offset in `after` differs from mapping
// them through `edits`: those the restore scan moved.
std::size_t restoredAnchors(const DocumentSnapshot& before, const DocumentSnapshot& after,
                            std::span<const RawEdit> edits) {
    std::size_t moved = 0;
    for (const RawEdit& e : edits) {
        for (const ResolvedAnchor& a : before.anchors.resolveRange(e.range)) {
            const Result<ByteOffset> now = after.anchors.resolve(a.id);
            if (now && *now != frost::text::mapOffset(a.offset, a.bias, edits)) {
                ++moved;
            }
        }
    }
    return moved;
}

TEST(DocumentPerf, UndoRedoLatency) {
    const std::uint32_t runs = kFullScale ? 200 : 20;
    const std::uint32_t deleteRuns = kFullScale ? 50 : 5;
    const std::uint32_t keystrokes = kFullScale ? 1000 : 100;
    const std::size_t pasteBytes = kFullScale ? std::size_t{64} << 10 : std::size_t{4} << 10;
    const std::uint64_t deletionBytes = kFullScale ? std::uint64_t{4} << 20 : std::uint64_t{256} << 10;
    ManualClock clock;
    Document doc = loadDocument(clock, kDocumentBytes, kAnchors, 8);
    SplitMix64 rng(6);
    for (std::uint32_t run = 0; run < runs; ++run) {
        ByteOffset cursor = lineStart(doc, static_cast<std::uint32_t>(rng.below(doc.text().lineCount())));
        for (std::uint32_t k = 0; k < keystrokes; ++k) {
            clock.advance(50ms);
            const char typed[2] = {static_cast<char>('a' + rng.below(26)), 0};
            (void)doc
                .replace(ByteRange{cursor, cursor}, std::string_view(typed, 1),
                         EditContext{EditKind::Typing, caret(cursor), caret(cursor + 1)})
                .value();
            cursor = cursor + 1;
        }
        doc.breakCoalescing();
    }
    const std::size_t typingRetained = historyBytes(doc);
    // Coalesced backspace runs (each removes `keystrokes` characters before a line start).
    for (std::uint32_t run = 0; run < deleteRuns; ++run) {
        ByteOffset cursor = lineStart(doc, 1 + static_cast<std::uint32_t>(rng.below(doc.text().lineCount() - 1)));
        for (std::uint32_t k = 0; k < keystrokes && cursor.value() > 0; ++k) {
            clock.advance(50ms);
            const ByteOffset next = doc.text().clip(ByteOffset{cursor.value() - 1});
            (void)doc
                .replace(ByteRange{next, cursor}, "", EditContext{EditKind::DeleteBackward, caret(cursor), caret(next)})
                .value();
            cursor = next;
        }
        doc.breakCoalescing();
    }
    const std::size_t deleteRetained = historyBytes(doc) - typingRetained;
    const std::string paste = codeLike(pasteBytes);
    const ByteOffset pasteAt = lineStart(doc, static_cast<std::uint32_t>(rng.below(doc.text().lineCount())));
    (void)doc.replace(ByteRange{pasteAt, pasteAt}, paste, EditContext{EditKind::Paste, caret(pasteAt), {}}).value();
    const ByteOffset cutFrom = doc.text().clip(ByteOffset{rng.below(doc.text().size().value() - deletionBytes)});
    const ByteOffset cutTo = doc.text().clip(cutFrom + deletionBytes);
    (void)doc.replace(ByteRange{cutFrom, cutTo}, "", EditContext{EditKind::Other, caret(cutFrom), caret(cutFrom)})
        .value();
    const std::uint32_t transactions = doc.undoState().transactions;
    ASSERT_EQ(transactions, runs + deleteRuns + 2);
    const std::size_t retained = historyBytes(doc);

    std::vector<std::int64_t> samples;
    std::uint64_t checksum = 0;
    struct Heavy {
        const char* name;
        std::int64_t ns;
        std::size_t moved;
    };
    std::vector<Heavy> heavy;
    const auto timed = [&](bool undo, const char* heavyName) {
        const DocumentSnapshot before = doc.snapshot();
        const auto start = Clock::now();
        const Result<ChangeSet> change = undo ? doc.undo() : doc.redo();
        const DocumentSnapshot after = doc.snapshot();
        if (change && !change->edits.empty()) {
            checksum += after.text.pointAt(change->edits.front().range.begin).line.value();
        }
        const std::int64_t ns = elapsedNs(start);
        ASSERT_TRUE(change.has_value()) << change.error().describe();
        samples.push_back(ns);
        if (heavyName != nullptr) {
            heavy.push_back(Heavy{heavyName, ns, restoredAnchors(before, after, change->edits)});
        }
    };
    for (std::uint32_t i = 0; i < transactions; ++i) {
        timed(true, i == 0 ? "deletion undo" : i == 1 ? "paste undo" : nullptr);
    }
    for (std::uint32_t i = 0; i < transactions; ++i) {
        timed(false, i == transactions - 2 ? "paste redo" : i == transactions - 1 ? "deletion redo" : nullptr);
    }
    const Percentiles p = percentiles(samples);
    std::printf("[text perf] DocumentPerf.UndoRedoLatency (%s, %zu-byte document, %u anchors, %u runs x %u keystrokes, "
                "%zu-byte paste, %llu-byte deletion): %zu ops, p50 %.1f us, p99 %.1f us, max %.1f us (checksum %llu)\n",
                kAssertBudgets ? "budgeted" : "report-only", kDocumentBytes, kAnchors, runs, keystrokes, pasteBytes,
                static_cast<unsigned long long>(deletionBytes), samples.size(), p.p50us, p.p99us, p.maxUs,
                static_cast<unsigned long long>(checksum));
    for (const Heavy& h : heavy) {
        std::printf("[text perf]   %s: %.1f us, %zu anchors restored\n", h.name, microseconds(h.ns), h.moved);
    }
    // History memory: right after recording, per kind of transaction, and after every node was
    // undone and redone once (each then also holds its anchorsAfter and a replaced anchorsBefore).
    const std::size_t navigated = historyBytes(doc);
    const auto report = [](const char* what, std::size_t bytes, std::uint32_t count) {
        const double each = static_cast<double>(bytes) / static_cast<double>(count);
        std::printf("[text perf]   history %s: %zu bytes of nodes for %u transactions (%.0f bytes each); 1000 such "
                    "transactions would hold %.2f MB, %.2f %% of B19's 80 MB\n",
                    what, bytes, count, each, each * 1000.0 / 1e6, each * 1000.0 / 80e6 * 100.0);
    };
    report("of typing runs", typingRetained, runs);
    report("of backspace runs", deleteRetained, deleteRuns);
    report("as recorded", retained, transactions);
    report("after undoing and redoing everything", navigated, transactions);
    ASSERT_TRUE(doc.checkInvariants().has_value());
    if (kAssertBudgets) {
        // Budget: B5 (text-side sub-budget, ADR 0017 decision 12).
        EXPECT_LE(p.p99us, 250.0) << "undo/redo p99 above the 0.25 ms text sub-budget of B5";
    }
}

TEST(DocumentPerf, AnchorsAndCarets) {
    ManualClock clock;
    const std::uint32_t manyAnchors = kFullScale ? 100'000 : 10'000;
    {
        // resolve with many anchors.
        Document doc = loadDocument(clock, kDocumentBytes, manyAnchors, 9);
        SplitMix64 rng(10);
        std::vector<std::int64_t> samples;
        std::uint64_t checksum = 0;
        for (int i = 0; i < 20'000; ++i) {
            const AnchorId id{1 + rng.below(manyAnchors)};
            const auto start = Clock::now();
            checksum += doc.anchors().resolve(id).value().value();
            samples.push_back(elapsedNs(start));
        }
        const Percentiles p = percentiles(samples);
        std::printf("[text perf] DocumentPerf.AnchorsAndCarets: resolve with %u anchors: p50 %.2f us, p99 %.2f us "
                    "(checksum %llu)\n",
                    manyAnchors, p.p50us, p.p99us, static_cast<unsigned long long>(checksum));
        // One edit collapsing 1000 anchors at distinct offsets.
        const ByteOffset from = doc.text().clip(ByteOffset{doc.text().size().value() / 2});
        for (std::uint64_t i = 0; i < 1000; ++i) {
            (void)doc.createAnchor(doc.text().clip(from + 2 * i), i % 2 == 0 ? Bias::Left : Bias::Right).value();
        }
        const ByteOffset to = doc.text().clip(from + 2000);
        const auto start = Clock::now();
        (void)doc.replace(ByteRange{from, to}, "", EditContext{}).value();
        const std::int64_t collapse = elapsedNs(start);
        const auto undoStart = Clock::now();
        (void)doc.undo().value();
        const std::int64_t restore = elapsedNs(undoStart);
        std::printf("[text perf]   one edit collapsing 1000 anchors: %.1f us; its undo (restoring them): %.1f us\n",
                    microseconds(collapse), microseconds(restore));
    }
    {
        // The undo of a paste holding anchors created after it: the restore scan meets them in the
        // paste's closed range and rejects each by id in O(1) (AnchorSet::find).
        Document doc = loadDocument(clock, kDocumentBytes, kAnchors, 13);
        const std::string paste = codeLike(std::size_t{64} << 10);
        const ByteOffset pasteAt = lineStart(doc, 1000);
        (void)doc.replace(ByteRange{pasteAt, pasteAt}, paste, EditContext{EditKind::Paste, caret(pasteAt), {}}).value();
        constexpr std::uint32_t kInside = 10'000;
        for (std::uint32_t i = 0; i < kInside; ++i) {
            (void)doc.createAnchor(doc.text().clip(pasteAt + (std::uint64_t{i} * 6) % paste.size()), Bias::Left)
                .value();
        }
        const auto start = Clock::now();
        (void)doc.undo().value();
        const std::int64_t ns = elapsedNs(start);
        std::printf("[text perf]   undo of a 64 KiB paste holding %u anchors created after it (among %u): %.1f us\n",
                    kInside, kAnchors + kInside, microseconds(ns));
    }
    {
        // The history cap dropping an old redo branch: 999 sealed transactions, undone to the root,
        // then keystrokes on a new branch; the second one drops the old branch (its nodes are freed
        // a few per later operation, UndoTree::releaseDropped).
        Document doc = loadDocument(clock, std::size_t{1} << 20, kAnchors, 14);
        SplitMix64 rng(15);
        for (int i = 0; i < 999; ++i) {
            const ByteOffset at = doc.text().clip(ByteOffset{rng.below(doc.text().size().value())});
            (void)doc.replace(ByteRange{at, at}, "x", EditContext{}).value();
        }
        for (int i = 0; i < 999; ++i) {
            (void)doc.undo().value();
        }
        std::vector<std::int64_t> samples;
        for (int i = 0; i < 6; ++i) {
            const ByteOffset at = lineStart(doc, 10 + static_cast<std::uint32_t>(i));
            const auto start = Clock::now();
            (void)doc.replace(ByteRange{at, at}, "y", EditContext{}).value();
            samples.push_back(elapsedNs(start));
        }
        std::printf("[text perf]   keystrokes after undoing 999 transactions, the second dropping them:");
        for (const std::int64_t ns : samples) {
            std::printf(" %.1f", microseconds(ns));
        }
        std::printf(" us (%zu dropped nodes still to free)\n", doc.undoTreeForTesting().dropped.size());
        ASSERT_TRUE(doc.checkInvariants().has_value());
    }
    {
        // A backspace next to 10k anchors at one offset.
        Document doc = loadDocument(clock, std::size_t{1} << 20, 0, 11);
        const ByteOffset at = lineStart(doc, 100);
        for (std::uint32_t i = 0; i < 10'000; ++i) {
            (void)doc.createAnchor(at, i % 2 == 0 ? Bias::Left : Bias::Right).value();
        }
        const ByteOffset before = doc.text().clip(ByteOffset{at.value() - 1});
        const auto start = Clock::now();
        (void)doc.replace(ByteRange{before, at}, "", EditContext{EditKind::DeleteBackward, caret(at), caret(before)})
            .value();
        const std::int64_t ns = elapsedNs(start);
        std::printf("[text perf]   backspace next to 10000 anchors at one offset: %.1f us\n", microseconds(ns));
    }
    {
        // Relabels under adversarial inserts: each new anchor goes after the previous one at one
        // offset and before a fixed neighbour, so the free label gap halves every time.
        AnchorSet set = AnchorSet(0, ByteOffset{1000}).withAnchor(ByteOffset{501}, Bias::Right).first;
        const std::uint32_t inserts = kFullScale ? 100'000 : 10'000;
        const auto start = Clock::now();
        for (std::uint32_t i = 0; i < inserts; ++i) {
            set = set.withAnchor(ByteOffset{500}, Bias::Right).first;
        }
        const std::int64_t ns = elapsedNs(start);
        std::printf("[text perf]   %u anchors inserted between a run at one offset and a fixed neighbour: %.1f ms, "
                    "%llu relabels of %llu entries\n",
                    inserts, static_cast<double>(ns) / 1e6,
                    static_cast<unsigned long long>(set.statsForTesting().relabels),
                    static_cast<unsigned long long>(set.statsForTesting().relabelledEntries));
    }
    {
        // A 10k-caret Typing batch and its undo.
        Document doc = loadDocument(clock, kDocumentBytes, kAnchors, 12);
        const std::uint32_t carets = std::min<std::uint32_t>(kFullScale ? 10'000 : 2'000, doc.text().lineCount());
        const std::uint32_t step = doc.text().lineCount() / carets;
        std::vector<Edit> edits;
        std::vector<Selection> selections;
        for (std::uint32_t i = 0; i < carets; ++i) {
            const ByteOffset at = lineStart(doc, i * step);
            edits.push_back(Edit{ByteRange{at, at}, "x"});
            selections.push_back(Selection{at, at});
        }
        const SelectionSet before = SelectionSet::fromSelections(selections, 0);
        const auto start = Clock::now();
        const ChangeSet change = doc.edit(edits, EditContext{EditKind::Typing, before, {}}).value();
        const SelectionSet after = before.transformed(change.edits);
        const std::int64_t ns = elapsedNs(start);
        const auto undoStart = Clock::now();
        (void)doc.undo().value();
        const std::int64_t undoNs = elapsedNs(undoStart);
        std::printf("[text perf]   %u-caret Typing batch (edit, anchors, SelectionSet::transformed): %.2f ms, "
                    "%zu carets after; its undo: %.2f ms\n",
                    carets, static_cast<double>(ns) / 1e6, after.size(), static_cast<double>(undoNs) / 1e6);
    }
}

} // namespace
