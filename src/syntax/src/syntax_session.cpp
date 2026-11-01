// SPDX-License-Identifier: Apache-2.0
//
// syntax_session.cpp: frost::syntax::SyntaxSession, the incremental parse state machine
// (ADR 0023, D3 to D6): edits to the view tree, the 2 ms synchronous attempt, the worker fallback
// with queued edits and the catch-up reparse, resync, and the size cut-off.
/*
 * The view tree (D5) is always in the coordinates of text(): the last parsed tree, copied
 * (ts_tree_copy, O(1)) and edited with every change since that parse. A published TSTree is never
 * edited; tree-sitter's subtree edit copies shared nodes on write, so readers of older SyntaxTrees
 * are unaffected.
 *
 * A worker job owns its parser, its input snapshot and (when it never ran synchronously) a copy of
 * the old tree; it never points back at the session. It stores its outcome in a shared JobSlot and
 * then calls the Notifier hook, so the owner's poll() always finds the result once woken. Edits
 * that arrive while it runs are queued as TSInputEdits and applied to its result in poll(), which
 * then starts one catch-up reparse; an in-flight parse is never cancelled for new edits, so
 * continuous typing in a big file cannot starve it. open(), resync, the size cut-off and the
 * destructor stop it through its stop_source.
 */

#include "frost/syntax/syntax_session.hpp"

#include "frost/core/assert.hpp"
#include "frost/core/mutex.hpp"
#include "frost/sched/task.hpp"

#include "input.hpp"
#include "language_state.hpp"
#include "parse.hpp"
#include "tree_state.hpp"

#include <algorithm>
#include <optional>
#include <stop_token>

namespace frost::syntax {

namespace detail {

namespace {

// tree-sitter offsets are uint32 and its end offset must be representable (ADR 0013 D6).
constexpr std::uint64_t kTreeSitterMaxBytes = 0xFFFF'FFFE;

// The wake-up hook, shared with running tasks; detached when the session reopens or dies.
struct Notifier {
    Mutex mutex;
    void (*hook)(void*) noexcept FROST_GUARDED_BY(mutex) = nullptr;
    void* context FROST_GUARDED_BY(mutex) = nullptr;

    void notify() noexcept {
        LockGuard lock(mutex);
        if (hook != nullptr) {
            hook(context);
        }
    }
    void detach() noexcept {
        LockGuard lock(mutex);
        hook = nullptr;
    }
};

struct WorkerOutcome {
    std::unique_ptr<ParseJob> job;
    ParseResult result;
};

// Where a worker job leaves its outcome for poll().
struct JobSlot {
    Mutex mutex;
    std::optional<WorkerOutcome> outcome FROST_GUARDED_BY(mutex);
};

// now + duration, or nullopt (no deadline) for Duration::max() or on overflow.
[[nodiscard]] std::optional<IClock::SteadyTime> deadlineAfter(const IClock& clock, IClock::Duration duration) noexcept {
    if (duration == IClock::Duration::max()) {
        return std::nullopt;
    }
    const IClock::SteadyTime now = clock.steadyNow();
    if (duration > IClock::SteadyTime::max() - now) {
        return std::nullopt;
    }
    return now + duration;
}

// The worker parse: runs the job to completion, a stop, or the time limit (status Deadline).
sched::Task<void> workerParse(std::unique_ptr<ParseJob> job, const IClock& clock, IClock::Duration limit,
                              std::stop_token stop, std::stop_token schedulerStop, std::shared_ptr<JobSlot> slot,
                              std::shared_ptr<Notifier> notifier) {
    ParseResult result = runParse(*job, clock, deadlineAfter(clock, limit), &stop, &schedulerStop);
    {
        LockGuard lock(slot->mutex);
        slot->outcome.emplace(WorkerOutcome{std::move(job), std::move(result)});
    }
    notifier->notify();
    co_return;
}

// Compiles a language's queries off the owner thread, then wakes the owner.
sched::Task<void> prepareQueries(const LanguageRegistry& registry, LanguageId language,
                                 std::shared_ptr<Notifier> notifier) {
    // A failure is a FrostIDE bug that SyntaxLanguage.AllQueriesCompile catches; highlights then
    // stay incomplete rather than failing the session.
    if (registry.prepare(language)) {
        notifier->notify();
    }
    co_return;
}

} // namespace

struct SessionState {
    const LanguageRegistry* registry = nullptr;
    sched::IScheduler* scheduler = nullptr;
    const IClock* clock = nullptr;
    SyntaxOptions options;

