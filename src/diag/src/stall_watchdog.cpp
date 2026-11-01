// SPDX-License-Identifier: Apache-2.0
//
// The stall detector and watchdog (frost/diag/stall_watchdog.hpp).

#include "frost/diag/stall_watchdog.hpp"

#include "frost/core/log.hpp"
#include "frost/diag/freeze_report.hpp"
#include "frost/io/path.hpp"

#include "internal.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#endif

namespace frost::diag {

// --- StallDetector (UI thread) ---

// The published encoding of a time: its count plus one, so a clock that starts at zero (a test's
// ManualClock) never publishes 0, which means "not armed".
std::int64_t StallDetector::ns(IClock::SteadyTime time) noexcept { return time.time_since_epoch().count() + 1; }

void StallDetector::closeStretch(IClock::SteadyTime now) noexcept {
    const IClock::Duration stretch = now - m_lastProgress;
    if (stretch > m_threshold) {
        m_stalls.fetch_add(1, std::memory_order_relaxed);
        if (stretch.count() > m_longestNs.load(std::memory_order_relaxed)) {
            m_longestNs.store(stretch.count(), std::memory_order_relaxed);
        }
    }
}

void StallDetector::eventLoopWoke(IClock::SteadyTime now) noexcept {
    if (!m_armed) {
        return;
    }
    if (m_busy && m_idleDepth == 0) {
        closeStretch(now); // A second awake() without aboutToBlock() is progress.
    }
    m_busy = true;
    m_lastProgress = now;
    if (m_idleDepth == 0) {
        publish(ns(now));
    }
}

void StallDetector::eventLoopWillBlock(IClock::SteadyTime now) noexcept {
    if (!m_armed) {
        m_armed = true;
    } else if (m_busy && m_idleDepth == 0) {
        closeStretch(now);
    }
    m_busy = false;
    publish(-ns(now));
}

bool StallDetector::eventStarted(IClock::SteadyTime now) noexcept {
    if (!m_armed || m_idleDepth > 0) {
        return false;
    }
    if (!m_busy) {
        // An event while the loop counts as blocked is a wake nobody announced: the glib
        // dispatcher emits aboutToBlock() and then dispatches inside the same iteration without
        // awake(), and Windows delivers sent messages inside the wait.
        m_busy = true;
        m_lastProgress = now;
        publish(ns(now));
        return true;
    }
    closeStretch(now);
    m_lastProgress = now;
    publish(ns(now));
    return false;
}

void StallDetector::idleBegin(IClock::SteadyTime now) noexcept {
    if (m_idleDepth++ == 0 && m_armed && m_busy) {
        closeStretch(now);
        publish(-ns(now));
    }
}

void StallDetector::idleEnd(IClock::SteadyTime now) noexcept {
    if (m_idleDepth == 0) {
        return; // Unbalanced; ignore rather than go negative.
    }
    if (--m_idleDepth == 0 && m_armed && m_busy) {
        m_lastProgress = now;
        publish(ns(now));
    }
}

// --- StallWatchdog ---

namespace {

IClock::SteadyTime decode(std::int64_t published) noexcept {
    return IClock::SteadyTime(IClock::Duration(published - 1));
}

std::int64_t millis(IClock::Duration duration) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

bool osDebuggerPresent() {
#ifdef _WIN32
    return IsDebuggerPresent() != FALSE;
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("TracerPid:")) {
            return line.find_first_not_of("0 \t", 10) != std::string::npos;
        }
    }
    return false;
#endif
}

} // namespace

struct StallWatchdog::Impl {
    Impl(Options options_, std::unique_ptr<IThreadSampler> sampler_, io::IFileSystem& fs_, const IClock& clock_)
        : options(std::move(options_)), sampler(std::move(sampler_)), fs(fs_), clock(clock_),
          detector(options.threshold), samples(options.maxSamples), sampleTimes(options.maxSamples) {
        if (!options.debuggerPresent) {
            options.debuggerPresent = &osDebuggerPresent;
        }
    }

    Options options;
    std::unique_ptr<IThreadSampler> sampler;
    io::IFileSystem& fs;
    const IClock& clock;
    StallDetector detector;

