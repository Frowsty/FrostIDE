// SPDX-License-Identifier: Apache-2.0
//
// RunCollector: records what a RunManager reports (the listener's and one run's callbacks) for the
// frost::run tests, and lets the test thread wait for a condition on it without fixed sleeps.
#pragma once

#include "frost/core/mutex.hpp"
#include "frost/run/run_manager.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::test {

class RunCollector {
public:
    // What the manager reported, in order: "started", "phase:<Phase>", "out", "err", "finished".
    struct Snapshot {
        std::vector<std::string> events;
        std::vector<run::RunPhase> phases;
        std::string out;
        std::string err;
        std::optional<run::RunResult> result;
        std::thread::id callbackThread{};
        bool callbacksOnOneThread = true;
    };

    RunCollector() = default;
    RunCollector(const RunCollector&) = delete;
    RunCollector& operator=(const RunCollector&) = delete;

    // The handlers own the recording state even if the test stops observing before a callback returns.
    [[nodiscard]] run::RunHandlers handlers() {
        run::RunHandlers handlers;
        handlers.onPhaseChanged = [state = m_state](const run::RunStatus& status) {
            state->record([&](Snapshot& s) {
                s.events.push_back("phase:" + std::string(run::toString(status.phase)));
                s.phases.push_back(status.phase);
            });
        };
        handlers.onOutput = [state = m_state](run::OutputStream stream, std::string_view text) {
            state->record([&](Snapshot& s) {
                if (stream == run::OutputStream::Stdout) {
                    if (s.events.empty() || s.events.back() != "out") {
                        s.events.emplace_back("out");
                    }
                    s.out.append(text);
                } else {
                    if (s.events.empty() || s.events.back() != "err") {
                        s.events.emplace_back("err");
                    }
                    s.err.append(text);
                }
            });
        };
        handlers.onFinished = [state = m_state](const run::RunResult& result) {
            state->record([&](Snapshot& s) {
                s.events.emplace_back("finished");
                s.result = result;
            });
        };
        return handlers;
    }

    // Waits up to `timeout` for `done` to hold; returns the snapshot either way.
    Snapshot waitUntil(const std::function<bool(const Snapshot&)>& done,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        UniqueLock lock(m_state->mutex);
        m_state->changed.wait_for(lock.native(), timeout,
                                  [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return done(m_state->snapshot); });
        return m_state->snapshot;
    }

    // Waits for onFinished.
    Snapshot waitFinished(std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        return waitUntil([](const Snapshot& s) { return s.result.has_value(); }, timeout);
    }

    Snapshot snapshot() {
        LockGuard lock(m_state->mutex);
        return m_state->snapshot;
    }

private:
    struct State {
        template<class F>
        void record(F change) FROST_NO_THREAD_SAFETY_ANALYSIS {
            {
                LockGuard lock(mutex);
                const auto self = std::this_thread::get_id();
                if (snapshot.callbackThread == std::thread::id{}) {
                    snapshot.callbackThread = self;
                } else if (snapshot.callbackThread != self) {
                    snapshot.callbacksOnOneThread = false;
                }
                change(snapshot);
            }
            changed.notify_all();
        }

        Mutex mutex;
        std::condition_variable changed;
        Snapshot snapshot FROST_GUARDED_BY(mutex);
    };

    const std::shared_ptr<State> m_state = std::make_shared<State>();
};

} // namespace frost::test
