// SPDX-License-Identifier: Apache-2.0
//
// WatchQueue (watch_queue.hpp): filtering, debouncing and the next() loop shared by every watcher
// (ADR 0020).

#include "watch_queue.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::io::detail {
namespace {

[[nodiscard]] bool isAscii(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

[[nodiscard]] char asciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

// Forwards a stop request from one token to the per-call stop source of a next().
struct Forward {
    std::stop_source* target;
    void operator()() const noexcept { static_cast<void>(target->request_stop()); }
};

} // namespace

bool startsWithFoldCase(std::string_view text, std::string_view prefix) {
    if (isAscii(prefix) && isAscii(text.substr(0, std::min(text.size(), prefix.size())))) {
        return text.size() >= prefix.size() &&
               std::ranges::equal(text.substr(0, prefix.size()), prefix,
                                  [](char a, char b) { return asciiLower(a) == asciiLower(b); });
    }
#ifdef _WIN32
    // Ordinal upper-casing maps UTF-16 units one to one, so the prefix's length in units is exact.
    auto wideText = toWide(text);
    auto widePrefix = toWide(prefix);
    if (!wideText || !widePrefix || wideText->size() < widePrefix->size()) {
        return false;
    }
    return CompareStringOrdinal(wideText->data(), static_cast<int>(widePrefix->size()), widePrefix->data(),
                                static_cast<int>(widePrefix->size()), TRUE) == CSTR_EQUAL;
#else
    return text.starts_with(prefix);
#endif
}

Result<WatchOptions> validateWatchOptions(const std::string& root, WatchOptions options, PathStyle style) {
    if (options.debounce < IClock::Duration::zero()) {
        return fail(ErrorCode::InvalidArgument, "watch: debounce must not be negative");
    }
    if (options.maxDelay < options.debounce) {
        return fail(ErrorCode::InvalidArgument, "watch: maxDelay must be at least debounce");
    }
    if (options.maxPendingPaths == 0) {
        return fail(ErrorCode::InvalidArgument, "watch: maxPendingPaths must be at least 1");
    }
    if (options.pollInterval < std::chrono::milliseconds(100)) {
        return fail(ErrorCode::InvalidArgument, "watch: pollInterval must be at least 100 ms");
    }
    const char separator = style == PathStyle::Windows ? '\\' : '/';
    for (std::string& excluded : options.excluded) {
        if (!isAbsolutePath(excluded, style)) {
            return fail(ErrorCode::InvalidArgument, std::format("watch: excluded path '{}' is not absolute", excluded));
        }
        auto normalized = normalizePath(excluded, style);
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        const bool below = style == PathStyle::Windows
                               ? startsWithFoldCase(*normalized, root) && normalized->size() > root.size() &&
                                     (root.back() == separator || (*normalized)[root.size()] == separator)
                               : isStrictlyBelow(*normalized, root, separator);
        if (!below) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("watch: excluded path '{}' is not below the root '{}'", excluded, root));
        }
        excluded = std::move(*normalized);
    }
    return options;
}

std::shared_ptr<WatchQueue> WatchQueue::create(sched::IScheduler& scheduler, WatchQueueConfig config) {
    return std::make_shared<WatchQueue>(scheduler, std::move(config));
}

WatchQueue::WatchQueue(sched::IScheduler& scheduler, WatchQueueConfig config)
    : m_scheduler(scheduler), m_config(std::move(config)),
      m_coalescer(m_config.root, m_config.separator, m_config.options.maxPendingPaths) {
    m_status.mode = m_config.mode;
}

bool WatchQueue::excludedBelow(std::string_view path) const {
    for (const std::string& excluded : m_config.options.excluded) {
        const bool prefix =
            m_config.foldCaseForExclusions ? startsWithFoldCase(path, excluded) : path.starts_with(excluded);
        if (prefix && path.size() > excluded.size() && path[excluded.size()] == m_config.separator) {
            return true;
        }
    }
    // The caller's own rule (WatchOptions::excludedBelow) sees only paths strictly below the root.
    return m_config.options.excludedBelow && path.size() > m_config.root.size() &&
           isAtOrBelow(path, m_config.root, m_config.separator) && m_config.options.excludedBelow(path);
}