    LanguageId language = LanguageId::None;
    const Language* lang = nullptr;
    text::Snapshot text;
    SyntaxTree view;
    ParserPtr parser; // The idle parser; null while a job owns it.

    std::shared_ptr<JobSlot> job; // Non-null while a worker parse runs or waits for poll().
    std::stop_source jobStop;
    std::uint64_t jobVersion = 0;     // The version of the text the job parses.
    std::vector<TSInputEdit> pending; // Edits from the job's text to `text`.
    std::shared_ptr<Notifier> notifier;

    SyntaxState state = SyntaxState::Off;
    SyntaxReason reason = SyntaxReason::NoLanguage;
    SyntaxStats stats;
    bool queriesSeen = false;

    [[nodiscard]] std::uint64_t maxBytes() const noexcept {
        return std::min(options.maxDocumentBytes, kTreeSitterMaxBytes);
    }

    void renewNotifier() noexcept {
        if (notifier) {
            notifier->detach();
        }
        notifier = std::make_shared<Notifier>();
        LockGuard lock(notifier->mutex);
        notifier->hook = options.onParseReady;
        notifier->context = options.onParseReadyContext;
    }

    void cancelJob() noexcept {
        if (job) {
            jobStop.request_stop();
            job.reset();
        }
        pending.clear();
    }

    void goOff(SyntaxReason why) noexcept {
        cancelJob();
        view = SyntaxTree{};
        state = SyntaxState::Off;
        reason = why;
    }

    void failWith(SyntaxReason why) noexcept {
        view = SyntaxTree{};
        pending.clear();
        state = SyntaxState::Failed;
        reason = why;
        ++stats.failures;
    }

    [[nodiscard]] ParserPtr takeParser() noexcept {
        ParserPtr taken = parser ? std::move(parser) : ParserPtr(ts_parser_new());
        // Cheap (it resets the parser); the language may have changed since the last parse.
        const bool ok = ts_parser_set_language(taken.get(), lang->grammar);
        FROST_ASSERT(ok);
        return taken;
    }

    [[nodiscard]] const TSTree* viewTree() const noexcept { return view.empty() ? nullptr : view.state()->tree.get(); }

    void publishParsed(TreePtr tree, std::uint64_t parsedVersion) noexcept {
        view = makeTree(std::move(tree), lang, text, parsedVersion);
        state = parsedVersion == text.version() ? SyntaxState::Current : SyntaxState::Stale;
        reason = SyntaxReason::None;
    }

    // Parses `text`, reusing `old` (in text's coordinates) when given: the synchronous attempt
    // within the budget, then the worker. Budget: B5.
    void startParse(const TSTree* old) noexcept {
        FROST_ASSERT(!job);
        auto parseJob = std::make_unique<ParseJob>(takeParser(), text, old);
        const IClock::Duration budget = options.syncParseBudget;
        if (budget > IClock::Duration::zero()) {
            ++stats.syncAttempts;
            ParseResult result = runParse(*parseJob, *clock, deadlineAfter(*clock, budget), nullptr, nullptr);
            stats.lastSyncParse = result.elapsed;
            stats.maxSyncParse = std::max(stats.maxSyncParse, result.elapsed);
            switch (result.status) {
            case ParseStatus::Done:
                ++stats.syncCompleted;
                parser = std::move(parseJob->parser);
                publishParsed(std::move(result.tree), text.version());
                return;
            case ParseStatus::Deadline: ++stats.syncOverruns; break;
            case ParseStatus::Stopped:
            case ParseStatus::Failed:
                parser = std::move(parseJob->parser);
                failWith(SyntaxReason::ParserError);
                return;
            }
        } else if (old != nullptr) {
            // The job outlives this call, so it keeps its own O(1) copy of the old tree.
            parseJob->ownedOldTree.reset(ts_tree_copy(old));
            parseJob->oldTree = parseJob->ownedOldTree.get();
        }
        ++stats.workerParses;
        job = std::make_shared<JobSlot>();
        jobStop = std::stop_source{};
        jobVersion = text.version();
        pending.clear();
        scheduler
            ->spawn(options.parseLane, workerParse(std::move(parseJob), *clock, options.workerParseTimeLimit,
                                                   jobStop.get_token(), scheduler->stopToken(), job, notifier))
            .detach();
        state = view.empty() ? SyntaxState::Pending : SyntaxState::Stale;
        reason = SyntaxReason::None;
    }