    std::atomic<bool> parked{false};
    std::atomic<bool> stopping{false};
    std::thread thread;
#ifdef _WIN32
    HANDLE timer = nullptr;
#endif

    // Watchdog-thread state.
    IClock::SteadyTime started{};
    bool watching = false;
    std::int64_t watchedValue = 0;
    IClock::SteadyTime stretchStart{};
    IClock::SystemTime stretchStartUtc{};
    std::vector<StackSample> samples;            // Preallocated (Budget: B19).
    std::vector<IClock::SteadyTime> sampleTimes; // Preallocated.
    std::size_t sampleCount = 0;
    IClock::SteadyTime nextSample{};
    bool unreliable = false;
    bool debugged = false;
    bool reporting = false; // A report path was allotted to the watched stall.
    bool interimWritten = false;
    std::string reportPath;
    std::optional<IClock::SteadyTime> lastPoll;
    std::size_t reportsStarted = 0;
    std::optional<IClock::SteadyTime> lastReportAt;
    std::uint64_t sequence = 0;

    std::atomic<std::uint64_t> unreliableStalls{0};
    std::atomic<std::uint64_t> whileDebugged{0};
    std::atomic<std::uint64_t> reportsWritten{0};
    std::atomic<std::uint64_t> reportsDropped{0};
    std::atomic<std::uint64_t> watchdogStalls{0};

    mutable std::mutex lastMutex;
    std::optional<ReportInfo> last;

    void wakeIfParked() noexcept {
        // The UI thread's half of the handshake: its progress store (seq_cst) precedes this load.
        if (parked.load(std::memory_order_seq_cst)) {
            parked.store(false, std::memory_order_seq_cst);
            parked.notify_one();
        }
    }

    void begin(std::int64_t value, IClock::SteadyTime now) {
        watching = true;
        watchedValue = value;
        stretchStart = decode(value);
        stretchStartUtc = clock.systemNow() - (now - stretchStart);
        sampleCount = 0;
        nextSample = stretchStart + options.samplingStart;
        unreliable = false;
        debugged = false;
        reporting = false;
        interimWritten = false;
        reportPath.clear();
    }

    void takeSample(IClock::SteadyTime now) {
        StackSample& sample = samples[sampleCount];
        if (sampler->sample(sample)) {
            sampleTimes[sampleCount] = now;
            ++sampleCount;
        }
        const IClock::Duration period =
            sampleCount < options.fastSamples ? options.samplePeriod : options.slowSamplePeriod;
        nextSample = now + period;
    }

    // Allots a report to the watched stall if the session limits allow one.
    bool allotReport(IClock::SteadyTime now) {
        if (reporting) {
            return true;
        }
        if (reportsStarted >= options.maxReports ||
            (lastReportAt.has_value() && now - *lastReportAt < options.minReportGap)) {
            return false;
        }
        ++reportsStarted;
        lastReportAt = now;
        reporting = true;
        auto path =
            io::joinPath(options.freezesDirectory, std::format("freeze-{}-{}-{}.json", compactUtc(stretchStartUtc),
                                                               currentProcessId(), ++sequence));
        reportPath = path ? std::move(*path) : std::string();
        return !reportPath.empty();
    }

    void writeReport(IClock::Duration duration, bool ongoing) {
        FreezeReport report;
        report.version = options.version;
        report.pid = currentProcessId();
        report.threadId = sampler->threadId();
        report.threadName = options.threadName;
        report.startedUtc = isoUtcMillis(stretchStartUtc);
        report.durationMs = millis(duration);
        report.ongoing = ongoing;
        report.unreliable = unreliable;
        report.thresholdMs = millis(options.threshold);
        report.samplePeriodMs = millis(options.samplePeriod);
        ModuleTable modules;
        report.samples.reserve(sampleCount);
        for (std::size_t i = 0; i < sampleCount; ++i) {
            FreezeSample sample;
            sample.tMs = millis(sampleTimes[i] - stretchStart);
            sample.frames.reserve(samples[i].count);
            for (std::size_t f = 0; f < samples[i].count; ++f) {
                const std::uintptr_t address = samples[i].frames[f];
                const int index = modules.indexOf(address);
                sample.frames.emplace_back(
                    index, index < 0 ? address : address - modules.modules()[static_cast<std::size_t>(index)].base);
            }
            report.samples.push_back(std::move(sample));
        }
        report.modules = modules.modules();
        auto written = fs.writeFileAtomic(reportPath, toJson(report));
        if (!written) {
            log::warn("diag", "freeze report {} not written: {}", reportPath, written.error().describe());
            return;
        }
        if (!ongoing) {
            reportsWritten.fetch_add(1, std::memory_order_relaxed);
        }
        log::warn("diag", "UI thread stalled for {} ms{}; freeze report {}", report.durationMs,
                  ongoing ? " so far" : "", reportPath);
        const std::scoped_lock lock(lastMutex);
        last = ReportInfo{reportPath, sampleCount, report.durationMs, ongoing, unreliable};
    }