bool WatchQueue::accepts(std::string_view path) const {
    if (!isAtOrBelow(path, m_config.root, m_config.separator) || excludedBelow(path)) {
        return false;
    }
    if (path == m_config.root) {
        return true;
    }
    if (!m_config.options.recursive) {
        const std::string_view rest =
            path.substr(m_config.root.size() + (m_config.root.back() == m_config.separator ? 0 : 1));
        if (rest.find(m_config.separator) != std::string_view::npos) {
            return false;
        }
    }
    if (!m_config.options.reportFrostTempFiles) {
        const std::size_t slash = path.rfind(m_config.separator);
        const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
        if (name.find(kAtomicTempMarker) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

bool WatchQueue::acceptLocked(RawEvent& event) const {
    if (event.kind != RawKind::Overflow) {
        return accepts(event.path);
    }
    if (!isAtOrBelow(event.path, m_config.root, m_config.separator) || excludedBelow(event.path)) {
        return false;
    }
    if (!m_config.options.recursive && event.path != m_config.root) {
        // A non-recursive watch has no subtree below a child: rescan from the root.
        event.path = m_config.root;
    }
    return true;
}

void WatchQueue::pushLocked(const RawEvent& event) {
    const IClock::SteadyTime now = m_scheduler.clock().steadyNow();
    if (m_coalescer.empty()) {
        m_firstEvent = now;
    }
    m_lastEvent = now;
    m_coalescer.push(event);
    ++m_status.rawEvents;
}

std::size_t WatchQueue::push(const RawEvent& event) { return push(std::span<const RawEvent>(&event, 1)); }

std::size_t WatchQueue::push(std::span<const RawEvent> events) {
    LockGuard lock(m_mutex);
    if (m_closed) {
        return 0;
    }
    std::size_t accepted = 0;
    for (const RawEvent& event : events) {
        RawEvent copy = event;
        if (acceptLocked(copy)) {
            pushLocked(copy);
            ++accepted;
        }
    }
    if (accepted != 0) {
        wakeLocked();
    }
    return accepted;
}

void WatchQueue::pushOverflow(std::string path) {
    RawEvent event{RawKind::Overflow, std::move(path), std::nullopt, false, 0};
    static_cast<void>(push(event));
}

void WatchQueue::close(Error error, bool deliverPending) {
    {
        LockGuard lock(m_mutex);
        if (m_closed) {
            return;
        }
        m_closed = true;
        m_deliverPending = deliverPending;
        m_closeError = std::move(error);
        wakeLocked();
    }
    // Outside the lock: the stop callbacks of a sleeping next() take it.
    static_cast<void>(m_closeSource.request_stop());
}

void WatchQueue::injectFailure(Error error) {
    LockGuard lock(m_mutex);
    m_injected = std::move(error);
    wakeLocked();
}

void WatchQueue::setMode(WatchMode mode, DegradeReason reason, std::uint64_t value, std::string detail) {
    LockGuard lock(m_mutex);
    m_status.mode = mode;
    m_status.reason = reason;
    m_status.reasonValue = value;
    m_status.detail = std::move(detail);
}

WatchStatus WatchQueue::status() const {
    LockGuard lock(m_mutex);
    WatchStatus status = m_status;
    status.pendingPaths = m_coalescer.pendingPaths();
    return status;
}

bool WatchQueue::closed() const {
    LockGuard lock(m_mutex);
    return m_closed;
}

bool WatchQueue::readyLocked() const { return m_closed || m_injected.has_value() || !m_coalescer.empty(); }

// Resumes the suspended next(), if any. Under the lock on purpose (see the header).
void WatchQueue::wakeLocked() {
    if (m_waiter) {
        m_scheduler.enqueue(std::exchange(m_waiter, {}), m_waiterLane);
    }
}

void WatchQueue::wakeWaiter() {
    LockGuard lock(m_mutex);
    wakeLocked();
}

// Suspends next() until readyLocked() or a stop of the per-call token; the stop callback that wakes it
// lives in next()'s frame, constructed before the first suspension, so construction never races with it.
class WatchQueue::WaitForEvents {
public:
    WaitForEvents(WatchQueue& queue, const std::stop_token& token, sched::Lane lane) noexcept
        : m_queue(queue), m_token(token), m_lane(lane) {}
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        LockGuard lock(m_queue.m_mutex);
        // A stop requested before this check is seen here; one requested after it runs the callback,
        // which takes the lock and finds the waiter.
        if (m_queue.readyLocked() || m_token.stop_requested()) {
            return false;
        }
        m_queue.m_waiter = handle;
        m_queue.m_waiterLane = m_lane;
        return true;
    }
    void await_resume() const noexcept {}

private:
    WatchQueue& m_queue;
    const std::stop_token& m_token;
    sched::Lane m_lane;
};

struct WatchQueue::Wake {
    WatchQueue* queue;
    void operator()() const noexcept { queue->wakeWaiter(); }
};

sched::Task<Result<FileChangeBatch>> WatchQueue::next(std::shared_ptr<WatchQueue> queue, std::stop_token token) {
    WatchQueue& self = *queue;
    {
        LockGuard lock(self.m_mutex);
        FROST_PRE(!self.m_consumerActive); // A single consumer (file_watcher.hpp).
        self.m_consumerActive = true;
    }
    struct Release {
        WatchQueue& queue;
        ~Release() {
            LockGuard lock(queue.m_mutex);
            queue.m_consumerActive = false;
        }
    } release{self};

    const sched::Lane lane = self.m_scheduler.currentLane().value_or(sched::Lane::UserVisible);
    std::stop_source callStop;
    const std::stop_token callToken = callStop.get_token();
    // Registered before any suspension; each may run inline here if its token is already stopped.
    std::stop_callback<Forward> fromCaller(token, Forward{&callStop});
    std::stop_callback<Forward> fromClose(self.m_closeSource.get_token(), Forward{&callStop});
    std::stop_callback<Forward> fromScheduler(self.m_scheduler.stopToken(), Forward{&callStop});
    std::stop_callback<Wake> wake(callToken, Wake{&self});

    const auto cancelled = [&]() -> Result<FileChangeBatch> {
        if (token.stop_requested()) {
            return fail(ErrorCode::Cancelled, "next() was cancelled");
        }
        return fail(ErrorCode::Cancelled, "the scheduler is shutting down");
    };

    for (;;) {
        std::optional<IClock::SteadyTime> due; // Set while changes are pending but not yet due.
        {
            LockGuard lock(self.m_mutex);
            if (self.m_injected) {
                Error error = std::move(*self.m_injected);
                self.m_injected.reset();
                co_return std::unexpected(std::move(error));
            }
            const bool closed = self.m_closed;
            if (closed && !(self.m_deliverPending && !self.m_coalescer.empty())) {
                co_return std::unexpected(*self.m_closeError);
            }
            if (!closed && token.stop_requested()) {
                co_return cancelled();
            }
            if (!self.m_coalescer.empty()) {
                const WatchOptions& options = self.m_config.options;
                due = std::min(self.m_lastEvent + options.debounce, self.m_firstEvent + options.maxDelay);
                if (closed || self.m_scheduler.clock().steadyNow() >= *due) {
                    std::vector<FileChange> changes = self.m_coalescer.take();
                    if (changes.empty()) {
                        continue; // The pending set netted to nothing.
                    }
                    FileChangeBatch batch;
                    batch.changes = std::move(changes);
                    batch.sequence = ++self.m_sequence;
                    batch.mode = self.m_status.mode;
                    ++self.m_status.batches;
                    self.m_status.rescans += static_cast<std::uint64_t>(
                        std::ranges::count(batch.changes, FileChangeKind::Rescan, &FileChange::kind));
                    co_return batch;
                }
            }
        }
        if (callToken.stop_requested()) {
            // Closed (handled above on the next round) or stopped by the caller or the scheduler.
            LockGuard lock(self.m_mutex);
            if (!self.m_closed && !self.m_injected) {
                co_return cancelled();
            }
            continue;
        }
        if (due) {
            const bool expired = co_await self.m_scheduler.sleepUntil(*due, callToken);
            if (!expired && !callToken.stop_requested()) {
                co_return cancelled(); // The scheduler refused the timer: it is shutting down.
            }
            continue;
        }
        co_await WaitForEvents(self, callToken, lane);
    }
}

} // namespace frost::io::detail
