// SPDX-License-Identifier: Apache-2.0
//
// parse.cpp: deadline-bounded, resumable tree-sitter parses (ADR 0023, D4).
/*
 * tree-sitter 0.27.1 has no timeout or cancellation flag; its only control is the progress
 * callback of ts_parser_parse_with_options, checked every OP_COUNT_PER_PARSER_CALLBACK_CHECK
 * (100) operations. Returning true halts the parse: ts_parser_parse returns NULL and the parser
 * keeps its state, and calling it again with the same input resumes (ts_parser_has_outstanding_parse
 * in lib/src/parser.c). A NULL result without a halt request is a scanner error.
 */

#include "parse.hpp"

namespace frost::syntax::detail {

namespace {

struct Progress {
    const IClock* clock = nullptr;
    std::optional<IClock::SteadyTime> deadline;
    const std::stop_token* stop = nullptr;
    const std::stop_token* schedulerStop = nullptr;
    bool hitDeadline = false;
    bool stopped = false;
};

bool onParseProgress(TSParseState* state) noexcept {
    auto& progress = *static_cast<Progress*>(state->payload);
    if ((progress.stop != nullptr && progress.stop->stop_requested()) ||
        (progress.schedulerStop != nullptr && progress.schedulerStop->stop_requested())) {
        progress.stopped = true;
        return true;
    }
    if (progress.deadline && progress.clock->steadyNow() >= *progress.deadline) {
        progress.hitDeadline = true;
        return true;
    }
    return false;
}

} // namespace

ParseJob::ParseJob(ParserPtr parserIn, text::Snapshot text, const TSTree* old) noexcept
    : parser(std::move(parserIn)), oldTree(old), input(std::move(text)) {
    tsInput = makeInput(input);
}

ParseResult runParse(ParseJob& job, const IClock& clock, std::optional<IClock::SteadyTime> deadline,
                     const std::stop_token* stop, const std::stop_token* schedulerStop) noexcept {
    Progress progress{&clock, deadline, stop, schedulerStop};
    TSParseOptions options{};
    options.payload = &progress;
    options.progress_callback = &onParseProgress;
    const IClock::SteadyTime start = clock.steadyNow();
    // On resume the parser ignores the old tree (it retained it on the first call).
    const TSTree* old = job.started ? nullptr : job.oldTree;
    job.started = true;
    TSTree* tree = ts_parser_parse_with_options(job.parser.get(), old, job.tsInput, options);
    ParseResult result;
    result.elapsed = clock.steadyNow() - start;
    if (tree != nullptr) {
        result.status = ParseStatus::Done;
        result.tree.reset(tree);
        job.started = false;
        job.oldTree = nullptr;
        job.ownedOldTree.reset();
        return result;
    }
    if (progress.hitDeadline && !progress.stopped) {
        result.status = ParseStatus::Deadline;
        return result;
    }
    ts_parser_reset(job.parser.get());
    job.started = false;
    result.status = progress.stopped ? ParseStatus::Stopped : ParseStatus::Failed;
    return result;
}

} // namespace frost::syntax::detail
