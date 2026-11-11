// SPDX-License-Identifier: Apache-2.0
//
// SyntaxSession (ADR 0023): incremental parses equal fresh parses after edits, tree-sitter rows
// equal the editor's lines for CRLF and lone CR text (D3), the size cut-off with its hysteresis,
// the worker fallback with stale highlights and queued edits (D4/D5), resyncs, the parse-ready
// hook, cancellation by open, and the worker's time limit.

#include "frost/core/clock.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_buffer.hpp"

#include "syntax/syntax_test_support.hpp"
#include "tree_state.hpp"
#include "ts.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ByteOffset;
using frost::sched::SimExecutor;
using frost::syntax::LanguageId;
using frost::syntax::LanguageRegistry;
using frost::syntax::SyntaxOptions;
using frost::syntax::SyntaxReason;
using frost::syntax::SyntaxSession;
using frost::syntax::SyntaxState;
using frost::syntax::SyntaxTree;
using frost::syntax::testing::freshDump;
using frost::syntax::testing::kindAt;
using frost::syntax::testing::sample;
using frost::text::ByteRange;
using frost::text::ChangeSet;
using frost::text::Document;
using frost::text::EditContext;
using frost::text::TextBufferOptions;

const LanguageRegistry& registry() {
    static const LanguageRegistry instance;
    return instance;
}

ByteRange range(std::uint64_t begin, std::uint64_t end) { return ByteRange{ByteOffset{begin}, ByteOffset{end}}; }

// Replaces [begin, end) of the document and returns the change set.
ChangeSet replace(Document& doc, std::uint64_t begin, std::uint64_t end, std::string_view text) {
    return doc.replace(range(begin, end), text, EditContext{}).value();
}

Document makeDocument(std::string_view text, std::uint32_t maxPieceBytes = 4096) {
    return Document::fromUtf8(std::string(text), TextBufferOptions{.maxPieceBytes = maxPieceBytes}).value();
}

// Every node's start and end point equals the editor's point (Snapshot::pointAt) at its byte.
void expectPointsMatchEditorLines(const SyntaxTree& tree) {
    ASSERT_FALSE(tree.empty());
    const frost::text::Snapshot text = tree.text();
    frost::syntax::detail::TreeCursor cursor(ts_tree_root_node(tree.state()->tree.get()));
    std::uint32_t nodes = 0;
    for (;;) {
        const TSNode node = ts_tree_cursor_current_node(cursor.get());
        ++nodes;
        for (const auto& [byte, point] : {std::pair{ts_node_start_byte(node), ts_node_start_point(node)},
                                          std::pair{ts_node_end_byte(node), ts_node_end_point(node)}}) {
            const frost::text::Point editor = text.pointAt(ByteOffset{byte});
            ASSERT_EQ(point.row, editor.line.value()) << "byte " << byte << " of " << ts_node_type(node);
            ASSERT_EQ(point.column, editor.column.value()) << "byte " << byte << " of " << ts_node_type(node);
        }
        if (ts_tree_cursor_goto_first_child(cursor.get())) {
            continue;
        }
        while (!ts_tree_cursor_goto_next_sibling(cursor.get())) {
            if (!ts_tree_cursor_goto_parent(cursor.get())) {
                EXPECT_GT(nodes, 1u);
                return;
            }
        }
    }
}

// --- Incremental parsing ---

