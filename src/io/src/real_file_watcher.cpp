// SPDX-License-Identifier: Apache-2.0
//
// RealFileWatcher (frost/io/file_watcher.hpp; ADR 0020): validation, the hop to Lane::Background, and the
// choice between the native backend and polling. The backends live in file_watcher_win.cpp,
// file_watcher_linux.cpp and polling_watcher.cpp.

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"

#include "real_file_watcher_impl.hpp" // asio first.

#include <format>
#include <utility>

namespace frost::io {

RealFileWatcher::RealFileWatcher(std::shared_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

Result<std::unique_ptr<RealFileWatcher>> RealFileWatcher::create(Reactor& reactor, sched::IScheduler& scheduler) {
    if (scheduler.callerDriven()) {
        return fail(ErrorCode::InvalidArgument,
                    "RealFileWatcher: a caller-driven scheduler cannot take wakeups from reactor threads "
                    "(use MemFsWatcher)");
    }
    if (reactor.stats().shutDown) {
        return fail(ErrorCode::Cancelled, "RealFileWatcher: the reactor is shut down");
    }
    return std::unique_ptr<RealFileWatcher>(new RealFileWatcher(std::make_shared<Impl>(reactor.impl(), scheduler)));
}

RealFileWatcher::~RealFileWatcher() {
    // Watches and watch() tasks refer to the watcher's scheduler and reactor (exit order, reactor.hpp).
    FROST_PRE(m_impl->liveWatches.load(std::memory_order_relaxed) == 0);
    FROST_PRE(m_impl->pendingWatchCalls.load(std::memory_order_relaxed) == 0);
}

sched::Task<Result<std::unique_ptr<IDirectoryWatch>>> RealFileWatcher::watch(std::string root, WatchOptions options,
                                                                             std::stop_token token) {
    std::shared_ptr<Impl> impl = m_impl; // The task never touches `this` after its first suspension.
    impl->pendingWatchCalls.fetch_add(1, std::memory_order_relaxed);
    struct Pending {
        Impl& impl;
        ~Pending() { impl.pendingWatchCalls.fetch_sub(1, std::memory_order_relaxed); }
    } pending{*impl};

    // Budget: B21. Everything below may block (CreateFileW, statfs, the inotify walk).
    co_await impl->scheduler.schedule(sched::Lane::Background);
    if (token.stop_requested()) {
        co_return fail(ErrorCode::Cancelled, "watch() was cancelled");
    }
    if (!isAbsolutePath(root)) {
        co_return fail(ErrorCode::InvalidArgument, std::format("watch '{}': the root must be absolute", root));
    }
    auto normalized = normalizePath(root);
    if (!normalized) {
        co_return std::unexpected(std::move(normalized.error()));
    }
    auto validated = detail::validateWatchOptions(*normalized, std::move(options), kNativePathStyle);
    if (!validated) {
        co_return std::unexpected(std::move(validated.error()));
    }
    auto status = realFileSystem().status(*normalized);
    if (!status) {
        co_return std::unexpected(std::move(status.error()));
    }
    if (status->type != FileType::Directory) {
        co_return fail(ErrorCode::InvalidArgument, std::format("watch '{}': not a directory", root));
    }
    const bool forced = validated->forcePolling;
    detail::WatchQueueConfig config{*normalized, kNativePathStyle == PathStyle::Windows ? '\\' : '/',
                                    std::move(*validated),
                                    /*foldCaseForExclusions=*/kNativePathStyle == PathStyle::Windows,
                                    forced ? WatchMode::Polling : WatchMode::Native};
    std::shared_ptr<detail::WatchQueue> queue = detail::WatchQueue::create(impl->scheduler, std::move(config));

    detail::DegradeTo degrade{DegradeReason::Forced, 0, "WatchOptions::forcePolling"};
    if (!forced) {
        auto native = co_await detail::startNativeWatch(impl, queue, token);
        if (!native) {
            co_return std::unexpected(std::move(native.error()));
        }
        if (native->producer) {
            impl->liveWatches.fetch_add(1, std::memory_order_relaxed);
            co_return std::make_unique<detail::RealDirectoryWatch>(impl, std::move(queue), std::move(*native));
        }
        degrade = std::move(native->degrade);
    }
    queue->setMode(WatchMode::Polling, degrade.reason, degrade.value, degrade.detail);
    auto polling = co_await detail::startPollingWatch(impl, queue, token);
    if (!polling) {
        co_return std::unexpected(std::move(polling.error()));
    }
    impl->liveWatches.fetch_add(1, std::memory_order_relaxed);
    co_return std::make_unique<detail::RealDirectoryWatch>(impl, std::move(queue), std::move(*polling));
}

namespace detail {

RealDirectoryWatch::RealDirectoryWatch(std::shared_ptr<RealFileWatcher::Impl> owner, std::shared_ptr<WatchQueue> queue,
                                       NativeOutcome producer) noexcept
    : m_owner(std::move(owner)), m_queue(std::move(queue)), m_producer(std::move(producer)) {}

RealDirectoryWatch::~RealDirectoryWatch() {
    // Never blocks (Budget: B21): the producer releases its OS resources on its own strand or task.
    m_producer.producer->requestStop();
    m_queue->close(Error(ErrorCode::Cancelled, "the watch was destroyed"), /*deliverPending=*/false);
    m_owner->liveWatches.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace detail
} // namespace frost::io