    void open(LanguageId id, const text::Snapshot& snapshot) noexcept {
        cancelJob();
        view = SyntaxTree{};
        renewNotifier();
        language = id;
        lang = nullptr;
        text = snapshot;
        queriesSeen = false;
        if (id == LanguageId::None) {
            goOff(SyntaxReason::NoLanguage);
            return;
        }
        Result<const Language*> found = registry->get(id);
        if (!found) {
            goOff(SyntaxReason::NoLanguage);
            return;
        }
        lang = *found;
        if (text.size().value() > maxBytes()) {
            goOff(SyntaxReason::TooLarge);
            return;
        }
        if (!lang->queriesReady()) {
            scheduler->spawn(options.parseLane, prepareQueries(*registry, id, notifier)).detach();
        }
        ++stats.fullParses;
        state = SyntaxState::Pending;
        reason = SyntaxReason::None;
        startParse(nullptr);
    }

    void resync(const text::Snapshot& after) noexcept {
        cancelJob();
        view = SyntaxTree{};
        text = after;
        ++stats.resyncs;
        ++stats.fullParses;
        state = SyntaxState::Pending;
        startParse(nullptr);
    }

    // Off and Failed sessions only record the text (an Off(TooLarge) one reopens once it fits);
    // a session growing past the limit goes Off. Returns true when `after` was handled here.
    [[nodiscard]] bool handleInactive(const text::Snapshot& after) noexcept {
        if (state == SyntaxState::Off) {
            text = after;
            if (reason == SyntaxReason::TooLarge &&
                after.size().value() <= std::min(options.reenableBytes, maxBytes())) {
                open(language, after);
            }
            return true;
        }
        if (state == SyntaxState::Failed) {
            text = after;
            return true;
        }
        if (after.size().value() > maxBytes()) {
            text = after;
            goOff(SyntaxReason::TooLarge);
            return true;
        }
        return false;
    }

    void applyEdits(const std::vector<TSInputEdit>& edits, const text::Snapshot& after) noexcept {
        text = after;
        stats.editsApplied += edits.size();
        if (!view.empty()) {
            TreePtr copy(ts_tree_copy(viewTree()));
            for (const TSInputEdit& edit : edits) {
                ts_tree_edit(copy.get(), &edit);
            }
            view = makeTree(std::move(copy), lang, text, view.parsedVersion());
        }
        if (job) {
            pending.insert(pending.end(), edits.begin(), edits.end());
            state = view.empty() ? SyntaxState::Pending : SyntaxState::Stale;
            return;
        }
        startParse(viewTree());
    }