TEST(SyntaxIncremental, EditedTreeEqualsFreshParse) {
    // Small pieces so tree-sitter reads many chunks; inserts include CRLF and lone CRs.
    const std::vector<std::string_view> inserts = {"a", " ", "\n", "(", "}", "\"", "/*", "// c\n", "\r\n", "\r", "x1"};
    for (const LanguageId id : frost::syntax::kAllLanguages) {
        SimExecutor sim;
        Document doc = makeDocument(sample(id), 16);
        SyntaxSession session(registry(), sim, sim);
        session.open(id, doc.text());
        ASSERT_EQ(session.state(), SyntaxState::Current) << frost::syntax::toString(id);
        std::uint64_t state = 12345;
        for (int step = 0; step < 24; ++step) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            const std::uint64_t size = doc.text().size().value();
            const std::uint64_t at = (state >> 33) % (size + 1);
            const std::uint64_t len = (state >> 20) % 4;
            const std::uint64_t end = std::min(size, at + (step % 3 == 0 ? len : 0));
            const std::string_view text = inserts[(state >> 40) % inserts.size()];
            const ChangeSet change = replace(doc, at, end, step % 5 == 4 ? std::string_view{} : text);
            session.apply(change, doc.text());
            ASSERT_EQ(session.state(), SyntaxState::Current) << frost::syntax::toString(id) << " step " << step;
            ASSERT_EQ(session.tree().dumpForTesting(true), freshDump(id, doc.text().text()))
                << frost::syntax::toString(id) << " step " << step << " text:\n"
                << doc.text().text();
        }
        EXPECT_EQ(session.stats().fullParses, 1u);
        EXPECT_EQ(session.stats().resyncs, 0u);
        EXPECT_GT(session.stats().editsApplied, 0u);
    }
}

TEST(SyntaxIncremental, MultiChangeSetFromUndo) {
    SimExecutor sim;
    Document doc = makeDocument(sample(LanguageId::Cpp), 32);
    SyntaxSession session(registry(), sim, sim);
    session.open(LanguageId::Cpp, doc.text());
    session.apply(replace(doc, 10, 10, "int added = 1;\n"), doc.text());
    session.apply(replace(doc, 100, 120, ""), doc.text());
    // Undo produces a change set with its own TextChanges; the session follows it too.
    const ChangeSet undone = doc.undo().value();
    session.apply(undone, doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Cpp, doc.text().text()));
    EXPECT_EQ(session.stats().resyncs, 0u);
}

// --- Rows: CRLF and lone CR (D3) ---

TEST(SyntaxRows, CrlfAndLoneCrRowsMatchEditorLines) {
    SimExecutor sim;
    const std::string_view text = "int a;\r\nint b;\rint c;\nint d;\r\r\nint e;\r";
    Document doc = makeDocument(text);
    SyntaxSession session(registry(), sim, sim);
    session.open(LanguageId::C, doc.text());
    ASSERT_EQ(session.state(), SyntaxState::Current);
    const SyntaxTree tree = session.tree();
    EXPECT_EQ(tree.dumpForTesting(true), freshDump(LanguageId::C, text));
    expectPointsMatchEditorLines(tree);
    // `e` is on the editor's line 5 (0-based): CRLF, CR, LF, CR, CRLF are five breaks.
    const std::string all = tree.text().text();
    const std::size_t e = all.find('e');
    EXPECT_EQ(tree.text().pointAt(ByteOffset{e}).line.value(), 5u);
    // Edits that create and split CRLF pairs keep the rows right.
    const auto check = [&](const ChangeSet& change, std::string_view what) {
        session.apply(change, doc.text());
        EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::C, doc.text().text())) << what;
        expectPointsMatchEditorLines(session.tree());
    };
    check(replace(doc, 6, 6, "\r"), "a lone CR before a CRLF");  // "int a;\r\r\nint b;\r..."
    check(replace(doc, 16, 16, "\n"), "a lone CR becomes CRLF"); // "...int b;\r\nint c;..."
    check(replace(doc, 8, 9, ""), "a CRLF loses its LF");        // "int a;\r\rint b;..."
    check(replace(doc, 7, 7, "\n"), "a CR gains an LF");         // "int a;\r\n\rint b;..."
}

TEST(SyntaxRows, CrlfAcrossAChunkBoundaryIsNotALoneCr) {
    SimExecutor sim;
    // With 4-byte pieces "[1,\r" and "\n2]" are separate chunks: the reader must look ahead.
    const std::string_view text = "[1,\r\n2,\r3]";
    Document doc = makeDocument(text, 4);
    ASSERT_EQ(doc.text().chunkAt(ByteOffset{0}), "[1,\r"); // The pair really is split.
    SyntaxSession session(registry(), sim, sim);
    session.open(LanguageId::Json, doc.text());
    ASSERT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Json, text));
    expectPointsMatchEditorLines(session.tree());
}

