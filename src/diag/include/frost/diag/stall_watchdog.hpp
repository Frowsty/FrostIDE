// SPDX-License-Identifier: Apache-2.0
//
// The UI-thread stall watchdog (PLAN.md section 2, Self-diagnostics; ADR 0022, D-I): counts
// stalls for B21 and writes freeze reports with stack samples of the stalled thread.
//
/*
 * Definitions.
 *   busy      from the event loop's awake() to its aboutToBlock() (QAbstractEventDispatcher),
 *             or from the first event after aboutToBlock() when no awake() came first (the glib
 *             dispatcher on Linux dispatches inside the iteration it announced with
 *             aboutToBlock(); Windows delivers sent messages inside the wait).
 *   progress  awake(), the start of every event (a global event filter), and the end of an
 *             idle section.
 *   idle      not busy, and also: inside a nested event loop (its own aboutToBlock), inside a
 *             modal size/move or menu loop (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE,
 *             WM_ENTERMENULOOP..WM_EXITMENULOOP), and inside a ScopedUiIdle.
 *   stall     a busy stretch with no progress for more than the threshold (50 ms; exactly 50 ms
 *             is not a stall). Counted on the UI thread when the stretch ends (Budget: B21), so
 *             the count is exact. Nothing counts before the first aboutToBlock() (start-up is
 *             B1's business), which arms the detector.
 *
 * The UI thread publishes one atomic<int64_t> (progressNs): > 0 the time of the last progress
 * while busy, < 0 minus the time the thread went idle, 0 not armed. Each hook reads the clock
 * once and stores once (Budget: B5, B21); awake() and the end of an idle section also load the
 * watchdog's "parked" flag (a Dekker handshake: store, then load; the watchdog stores parked,
 * then loads progressNs) and wake it only if it is parked.
 *
 * The watchdog thread ("frost-watchdog", above normal priority) polls every 10 ms while the UI
 * thread is busy (Windows: a high-resolution waitable timer, never timeBeginPeriod), and parks
 * after 1 s of idleness until the next awake() (or unannounced wake). Once a busy stretch is
 * 25 ms old it samples the UI thread's stack every 10 ms (at most 64 frames), for 100 samples,
 * then every 100 ms, at most 200 samples (Budget: B19 for 200 x 64 x 8 bytes). A stretch that
 * ends after more than the threshold becomes a report in
 * <freezes>/freeze-<UTC>-<pid>-<seq>.json (frost/diag/freeze_report.hpp); one still running
 * after 2 s is written early with "ongoing": true and replaced atomically by the final report. At most 20 reports per
 * session, at least 10 s apart; the others are counted as dropped. A poll that wakes more than 30 ms late (the machine
 * was suspended or starved) marks the stretch unreliable. Stalls while a debugger is attached are counted separately
 * and never reported.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/diag/thread_sampler.hpp"
#include "frost/io/file_system.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace frost::diag {

inline constexpr IClock::Duration kStallThreshold = std::chrono::milliseconds(50);

struct StallStats {
    std::uint64_t stalls = 0;         // Counted by the UI thread (B21).
    IClock::Duration longest{};       // The longest stall so far.
    std::uint64_t unreliable = 0;     // Stalls the watchdog saw with a late wake.
    std::uint64_t whileDebugged = 0;  // Stalls the watchdog saw with a debugger attached.
    std::uint64_t reportsWritten = 0; // Final reports written.
    std::uint64_t reportsDropped = 0; // Stalls over the report limits.
    std::uint64_t watchdogStalls = 0; // Stalls as the watchdog saw them (agrees with `stalls`).
};

/*
 * The UI thread's half: the state machine over the hook calls, with explicit times, so tests can
 * drive it exactly. Single writer (the UI thread); progressNs() and the counters may be read from
 * any thread.
 */
class StallDetector {
public:
    explicit StallDetector(IClock::Duration threshold = kStallThreshold) noexcept : m_threshold(threshold) {}

    void eventLoopWoke(IClock::SteadyTime now) noexcept;
    void eventLoopWillBlock(IClock::SteadyTime now) noexcept;
    // Returns true when the event began a busy stretch (an unannounced wake; see "busy").
    bool eventStarted(IClock::SteadyTime now) noexcept;
    void idleBegin(IClock::SteadyTime now) noexcept;
    void idleEnd(IClock::SteadyTime now) noexcept;

    [[nodiscard]] bool armed() const noexcept { return m_armed; }
    [[nodiscard]] std::uint64_t stalls() const noexcept { return m_stalls.load(std::memory_order_relaxed); }
    [[nodiscard]] IClock::Duration longest() const noexcept {
        return IClock::Duration(m_longestNs.load(std::memory_order_relaxed));
    }
    // See the header comment. Relaxed is enough for the watchdog's estimate; the parked handshake
    // in StallWatchdog uses sequentially consistent operations on its own.
    [[nodiscard]] std::int64_t progressNs() const noexcept { return m_progressNs.load(std::memory_order_seq_cst); }

private:
    // Ends the current no-progress stretch at `now`, counting it if it was a stall.
    void closeStretch(IClock::SteadyTime now) noexcept;
    void publish(std::int64_t value) noexcept { m_progressNs.store(value, std::memory_order_seq_cst); }
    static std::int64_t ns(IClock::SteadyTime time) noexcept;

