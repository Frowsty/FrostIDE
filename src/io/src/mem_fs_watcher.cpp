// SPDX-License-Identifier: Apache-2.0
//
// MemFsWatcher (frost/io/mem_fs_watcher.hpp): MemFs observer events into a WatchQueue (ADR 0020).

#include "frost/io/mem_fs_watcher.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"

#include "watch_queue.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::io {

// One watch's observer: converts MemFs events and detects the removal of the root.
struct MemFsWatcher::WatchState final : IMemFsObserver {
    WatchState(std::shared_ptr<detail::WatchQueue> watchQueue, char rootSeparator)
        : queue(std::move(watchQueue)), separator(rootSeparator) {}

    void onMemFsEvents(std::span<const MemFsEvent> events) noexcept override {
        const std::string& root = queue->root();
        std::vector<detail::RawEvent> raw;
        raw.reserve(events.size());
        for (const MemFsEvent& event : events) {
            const bool rootGone = ((event.kind == MemFsEventKind::Removed || event.kind == MemFsEventKind::MovedFrom) &&
                                   detail::isAtOrBelow(root, event.path, separator)) ||
                                  (event.kind == MemFsEventKind::MovedTo && event.path == root);
            if (rootGone) {
                raw.push_back({detail::RawKind::Removed, root, FileType::Directory, false, 0});
                static_cast<void>(queue->push(raw));
                queue->close(Error(ErrorCode::NotFound, std::format("watch '{}': the root was removed or moved", root)),
                             /*deliverPending=*/true);
                return;
            }
            detail::RawKind kind = detail::RawKind::Modified;
            switch (event.kind) {
            case MemFsEventKind::Created: kind = detail::RawKind::Created; break;
            case MemFsEventKind::Modified: kind = detail::RawKind::Modified; break;
            case MemFsEventKind::Removed: kind = detail::RawKind::Removed; break;
            case MemFsEventKind::MovedFrom: kind = detail::RawKind::MovedFrom; break;
            case MemFsEventKind::MovedTo: kind = detail::RawKind::MovedTo; break;
            }
            raw.push_back({kind, event.path, event.type, false, event.cookie});
        }
        static_cast<void>(queue->push(raw));
    }

    std::shared_ptr<detail::WatchQueue> queue;
    char separator;
};

class MemFsDirectoryWatch final : public IDirectoryWatch {
public:
    MemFsDirectoryWatch(MemFsWatcher& owner, std::shared_ptr<MemFsWatcher::WatchState> state)
        : m_owner(owner), m_state(std::move(state)) {}
    // Never blocks beyond the MemFs lock (Budget: B21).
    ~MemFsDirectoryWatch() override {
        m_owner.m_fs.removeObserver(*m_state);
        m_state->queue->close(Error(ErrorCode::Cancelled, "the watch was destroyed"), /*deliverPending=*/false);
        m_owner.forget(m_state.get());
    }
    MemFsDirectoryWatch(const MemFsDirectoryWatch&) = delete ("a watch has one owner");
    MemFsDirectoryWatch& operator=(const MemFsDirectoryWatch&) = delete ("a watch has one owner");

    sched::Task<Result<FileChangeBatch>> next(std::stop_token token) override {
        return detail::WatchQueue::next(m_state->queue, std::move(token));
    }
    const std::string& root() const noexcept override { return m_state->queue->root(); }
    WatchStatus status() const override { return m_state->queue->status(); }

private:
    MemFsWatcher& m_owner;
    std::shared_ptr<MemFsWatcher::WatchState> m_state;
};

MemFsWatcher::MemFsWatcher(MemFs& fs, sched::IScheduler& scheduler) : m_fs(fs), m_scheduler(scheduler) {}

MemFsWatcher::~MemFsWatcher() {
    LockGuard lock(m_mutex);
    FROST_PRE(m_live.empty()); // Watches refer to their watcher.
}

void MemFsWatcher::forget(WatchState* state) {
    LockGuard lock(m_mutex);
    std::erase_if(m_live, [&](const std::shared_ptr<WatchState>& live) { return live.get() == state; });
}

sched::Task<Result<std::unique_ptr<IDirectoryWatch>>> MemFsWatcher::watch(std::string root, WatchOptions options,
                                                                          std::stop_token token) {
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "watch() was cancelled");
    }
    const PathStyle style = m_fs.style();
    if (!isAbsolutePath(root, style)) {
        co_return fail(ErrorCode::InvalidArgument, std::format("watch '{}': the root must be absolute", root));
    }
    auto normalized = normalizePath(root, style);
    if (!normalized) {
        co_return std::unexpected(std::move(normalized.error()));
    }
    // MemFs keys spell drive letters in upper case.
    if (style == PathStyle::Windows && normalized->size() >= 2 && (*normalized)[1] == ':' && (*normalized)[0] >= 'a' &&
        (*normalized)[0] <= 'z') {
        (*normalized)[0] = static_cast<char>((*normalized)[0] - ('a' - 'A'));
    }
    auto validated = detail::validateWatchOptions(*normalized, std::move(options), style);
    if (!validated) {
        co_return std::unexpected(std::move(validated.error()));
    }
    const char separator = style == PathStyle::Windows ? '\\' : '/';
    const bool forced = validated->forcePolling;
    detail::WatchQueueConfig config{*normalized, separator, std::move(*validated), /*foldCaseForExclusions=*/false,
                                    forced ? WatchMode::Polling : WatchMode::Native};
    auto queue = detail::WatchQueue::create(m_scheduler, std::move(config));
    if (forced) {
        queue->setMode(WatchMode::Polling, DegradeReason::Forced, 0, "WatchOptions::forcePolling");
    }
    auto state = std::make_shared<WatchState>(std::move(queue), separator);
    // Observe first, then check the root: a removal in between is then seen by the observer.
    m_fs.addObserver(*state);
    auto status = m_fs.status(*normalized);
    if (!status || status->type != FileType::Directory) {
        m_fs.removeObserver(*state);
        if (!status) {
            co_return std::unexpected(std::move(status.error()));
        }
        co_return fail(ErrorCode::InvalidArgument, std::format("watch '{}': not a directory", root));
    }
    {
        LockGuard lock(m_mutex);
        m_live.push_back(state);
    }
    co_return std::make_unique<MemFsDirectoryWatch>(*this, std::move(state));
}

void MemFsWatcher::injectOverflow(std::string_view path) {
    LockGuard lock(m_mutex);
    for (const std::shared_ptr<WatchState>& state : m_live) {
        const std::string& root = state->queue->root();
        if (detail::isAtOrBelow(path, root, state->separator)) {
            state->queue->pushOverflow(std::string(path));
        } else if (detail::isStrictlyBelow(root, path, state->separator)) {
            state->queue->pushOverflow(root);
        }
    }
}

void MemFsWatcher::injectFailure(ErrorCode code) {
    LockGuard lock(m_mutex);
    for (const std::shared_ptr<WatchState>& state : m_live) {
        state->queue->injectFailure(Error(code, "MemFsWatcher::injectFailure"));
    }
}

void MemFsWatcher::setDegraded(DegradeReason reason, std::uint64_t reasonValue) {
    LockGuard lock(m_mutex);
    for (const std::shared_ptr<WatchState>& state : m_live) {
        state->queue->setMode(WatchMode::Polling, reason, reasonValue, "MemFsWatcher::setDegraded");
        state->queue->pushOverflow(state->queue->root());
    }
}

} // namespace frost::io
