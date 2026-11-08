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

    // Handlers for start(); they record into this collector.
    [[nodiscard]] run::RunHandlers handlers() {
        run::RunHandlers handlers;
        handlers.onPhaseChanged = [this](const run::RunStatus& status) {
            record([&](Snapshot& s) {
                s.events.push_back("phase:" + std::string(run::toString(status.phase)));
                s.phases.push_back(status.phase);
            });
        };
        handlers.onOutput = [this](run::OutputStream stream, std::string_view text) {
            record([&](Snapshot& s) {
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
        handlers.onFinished = [this](const run::RunResult& result) {
            record([&](Snapshot& s) {
                s.events.emplace_back("finished");
                s.result = result;
            });
        };
        return handlers;
    }

    // Waits up to `timeout` for `done` to hold; returns the snapshot either way.
    Snapshot waitUntil(const std::function<bool(const Snapshot&)>& done,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        UniqueLock lock(m_mutex);
        m_changed.wait_for(lock.native(), timeout, [&]() FROST_NO_THREAD_SAFETY_ANALYSIS { return done(m_snapshot); });
        return m_snapshot;
    }

    // Waits for onFinished.
    Snapshot waitFinished(std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        return waitUntil([](const Snapshot& s) { return s.result.has_value(); }, timeout);
    }

    Snapshot snapshot() {
        LockGuard lock(m_mutex);
        return m_snapshot;
    }

private:
    template<class F>
    void record(F change) FROST_NO_THREAD_SAFETY_ANALYSIS {
        {
            LockGuard lock(m_mutex);
            const auto self = std::this_thread::get_id();
            if (m_snapshot.callbackThread == std::thread::id{}) {
                m_snapshot.callbackThread = self;
            } else if (m_snapshot.callbackThread != self) {
                m_snapshot.callbacksOnOneThread = false;
            }
            change(m_snapshot);
        }
        m_changed.notify_all();
    }

    Mutex m_mutex;
    std::condition_variable m_changed;
    Snapshot m_snapshot FROST_GUARDED_BY(m_mutex);
};

} // namespace frost::test