// --- The size cut-off ---

TEST(SyntaxSize, OffAboveTheThresholdWithHysteresis) {
    SimExecutor sim;
    SyntaxOptions options;
    options.maxDocumentBytes = 4096;
    options.reenableBytes = 4000;
    {
        SyntaxSession session(registry(), sim, sim, options);
        session.open(LanguageId::Json, frost::text::TextBuffer::fromUtf8(std::string(4097, ' ')).value().current());
        EXPECT_EQ(session.state(), SyntaxState::Off);
        EXPECT_EQ(session.reason(), SyntaxReason::TooLarge);
        EXPECT_TRUE(session.tree().empty());
        EXPECT_EQ(session.stats().fullParses, 0u);
    }
    Document doc = makeDocument(std::string(4090, ' ') + "[1]");
    SyntaxSession session(registry(), sim, sim, options);
    session.open(LanguageId::Json, doc.text());
    ASSERT_EQ(session.state(), SyntaxState::Current);      // 4093 bytes: at most 4096 parses.
    session.apply(replace(doc, 0, 0, "    "), doc.text()); // 4097 bytes
    EXPECT_EQ(session.state(), SyntaxState::Off);
    EXPECT_EQ(session.reason(), SyntaxReason::TooLarge);
    EXPECT_TRUE(session.tree().empty());
    session.apply(replace(doc, 0, 50, ""), doc.text()); // 4047 bytes: above reenableBytes, stays Off
    EXPECT_EQ(session.state(), SyntaxState::Off);
    EXPECT_EQ(session.text().version(), doc.text().version());
    session.apply(replace(doc, 0, 100, ""), doc.text()); // 3947 bytes: reopens
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Json, doc.text().text()));
}

TEST(SyntaxSize, DefaultThresholdIsFiftyMegabytes) {
    static_assert(frost::syntax::kDefaultMaxDocumentBytes == 50'000'000);
    SimExecutor sim;
    SyntaxSession session(registry(), sim, sim);
    // NOLINTNEXTLINE(bugprone-string-constructor): Deliberate limit+1 allocation.
    const frost::text::TextBuffer big = frost::text::TextBuffer::fromUtf8(std::string(50'000'001, ' ')).value();
    session.open(LanguageId::C, big.current());
    EXPECT_EQ(session.state(), SyntaxState::Off);
    EXPECT_EQ(session.reason(), SyntaxReason::TooLarge);
    EXPECT_EQ(session.stats().syncAttempts, 0u);
    EXPECT_EQ(session.stats().workerParses, 0u);
    EXPECT_EQ(sim.readyCount(), 0u);
}

// --- The worker fallback ---

struct HookCounter {
    int calls = 0;
    static void onReady(void* context) noexcept { ++static_cast<HookCounter*>(context)->calls; }
};

SyntaxOptions workerOnly(HookCounter* counter = nullptr) {
    SyntaxOptions options;
    options.syncParseBudget = 0ms; // Every parse goes to the worker.
    if (counter != nullptr) {
        options.onParseReady = &HookCounter::onReady;
        options.onParseReadyContext = counter;
    }
    return options;
}

