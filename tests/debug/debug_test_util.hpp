// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the frost::debug tests: polling with a deadline, and a recorder of a DebugSession's
// callbacks (they arrive on the DAP client's reader thread). Header-only; test code only.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::debug::test {

using namespace std::chrono_literals;

inline constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

// Polls `done` every few milliseconds for up to `timeout`.
inline bool waitUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// An absolute path for `name` under a fixed root ("C:\work\..." or "/work/...").
inline std::string workPath(std::string_view name) {
    std::string out = kWindows ? "C:\\work\\" : "/work/";
    for (const char c : name) {
        out.push_back(kWindows && c == '/' ? '\\' : c);
    }
    return out;
}

// What a session reported, in order.
struct SessionRecorder {
    mutable Mutex mutex;
    std::vector<std::string> events FROST_GUARDED_BY(mutex);
    std::vector<StopInfo> stops FROST_GUARDED_BY(mutex);
    std::vector<DebugThread> threads FROST_GUARDED_BY(mutex);
    std::vector<FrameList> frames FROST_GUARDED_BY(mutex);
    std::vector<DebugFrame> selected FROST_GUARDED_BY(mutex);
    std::string output FROST_GUARDED_BY(mutex);
    std::vector<BreakpointResult> breakpoints FROST_GUARDED_BY(mutex);
    std::optional<DebugOutcome> ended FROST_GUARDED_BY(mutex);

    DebugSessionCallbacks callbacks() {
        return {
            .onStateChanged =
                [this](DebugState state) {
                    LockGuard lock(mutex);
                    events.emplace_back(toString(state));
                },
            .onStopped =
                [this](const StopInfo& stop) {
                    LockGuard lock(mutex);
                    events.push_back("stopped " + stop.reason);
                    stops.push_back(stop);
                },
            .onThreads =
                [this](const std::vector<DebugThread>& list) {
                    LockGuard lock(mutex);
                    threads = list;
                },
            .onFrames =
                [this](std::int64_t, const FrameList& list) {
                    LockGuard lock(mutex);
                    frames.push_back(list);
                },
            .onFrameSelected =
                [this](std::int64_t, const DebugFrame& frame) {
                    LockGuard lock(mutex);
                    selected.push_back(frame);
                },
            .onOutput =
                [this](std::string_view, std::string_view text) {
                    LockGuard lock(mutex);
                    output += text;
                },
            .onBreakpoints =
                [this](const std::vector<BreakpointResult>& results) {
                    LockGuard lock(mutex);
                    breakpoints.insert(breakpoints.end(), results.begin(), results.end());
                },
            .onExited =
                [this](int code) {
                    LockGuard lock(mutex);
                    events.push_back("exited " + std::to_string(code));
                },
            .onEnded =
                [this](const DebugOutcome& outcome) {
                    LockGuard lock(mutex);
                    events.emplace_back("ended");
                    ended = outcome;
                },
        };
    }

    [[nodiscard]] std::size_t stopCount() const {
        LockGuard lock(mutex);
        return stops.size();
    }
    [[nodiscard]] std::size_t selectedCount() const {
        LockGuard lock(mutex);
        return selected.size();
    }
    [[nodiscard]] bool hasEnded() const {
        LockGuard lock(mutex);
        return ended.has_value();
    }
    [[nodiscard]] std::vector<std::string> eventList() const {
        LockGuard lock(mutex);
        return events;
    }
};

// The adapter factory over a fake adapter's transport.
inline AdapterFactory fakeFactory(FakeDebugAdapter& adapter) {
    return [&adapter](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        return makeDapClient(adapter.transport(), std::move(handlers));
    };
}

} // namespace frost::debug::test