    bool poll() noexcept {
        bool changed = false;
        if (lang != nullptr && !queriesSeen && lang->queriesReady()) {
            queriesSeen = true;
            changed = true;
        }
        if (!job) {
            return changed;
        }
        std::optional<WorkerOutcome> outcome;
        {
            LockGuard lock(job->mutex);
            outcome.swap(job->outcome);
        }
        if (!outcome) {
            return changed;
        }
        job.reset();
        ParseJob& finished = *outcome->job;
        switch (outcome->result.status) {
        case ParseStatus::Done: {
            ++stats.workerCompleted;
            parser = std::move(finished.parser);
            TreePtr tree = std::move(outcome->result.tree);
            for (const TSInputEdit& edit : pending) {
                ts_tree_edit(tree.get(), &edit);
            }
            const bool catchUp = !pending.empty();
            pending.clear();
            publishParsed(std::move(tree), jobVersion);
            if (catchUp) {
                ++stats.catchUps;
                startParse(viewTree());
            }
            return true;
        }
        case ParseStatus::Deadline:
            // The worker's time limit; the halted parse is abandoned.
            ts_parser_reset(finished.parser.get());
            parser = std::move(finished.parser);
            failWith(SyntaxReason::TimeLimit);
            return true;
        case ParseStatus::Stopped:
        case ParseStatus::Failed:
            parser = std::move(finished.parser);
            failWith(SyntaxReason::ParserError);
            return true;
        }
        return true;
    }
};

} // namespace detail

SyntaxSession::SyntaxSession(const LanguageRegistry& registry, sched::IScheduler& scheduler, const IClock& clock,
                             SyntaxOptions options) noexcept
    : m_state(std::make_unique<detail::SessionState>()) {
    m_state->registry = &registry;
    m_state->scheduler = &scheduler;
    m_state->clock = &clock;
    m_state->options = options;
    m_state->renewNotifier();
}

SyntaxSession::~SyntaxSession() {
    if (m_state) {
        m_state->cancelJob();
        m_state->notifier->detach();
    }
}

SyntaxSession::SyntaxSession(SyntaxSession&& other) noexcept = default;

SyntaxSession& SyntaxSession::operator=(SyntaxSession&& other) noexcept {
    if (this != &other) {
        if (m_state) {
            m_state->cancelJob();
            m_state->notifier->detach();
        }
        m_state = std::move(other.m_state);
    }
    return *this;
}

void SyntaxSession::open(LanguageId language, const text::Snapshot& text) noexcept {
    FROST_PRE(m_state != nullptr);
    m_state->open(language, text);
}

void SyntaxSession::apply(const text::ChangeSet& changes, const text::Snapshot& after) noexcept {
    FROST_PRE(m_state != nullptr);
    detail::SessionState& s = *m_state;
    if (s.handleInactive(after)) {
        return;
    }
    if (changes.baseVersion != s.text.version() || changes.version != after.version() ||
        !after.sharesStorageWith(s.text)) {
        s.resync(after);
        return;
    }
    const std::vector<TSInputEdit> edits = detail::toInputEdits(changes.changes);
    if (edits.empty()) {
        if (after.version() != s.text.version()) {
            s.resync(after);
        } else {
            s.text = after;
        }
        return;
    }
    s.applyEdits(edits, after);
}

void SyntaxSession::apply(std::span<const text::TextChange> changes, const text::Snapshot& after) noexcept {
    FROST_PRE(m_state != nullptr);
    detail::SessionState& s = *m_state;
    if (s.handleInactive(after)) {
        return;
    }
    std::int64_t delta = 0;
    for (const text::TextChange& change : changes) {
        delta += static_cast<std::int64_t>(change.newEnd.value()) - static_cast<std::int64_t>(change.oldEnd.value());
    }
    const auto before = static_cast<std::int64_t>(s.text.size().value());
    if (!after.sharesStorageWith(s.text) || after.version() <= s.text.version() ||
        before + delta != static_cast<std::int64_t>(after.size().value())) {
        s.resync(after);
        return;
    }
    s.applyEdits(detail::toInputEdits(changes), after);
}

bool SyntaxSession::poll() noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->poll();
}

SyntaxTree SyntaxSession::tree() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->view;
}

SyntaxState SyntaxSession::state() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->state;
}

SyntaxReason SyntaxSession::reason() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->reason;
}

bool SyntaxSession::parseInFlight() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->job != nullptr;
}

const SyntaxStats& SyntaxSession::stats() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->stats;
}

const text::Snapshot& SyntaxSession::text() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->text;
}

LanguageId SyntaxSession::language() const noexcept {
    FROST_PRE(m_state != nullptr);
    return m_state->language;
}

} // namespace frost::syntax