TEST(SyntaxWorker, ParsesOnTheWorkerAndKeepsTheStaleTreeMeanwhile) {
    SimExecutor sim;
    HookCounter hook;
    Document doc = makeDocument(sample(LanguageId::C));
    SyntaxSession session(registry(), sim, sim, workerOnly(&hook));
    session.open(LanguageId::C, doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Pending);
    EXPECT_TRUE(session.parseInFlight());
    EXPECT_TRUE(session.tree().empty());
    EXPECT_FALSE(session.poll() && !session.tree().empty()); // Nothing has run yet.
    sim.runUntilIdle();
    EXPECT_GE(hook.calls, 1);
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_FALSE(session.parseInFlight());
    EXPECT_EQ(session.stats().syncAttempts, 0u);
    EXPECT_EQ(session.stats().workerParses, 1u);
    EXPECT_EQ(session.stats().workerCompleted, 1u);
    const std::string printfBefore = kindAt(session.tree(), "printf");
    EXPECT_EQ(printfBefore, "function.call");

    // An edit: the old tree, edited into the new coordinates, serves highlights until the
    // worker lands; "printf" moved by two bytes and is still a call.
    session.apply(replace(doc, 0, 0, "\n\n"), doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Stale);
    const SyntaxTree stale = session.tree();
    ASSERT_FALSE(stale.empty());
    EXPECT_TRUE(stale.stale());
    EXPECT_EQ(stale.version(), doc.text().version());
    EXPECT_LT(stale.parsedVersion(), stale.version());
    EXPECT_EQ(kindAt(stale, "printf"), "function.call");
    EXPECT_EQ(kindAt(stale, "/** Doc comment. */"), "comment.doc");
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::C, doc.text().text()));
}

TEST(SyntaxWorker, EditsDuringAWorkerParseCatchUp) {
    SimExecutor sim;
    Document doc = makeDocument(sample(LanguageId::Cpp));
    SyntaxSession session(registry(), sim, sim, workerOnly());
    session.open(LanguageId::Cpp, doc.text());
    // Three edits land before the worker runs: they queue behind the open's job.
    session.apply(replace(doc, 0, 0, "int first;\n"), doc.text());
    session.apply(replace(doc, 5, 10, "x"), doc.text());
    session.apply(replace(doc, doc.text().size().value(), doc.text().size().value(), "// end\n"), doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Pending);
    EXPECT_EQ(session.stats().workerParses, 1u);
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    // The first result is stale: a catch-up reparse starts from it.
    EXPECT_EQ(session.state(), SyntaxState::Stale);
    EXPECT_EQ(session.stats().catchUps, 1u);
    EXPECT_FALSE(session.tree().empty());
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.stats().workerParses, 2u);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Cpp, doc.text().text()));
}

std::string bigC(int functions) {
    std::string text;
    for (int i = 0; i < functions; ++i) {
        text += "static int function" + std::to_string(i) + "(int a, int b) {\n    return a * " + std::to_string(i) +
                " + b; /* body */\n}\n";
    }
    return text;
}

TEST(SyntaxWorker, SynchronousOverrunMovesToTheWorker) {
    // A real clock and a 1 ns budget: the synchronous attempt overruns at its first progress check
    // and the same parse resumes on the worker.
    SimExecutor sim;
    SyntaxOptions options;
    options.syncParseBudget = 1ns;
    const std::string text = bigC(2000);
    Document doc = makeDocument(text);
    SyntaxSession session(registry(), sim, frost::systemClock(), options);
    session.open(LanguageId::C, doc.text());
    EXPECT_EQ(session.stats().syncAttempts, 1u);
    EXPECT_EQ(session.stats().syncOverruns, 1u);
    EXPECT_EQ(session.stats().workerParses, 1u);
    EXPECT_EQ(session.state(), SyntaxState::Pending);
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    ASSERT_EQ(session.state(), SyntaxState::Current);
    EXPECT_FALSE(session.tree().hasError());
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::C, text));
    // An incremental edit also overruns (1 ns) and resumes from the old tree on the worker.
    session.apply(replace(doc, 0, 0, "int added;\n"), doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Stale);
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::C, doc.text().text()));
}

TEST(SyntaxWorker, WorkerTimeLimitFails) {
    SimExecutor sim;
    SyntaxOptions options = workerOnly();
    options.workerParseTimeLimit = 1ns;
    Document doc = makeDocument(bigC(500));
    SyntaxSession session(registry(), sim, frost::systemClock(), options);
    session.open(LanguageId::C, doc.text());
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Failed);
    EXPECT_EQ(session.reason(), SyntaxReason::TimeLimit);
    EXPECT_TRUE(session.tree().empty());
    EXPECT_EQ(session.stats().failures, 1u);
    // A failed session only records the text; open() starts over.
    session.apply(replace(doc, 0, 0, "x"), doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Failed);
    EXPECT_EQ(session.text().version(), doc.text().version());
}