    void finish(IClock::SteadyTime end) {
        watching = false;
        const IClock::Duration duration = end - stretchStart;
        if (duration <= options.threshold) {
            return;
        }
        watchdogStalls.fetch_add(1, std::memory_order_relaxed);
        if (unreliable) {
            unreliableStalls.fetch_add(1, std::memory_order_relaxed);
        }
        debugged = debugged || options.debuggerPresent();
        if (debugged) {
            whileDebugged.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!allotReport(end)) {
            reportsDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeReport(duration, false);
    }

    void poll(IClock::SteadyTime now) {
        if (watching && lastPoll.has_value() && now - *lastPoll > options.pollPeriod + options.lateWake) {
            unreliable = true;
        }
        lastPoll = now;
        const std::int64_t value = detector.progressNs();
        if (watching && value != watchedValue) {
            finish(value > 0 ? decode(value) : value < 0 ? decode(-value) : now);
        }
        if (value <= 0) {
            return;
        }
        if (!watching) {
            begin(value, now);
        }
        const IClock::Duration elapsed = now - stretchStart;
        if (elapsed >= options.samplingStart && sampleCount < options.maxSamples && now >= nextSample) {
            takeSample(now);
        }
        if (elapsed > options.interimAfter && !interimWritten) {
            interimWritten = true;
            debugged = debugged || options.debuggerPresent();
            if (!debugged && allotReport(now)) {
                writeReport(elapsed, true);
            }
        }
    }

    // How long the UI thread has been idle (or the watchdog unarmed), or nullopt while busy.
    std::optional<IClock::Duration> idleFor(IClock::SteadyTime now) const noexcept {
        const std::int64_t value = detector.progressNs();
        if (value > 0 || watching) {
            return std::nullopt;
        }
        return now - (value < 0 ? decode(-value) : started);
    }

    void sleepPollPeriod() noexcept {
#ifdef _WIN32
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(options.pollPeriod.count() / 100); // 100 ns units, relative.
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE) {
                (void)WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_for(options.pollPeriod);
    }

    void run() noexcept {
        nameAndGuardThisThread("frost-watchdog");
#ifdef _WIN32
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        while (!stopping.load(std::memory_order_acquire)) {
            const IClock::SteadyTime now = clock.steadyNow();
            try {
                poll(now);
            } catch (...) {
                // Out of memory while writing a report: drop it and keep watching.
            }
            if (const auto idle = idleFor(now); idle.has_value() && *idle >= options.parkAfter) {
                // The watchdog's half of the handshake: store parked, then load the progress.
                parked.store(true, std::memory_order_seq_cst);
                if (detector.progressNs() > 0 || stopping.load(std::memory_order_seq_cst)) {
                    parked.store(false, std::memory_order_seq_cst);
                    continue;
                }
                parked.wait(true, std::memory_order_seq_cst);
                lastPoll.reset(); // A parked sleep is not a late wake.
                continue;
            }
            sleepPollPeriod();
        }
    }
};

Result<std::unique_ptr<StallWatchdog>> StallWatchdog::start(Options options, std::unique_ptr<IThreadSampler> sampler,
                                                            io::IFileSystem& fs, const IClock& clock) {
    if (sampler == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the stall watchdog needs a thread sampler");
    }
    if (options.freezesDirectory.empty()) {
        return fail(ErrorCode::InvalidArgument, "the stall watchdog needs a freezes directory");
    }
    if (options.pollPeriod <= IClock::Duration::zero() || options.samplePeriod <= IClock::Duration::zero() ||
        options.slowSamplePeriod <= IClock::Duration::zero() || options.maxSamples == 0) {
        return fail(ErrorCode::InvalidArgument, "the stall watchdog's periods and sample limit must be positive");
    }
    auto impl = std::make_unique<Impl>(std::move(options), std::move(sampler), fs, clock);
    impl->started = clock.steadyNow();
    if (impl->options.threaded) {
#ifdef _WIN32
        // A high-resolution timer gives 10 ms polls without raising the system timer
        // resolution (no timeBeginPeriod).
        impl->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (impl->timer == nullptr) {
            return fail(ErrorCode::IoError, std::format("CreateWaitableTimerExW failed ({})", GetLastError()));
        }
#endif
        Impl* raw = impl.get();
        try {
            impl->thread = std::thread([raw] { raw->run(); });
        } catch (const std::system_error& error) {
#ifdef _WIN32
            CloseHandle(impl->timer);
#endif
            return fail(ErrorCode::IoError, std::string("cannot start the watchdog thread: ") + error.what());
        }
    }
    return std::unique_ptr<StallWatchdog>(new StallWatchdog(std::move(impl)));
}

StallWatchdog::StallWatchdog(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

StallWatchdog::~StallWatchdog() {
    stop();
#ifdef _WIN32
    if (m_impl->timer != nullptr) {
        CloseHandle(m_impl->timer);
    }
#endif
}

void StallWatchdog::eventLoopWoke() noexcept {
    m_impl->detector.eventLoopWoke(m_impl->clock.steadyNow());
    m_impl->wakeIfParked();
}

void StallWatchdog::eventLoopWillBlock() noexcept { m_impl->detector.eventLoopWillBlock(m_impl->clock.steadyNow()); }

void StallWatchdog::eventStarted() noexcept {
    if (m_impl->detector.eventStarted(m_impl->clock.steadyNow())) {
        m_impl->wakeIfParked();
    }
}

void StallWatchdog::idleBegin() noexcept { m_impl->detector.idleBegin(m_impl->clock.steadyNow()); }

void StallWatchdog::idleEnd() noexcept {
    m_impl->detector.idleEnd(m_impl->clock.steadyNow());
    m_impl->wakeIfParked();
}

void StallWatchdog::poll() { m_impl->poll(m_impl->clock.steadyNow()); }

StallStats StallWatchdog::stats() const noexcept {
    StallStats stats;
    stats.stalls = m_impl->detector.stalls();
    stats.longest = m_impl->detector.longest();
    stats.unreliable = m_impl->unreliableStalls.load(std::memory_order_relaxed);
    stats.whileDebugged = m_impl->whileDebugged.load(std::memory_order_relaxed);
    stats.reportsWritten = m_impl->reportsWritten.load(std::memory_order_relaxed);
    stats.reportsDropped = m_impl->reportsDropped.load(std::memory_order_relaxed);
    stats.watchdogStalls = m_impl->watchdogStalls.load(std::memory_order_relaxed);
    return stats;
}

std::optional<StallWatchdog::ReportInfo> StallWatchdog::lastReport() const {
    const std::scoped_lock lock(m_impl->lastMutex);
    return m_impl->last;
}

const StallDetector& StallWatchdog::detector() const noexcept { return m_impl->detector; }

namespace {
constinit std::atomic<StallWatchdog*> g_installed{nullptr};
} // namespace

void StallWatchdog::install(StallWatchdog* watchdog) noexcept {
    g_installed.store(watchdog, std::memory_order_release);
}

StallWatchdog* StallWatchdog::installed() noexcept { return g_installed.load(std::memory_order_acquire); }

void StallWatchdog::stop() noexcept {
    if (!m_impl->thread.joinable()) {
        return;
    }
    m_impl->stopping.store(true, std::memory_order_seq_cst);
    m_impl->parked.store(false, std::memory_order_seq_cst);
    m_impl->parked.notify_one();
    m_impl->thread.join();
}

} // namespace frost::diag