    IClock::Duration m_threshold;
    bool m_armed = false;
    bool m_busy = false;
    int m_idleDepth = 0;
    IClock::SteadyTime m_lastProgress{};
    std::atomic<std::uint64_t> m_stalls{0};
    std::atomic<std::int64_t> m_longestNs{0};
    std::atomic<std::int64_t> m_progressNs{0};
};

class StallWatchdog {
public:
    struct Options {
        std::string freezesDirectory; // Must exist when a report is written.
        std::string version;          // The report's "version".
        std::string threadName = "main";
        IClock::Duration threshold = kStallThreshold;
        IClock::Duration samplingStart = std::chrono::milliseconds(25);
        IClock::Duration samplePeriod = std::chrono::milliseconds(10);
        IClock::Duration slowSamplePeriod = std::chrono::milliseconds(100);
        std::size_t fastSamples = 100;
        std::size_t maxSamples = 200;
        IClock::Duration pollPeriod = std::chrono::milliseconds(10);
        IClock::Duration parkAfter = std::chrono::seconds(1);
        IClock::Duration interimAfter = std::chrono::seconds(2);
        IClock::Duration lateWake = std::chrono::milliseconds(30);
        std::size_t maxReports = 20;
        IClock::Duration minReportGap = std::chrono::seconds(10);
        // Whether a debugger is attached; the OS check when empty.
        std::function<bool()> debuggerPresent;
        // false: no thread; the owner calls poll() (tests, with a ManualClock).
        bool threaded = true;
    };

    // What the self-test and the tests read back about the latest report.
    struct ReportInfo {
        std::string path;
        std::size_t samples = 0;
        std::int64_t durationMs = 0;
        bool ongoing = false;
        bool unreliable = false;
    };

    /*
     * Starts the watchdog for the thread `sampler` samples; the hooks below must then be called
     * on that thread. `fs` and `clock` must outlive the watchdog. Errors: InvalidArgument (no
     * sampler, no freezes directory, a zero period), IoError (no thread or timer).
     */
    [[nodiscard]] static Result<std::unique_ptr<StallWatchdog>>
    start(Options options, std::unique_ptr<IThreadSampler> sampler, io::IFileSystem& fs, const IClock& clock);

    StallWatchdog(const StallWatchdog&) = delete ("owns the watchdog thread");
    StallWatchdog& operator=(const StallWatchdog&) = delete ("owns the watchdog thread");
    ~StallWatchdog(); // stop()

    // UI-thread hooks (see the header comment). noexcept, allocation-free.
    void eventLoopWoke() noexcept;
    void eventLoopWillBlock() noexcept;
    void eventStarted() noexcept;
    void idleBegin() noexcept;
    void idleEnd() noexcept;

    // One watchdog step at the clock's current time; for Options::threaded == false.
    void poll();

    // Thread-safe.
    [[nodiscard]] StallStats stats() const noexcept;
    [[nodiscard]] std::optional<ReportInfo> lastReport() const;
    [[nodiscard]] const StallDetector& detector() const noexcept;

    /*
     * Stops and joins the watchdog thread; bounded by one sample or one report write in
     * progress. A stall in progress is abandoned (its interim report, if any, stays). Idempotent;
     * call it from the UI thread before the event loop's objects are destroyed.
     */
    void stop() noexcept;

    /*
     * The process's UI watchdog, which ScopedUiIdle's default constructor uses: frost's program
     * installs its watchdog after starting it and uninstalls it (nullptr) before stopping it.
     * Acquire/release atomic; nullptr when none is installed.
     */
    static void install(StallWatchdog* watchdog) noexcept;
    [[nodiscard]] static StallWatchdog* installed() noexcept;

    struct Impl;

private:
    explicit StallWatchdog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> m_impl;
};

/*
 * Marks a section of UI-thread code that may legitimately block without being a stall (a
 * native modal loop frost does not see, such as QDrag::exec or a native file dialog, or a
 * deliberate wait). The default constructor uses StallWatchdog::installed() and does nothing
 * when no watchdog is installed. UI thread only; null-safe; nests.
 */
class ScopedUiIdle {
public:
    ScopedUiIdle() noexcept : ScopedUiIdle(StallWatchdog::installed()) {}
    explicit ScopedUiIdle(StallWatchdog* watchdog) noexcept : m_watchdog(watchdog) {
        if (m_watchdog != nullptr) {
            m_watchdog->idleBegin();
        }
    }
    ScopedUiIdle(const ScopedUiIdle&) = delete ("ends one idle section");
    ScopedUiIdle& operator=(const ScopedUiIdle&) = delete ("ends one idle section");
    ~ScopedUiIdle() {
        if (m_watchdog != nullptr) {
            m_watchdog->idleEnd();
        }
    }

private:
    StallWatchdog* m_watchdog;
};

} // namespace frost::diag