TEST(SyntaxWorker, OpenCancelsTheRunningJob) {
    SimExecutor sim;
    HookCounter hook;
    SyntaxSession session(registry(), sim, sim, workerOnly(&hook));
    const frost::text::TextBuffer first = frost::text::TextBuffer::fromUtf8(std::string(sample(LanguageId::C))).value();
    const frost::text::TextBuffer second = frost::text::TextBuffer::fromUtf8("{\"a\": 1}").value();
    session.open(LanguageId::C, first.current());
    session.open(LanguageId::Json, second.current());
    sim.runUntilIdle();
    EXPECT_TRUE(session.poll());
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.language(), LanguageId::Json);
    EXPECT_EQ(session.tree().language(), LanguageId::Json);
    EXPECT_EQ(session.stats().workerCompleted, 1u);
    // Only the second open's notifier is attached: the abandoned job never calls the hook. The
    // second may call it twice (its parse and its query compilation).
    EXPECT_GE(hook.calls, 1);
    EXPECT_LE(hook.calls, 2);
}

// --- Resyncs and inactive sessions ---

TEST(SyntaxSessionState, VersionGapResyncs) {
    SimExecutor sim;
    Document doc = makeDocument(sample(LanguageId::Json));
    SyntaxSession session(registry(), sim, sim);
    session.open(LanguageId::Json, doc.text());
    (void)replace(doc, 1, 1, " "); // A change the session never sees...
    const ChangeSet next = replace(doc, 2, 2, " ");
    session.apply(next, doc.text()); // ... so this one's baseVersion does not match.
    EXPECT_EQ(session.stats().resyncs, 1u);
    EXPECT_EQ(session.state(), SyntaxState::Current);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Json, doc.text().text()));
    // A snapshot of another buffer resyncs too.
    Document other = makeDocument("[1, 2]");
    const ChangeSet foreign = replace(other, 0, 0, " ");
    session.apply(foreign, other.text());
    EXPECT_EQ(session.stats().resyncs, 2u);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Json, other.text().text()));
}

TEST(SyntaxSessionState, TextChangeSpanOverload) {
    SimExecutor sim;
    Document doc = makeDocument(sample(LanguageId::Markdown));
    SyntaxSession session(registry(), sim, sim);
    session.open(LanguageId::Markdown, doc.text());
    const ChangeSet change = replace(doc, 0, 0, "# New\n\n");
    session.apply(std::span<const frost::text::TextChange>(change.changes), doc.text());
    EXPECT_EQ(session.stats().resyncs, 0u);
    EXPECT_EQ(session.tree().dumpForTesting(true), freshDump(LanguageId::Markdown, doc.text().text()));
}

TEST(SyntaxSessionState, NoLanguageIsOff) {
    SimExecutor sim;
    Document doc = makeDocument("plain text");
    SyntaxSession session(registry(), sim, sim);
    EXPECT_EQ(session.state(), SyntaxState::Off);
    session.open(LanguageId::None, doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Off);
    EXPECT_EQ(session.reason(), SyntaxReason::NoLanguage);
    session.apply(replace(doc, 0, 0, "more "), doc.text());
    EXPECT_EQ(session.state(), SyntaxState::Off);
    EXPECT_EQ(session.text().version(), doc.text().version());
    EXPECT_TRUE(session.tree().empty());
    EXPECT_FALSE(session.poll());
}

TEST(SyntaxSessionState, MovedSessionKeepsWorking) {
    SimExecutor sim;
    Document doc = makeDocument(sample(LanguageId::Xml));
    SyntaxSession first(registry(), sim, sim, workerOnly());
    first.open(LanguageId::Xml, doc.text());
    SyntaxSession moved(std::move(first));
    sim.runUntilIdle();
    EXPECT_TRUE(moved.poll());
    EXPECT_EQ(moved.state(), SyntaxState::Current);
    EXPECT_EQ(moved.tree().dumpForTesting(true), freshDump(LanguageId::Xml, doc.text().text()));
}

} // namespace
