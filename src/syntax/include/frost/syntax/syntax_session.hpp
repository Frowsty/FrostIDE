// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax::SyntaxSession (ADR 0023, D3 to D6): incremental tree-sitter parsing of one
// document. It turns text ChangeSets into tree edits, reparses within a 2 ms synchronous budget on
// the typing path, moves an overrunning parse to a sched worker while the stale tree keeps the old
// highlights, and switches syntax off for documents above 50 MB.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/sched/lane.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/document.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace frost::syntax {

namespace detail {
struct SessionState;
} // namespace detail

// PLAN.md section 2 "Syntax": syntax is off above 50 MB (decimal, like F2).
inline constexpr std::uint64_t kDefaultMaxDocumentBytes = 50'000'000;

// Session options. At namespace scope so it can be a default argument.
struct SyntaxOptions {
    // The synchronous parse budget on the typing path (PLAN.md 3.4 step 2). Zero sends every parse
    // to the worker; IClock::Duration::max() parses inline to completion (tests). Budget: B5.
    IClock::Duration syncParseBudget = std::chrono::milliseconds(2);
    // A worker parse still running after this long is stopped: Failed(TimeLimit).
    IClock::Duration workerParseTimeLimit = std::chrono::seconds(30);
    // Syntax is Off(TooLarge) above this size; at most 0xFFFF'FFFE (tree-sitter's uint32 offsets).
    std::uint64_t maxDocumentBytes = kDefaultMaxDocumentBytes;
    // An Off(TooLarge) session switches back on at or below this size (hysteresis).
    std::uint64_t reenableBytes = 49'000'000;
    // The lane of worker parses and query compilation (ADR 0002: highlighting is UserVisible).
    sched::Lane parseLane = sched::Lane::UserVisible;
    // Called on the worker after a worker parse or a query compilation finished, so the owner can
    // poll(); never after the session is destroyed or reopened. Must be cheap, must not block and
    // must not call back into the session (typically it posts a UI wake-up). May be null.
    void (*onParseReady)(void* context) noexcept = nullptr;
    void* onParseReadyContext = nullptr;
};

// Where the session is. Off: no syntax (reason NoLanguage or TooLarge). Pending: the first parse
// is running and tree() is empty. Current: tree() is parsed at text(). Stale: a reparse is
// running and tree() is the previous parse edited to text(). Failed: the last parse hit a limit or
// a grammar error (reason TimeLimit or ParserError); tree() is empty until open().
enum class SyntaxState : std::uint8_t { Off, Pending, Current, Stale, Failed };
enum class SyntaxReason : std::uint8_t { None, NoLanguage, TooLarge, TimeLimit, ParserError };

// Counters for tests and the editor view's B5 marker.
struct SyntaxStats {
    std::uint64_t syncAttempts = 0;    // Synchronous parse attempts.
    std::uint64_t syncCompleted = 0;   // ... that finished within the budget.
    std::uint64_t syncOverruns = 0;    // ... that hit the budget and moved to the worker.
    std::uint64_t workerParses = 0;    // Parses handed to the worker.
    std::uint64_t workerCompleted = 0; // ... that landed with a tree.
    std::uint64_t catchUps = 0;        // Reparses started because edits arrived during a worker parse.
    std::uint64_t resyncs = 0;         // Full reparses after a version gap or foreign snapshot.
    std::uint64_t fullParses = 0;      // Parses without an old tree (open and resync).
    std::uint64_t failures = 0;        // Parses that ended Failed.
    std::uint64_t editsApplied = 0;    // tree-sitter edits applied to the view tree.
    IClock::Duration lastSyncParse{};  // Wall time of the last synchronous attempt.
    IClock::Duration maxSyncParse{};   // The longest synchronous attempt.
};

/*
 * The syntax state of one document. Owner thread only (the document's thread), like Document;
 * the SyntaxTree values it hands out are safe on any thread. Movable, not copyable: a running job
 * never points back at the session. Allocation failure terminates (noexcept).
 *
 * Lifetime: `registry`, `scheduler` and `clock` must outlive the session and every job it started
 * (a destroyed session's job finishes on its own; it holds only its parser, trees and snapshot).
 * The scheduler's enqueue must be callable from the owner thread (ThreadPool: any thread;
 * SimExecutor: its owner).
 *
 * Positions: tree-sitter rows are LF rows; the session presents every lone CR as LF to tree-sitter
 * (a one-byte substitution, so offsets are unchanged) and leaves CRLF alone, which makes tree-
 * sitter's rows equal the editor's LSP lines (ADR 0023 D3). Each D8 TextChange then maps field by
 * field to a TSInputEdit.
 */
class SyntaxSession {
public:
    SyntaxSession(const LanguageRegistry& registry, sched::IScheduler& scheduler, const IClock& clock = systemClock(),
                  SyntaxOptions options = {}) noexcept;
    // Stops a running job (it frees its parser and trees itself) and never blocks on it.
    ~SyntaxSession();
    SyntaxSession(SyntaxSession&& other) noexcept;
    SyntaxSession& operator=(SyntaxSession&& other) noexcept;
    SyntaxSession(const SyntaxSession&) = delete ("a session is the single writer of its trees");
    SyntaxSession& operator=(const SyntaxSession&) = delete ("a session is the single writer of its trees");

    // Starts over with `language` and `text`: stops any job, drops the tree, then parses (the
    // synchronous attempt, or the worker). Off(NoLanguage) for None, Off(TooLarge) above
    // maxDocumentBytes. Schedules query compilation on parseLane if the queries are not ready.
    // Budget: B5.
    void open(LanguageId language, const text::Snapshot& text) noexcept;

    /*
     * Follows a Document change from text() to `after`. If changes.baseVersion != text().version(),
     * changes.version != after.version(), or `after` is not from text()'s buffer, the session
     * resyncs (a full parse of `after`; stats().resyncs counts it): self-healing, never an error.
     * Above maxDocumentBytes it goes Off(TooLarge) and drops its trees; Off(TooLarge) at or below
     * reenableBytes reopens. Off(NoLanguage) and Failed only record `after`. Budget: B5.
     */
    void apply(const text::ChangeSet& changes, const text::Snapshot& after) noexcept;

    // The same for TextBuffer users: D8 changes in application order from text() to `after`.
    // Resyncs when the changes do not account for the size difference, after.version() is not
    // newer than text().version(), or `after` is from another buffer. Budget: B5.
    void apply(std::span<const text::TextChange> changes, const text::Snapshot& after) noexcept;

    // Collects a finished worker parse: applies the edits that arrived meanwhile and, if there were
    // any, starts the catch-up reparse. Returns true when tree() changed or the language's queries
    // became ready since the last call. Budget: B5.
    bool poll() noexcept;

    // The view tree, in text() coordinates; empty when Off, Failed, or before the first parse. O(1).
    [[nodiscard]] SyntaxTree tree() const noexcept;
    [[nodiscard]] SyntaxState state() const noexcept;
    [[nodiscard]] SyntaxReason reason() const noexcept;
    // True while a worker parse is running (or finished and not yet collected by poll()).
    [[nodiscard]] bool parseInFlight() const noexcept;
    [[nodiscard]] const SyntaxStats& stats() const noexcept;
    // The latest snapshot the session was given.
    [[nodiscard]] const text::Snapshot& text() const noexcept;
    [[nodiscard]] LanguageId language() const noexcept;

private:
    std::unique_ptr<detail::SessionState> m_state;
};

} // namespace frost::syntax
