// SPDX-License-Identifier: Apache-2.0
//
// parse.hpp: one tree-sitter parse of a snapshot that can halt at a deadline and resume later,
// possibly on another thread (ADR 0023, D4). The session's synchronous attempt and its worker
// fallback both run a ParseJob through runParse.
#pragma once

#include "frost/core/clock.hpp"

#include "input.hpp"
#include "ts.hpp"

#include <optional>
#include <stop_token>

namespace frost::syntax::detail {

/*
 * A parse in progress. Heap-allocated and moved as a whole (unique_ptr), because tree-sitter keeps
 * pointers to the input payload between the calls of a halted parse. `oldTree` is the edited view
 * tree the parse may reuse; it is read by the first call only (the parser retains what it needs)
 * and owned by the job only when the job is handed to the worker before its first call.
 * Not thread-safe; one thread runs it at a time, and a halted job can move to another thread.
 */
struct ParseJob {
    ParserPtr parser;
    TreePtr ownedOldTree;
    const TSTree* oldTree = nullptr;
    InputSource input;
    TSInput tsInput{};
    bool started = false; // The parser holds an outstanding (halted) parse of `input`.

    ParseJob(ParserPtr parserIn, text::Snapshot text, const TSTree* old) noexcept;
    ParseJob(const ParseJob&) = delete ("tree-sitter holds pointers into the job");
    ParseJob& operator=(const ParseJob&) = delete ("tree-sitter holds pointers into the job");
};

enum class ParseStatus : std::uint8_t {
    Done,     // `tree` holds the result.
    Deadline, // The deadline passed; the job can resume with another runParse call.
    Stopped,  // A stop token fired; the parser was reset.
    Failed,   // tree-sitter gave up (a scanner error); the parser was reset.
};

struct ParseResult {
    ParseStatus status = ParseStatus::Failed;
    TreePtr tree;
    IClock::Duration elapsed{};
};

// Runs or resumes `job` until it finishes, `deadline` passes (none when nullopt) or a token stops.
// tree-sitter checks progress every 100 parser operations, so the overrun past the deadline is one
// such interval. Tokens may be null.
[[nodiscard]] ParseResult runParse(ParseJob& job, const IClock& clock, std::optional<IClock::SteadyTime> deadline,
                                   const std::stop_token* stop, const std::stop_token* schedulerStop) noexcept;

} // namespace frost::syntax::detail
