// SPDX-License-Identifier: Apache-2.0
//
// ThreadPool: workers, work finding, the low-lane cap, parking and timers
// (see frost/sched/thread_pool.hpp for the design and ADR 0002 for the measurements).

#include "frost/sched/thread_pool.hpp"

#include "frost/core/assert.hpp"
#include "frost/sched/chase_lev_deque.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/sched/lane_policy.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace frost::sched {

struct ThreadPool::Worker {
    Worker(unsigned workerIndex, std::uint64_t seed) noexcept : index(workerIndex), rng(seed) {}

    // One deque per lane. Pushed and popped by this worker; stolen from by the others.
    std::array<ChaseLevDeque<void>, kLaneCount> deques;
    // The remaining members belong to the worker's own thread.
    LaneSelector selector;
    std::uint64_t dispatches = 0;
    unsigned index;
    detail::SplitMix64 rng;
    std::thread thread; // Written by the constructing thread, joined by shutdown(); the worker never touches it.
};

namespace {

// What the calling thread is doing for which pool. constinit and trivially
// destructible, so no thread runs initialisation or exit-time code for it.
struct WorkerContext {
    ThreadPool* pool = nullptr;
    ThreadPool::Worker* worker = nullptr;
    Lane lane = Lane::Interactive;
    bool inSegment = false;
};
constinit thread_local WorkerContext t_context{};

// A short pause inside spin loops, so a spinning worker yields pipeline
// resources to its hyper-thread sibling.
void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// Pauses per spin round before a worker re-searches (~1-2 us on current x86 parts).
constexpr int kPausesPerSpinRound = 32;

// The longest single timer-thread wait. Bounds the arithmetic for "sleep forever"
// deadlines (SteadyTime::max()), which some standard libraries overflow.
constexpr std::chrono::hours kMaxTimerWait{1};

// Names threads for debuggers and profilers ("frost-w3", "frost-timer").
void nameThisThread(const char* prefix, unsigned index, bool numbered) {
    std::string name = prefix;
    if (numbered) {
        name += std::to_string(index);
    }
#ifdef _WIN32
    const std::wstring wide(name.begin(), name.end()); // ASCII only.
    (void)SetThreadDescription(GetCurrentThread(), wide.c_str());
#else
    name.resize(std::min<std::size_t>(name.size(), 15)); // Linux limit: 16 bytes including the terminator.
    (void)pthread_setname_np(pthread_self(), name.c_str());
#endif
}

} // namespace

bool detail::onPoolWorkerThread() noexcept { return t_context.pool != nullptr; }

unsigned ThreadPool::defaultWorkerCount() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? hardware - 1 : 1;
}

unsigned ThreadPool::defaultLowLaneCap() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 4 ? hardware - 4 : 1;
}

ThreadPool::ThreadPool(ThreadPoolOptions options) : m_clock(options.clock != nullptr ? options.clock : &systemClock()) {
    const unsigned workers = options.workerCount != 0 ? options.workerCount : defaultWorkerCount();
    const unsigned requestedCap = options.lowLaneCap != 0 ? options.lowLaneCap : defaultLowLaneCap();
    // At most workers - 1, so one worker is always free of low-lane work.
    m_lowCap = workers > 1 ? std::clamp(requestedCap, 1U, workers - 1) : 1U;

    detail::SplitMix64 seeder(options.seed);
    m_workers.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        m_workers.push_back(std::make_unique<Worker>(i, seeder.next()));
    }
    // Threads start only after every Worker exists, because thieves index m_workers.
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        Worker* raw = worker.get();
        worker->thread = std::thread([this, raw] { workerMain(*raw); });
    }
    m_timerThread = std::thread([this] { timerMain(); });
}

ThreadPool::~ThreadPool() {
    shutdown();
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        for (const ChaseLevDeque<void>& deque : worker->deques) {
            FROST_ASSERT(deque.sizeApprox() == 0);
        }
    }
}

void ThreadPool::shutdown() {
    FROST_PRE(t_context.pool != this);
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    m_stopSource.request_stop();
    {
        LockGuard lock(m_timerMutex);
        m_timerStopping = true;
    }
    m_timerCv.notify_one();
    // The timer thread hands every pending sleep to the queues before it exits.
    m_timerThread.join();
    // Seq_cst: Dekker handshake with finishUnit(). Either the unit that brings
    // m_outstanding to zero sees m_draining, or this load sees zero.
    m_draining.store(true, std::memory_order_seq_cst);
    if (m_outstanding.load(std::memory_order_seq_cst) == 0) {
        signalExit();
    }
    for (const std::unique_ptr<Worker>& worker : m_workers) {
        worker->thread.join();
    }
}

void ThreadPool::waitIdle() {
    FROST_PRE(t_context.pool != this);
    // Seq_cst: Dekker handshake with finishUnit(): register as a waiter, then read the count.
    m_idleWaiters.fetch_add(1, std::memory_order_seq_cst);
    {
        UniqueLock lock(m_idleMutex);
        while (m_outstanding.load(std::memory_order_seq_cst) != 0) {
            m_idleCv.wait(lock.native());
        }
    }
    m_idleWaiters.fetch_sub(1, std::memory_order_seq_cst);
}

void ThreadPool::notifyClockChanged() {
    {
        // Taking the lock orders this notification after a concurrent wait has started.
        LockGuard lock(m_timerMutex);
    }
    m_timerCv.notify_one();
}

ThreadPoolStats ThreadPool::stats() const noexcept {
    // Relaxed: a diagnostic snapshot, never used for synchronisation.
    ThreadPoolStats stats;
    for (std::size_t i = 0; i < kLaneCount; ++i) {
        stats.queued[i] = m_queued[i].value.load(std::memory_order_relaxed);
    }
    stats.lowRunning = m_lowRunning.load(std::memory_order_relaxed);
    stats.outstanding = m_outstanding.load(std::memory_order_relaxed);
    stats.parked = m_sleepers.load(std::memory_order_relaxed);
    return stats;
}

std::optional<Lane> ThreadPool::currentLane() const noexcept {
    if (t_context.pool == this && t_context.inSegment) {
        return t_context.lane;
    }
    return std::nullopt;
}

void ThreadPool::enqueue(std::coroutine_handle<> handle, Lane lane) noexcept {
    FROST_PRE(static_cast<bool>(handle));
    // Relaxed: a best-effort check of the "no work after shutdown" precondition.
    FROST_PRE(!m_exit.load(std::memory_order_relaxed));
    const std::size_t index = laneIndex(lane);
    // Both counters rise before the element is visible, so a worker that takes and
    // finishes it can never drive them below the true value. Seq_cst: m_queued is
    // the producer half of the parking handshake (see park()); m_outstanding uses
    // seq_cst for uniformity with finishUnit().
    m_outstanding.fetch_add(1, std::memory_order_seq_cst);
    m_queued[index].value.fetch_add(1, std::memory_order_seq_cst);
    if (t_context.pool == this) {
        t_context.worker->deques[index].push(handle.address());
    } else {
        InjectionQueue& queue = m_injection[index];
        LockGuard lock(queue.mutex);
        queue.items.push_back(handle.address());
        // Release: a reader that sees the new size also sees the element (it re-reads under the mutex anyway).
        queue.size.store(static_cast<std::int64_t>(queue.items.size()), std::memory_order_release);
    }
    // Seq_cst: after the m_queued store above, read m_sleepers (Dekker with park()).
    // Low-lane work is runnable only below the cap; when the cap is full, a worker
    // finishing a low-lane segment picks it up instead (see runSegment()).
    if (m_sleepers.load(std::memory_order_seq_cst) > 0 &&
        (!isLowLane(lane) || m_lowRunning.load(std::memory_order_seq_cst) < m_lowCap)) {
        wakeOne();
    }
}

void ThreadPool::workerMain(Worker& worker) {
    t_context.pool = this;
    t_context.worker = &worker;
    nameThisThread("frost-w", worker.index, true);
    for (;;) {
        if (const std::optional<Found> found = findWork(worker)) {
            runSegment(*found);
            continue;
        }
        bool ran = false;
        for (unsigned round = 0; round < kSpinRounds && !ran; ++round) {
            for (int i = 0; i < kPausesPerSpinRound; ++i) {
                cpuRelax();
            }
            if (const std::optional<Found> found = findWork(worker)) {
                runSegment(*found);
                ran = true;
            }
        }
        if (!ran && !park()) {
            break;
        }
    }
    t_context = WorkerContext{};
}

std::optional<ThreadPool::Found> ThreadPool::findWork(Worker& worker) {
    // Relaxed: these counters only steer the search. A stale zero at worst sends
    // the worker to park(), whose seq_cst re-check sees the work.
    LaneMask eligible = 0;
    for (const Lane lane : kAllLanes) {
        if (m_queued[laneIndex(lane)].value.load(std::memory_order_relaxed) > 0) {
            eligible = static_cast<LaneMask>(eligible | laneBit(lane));
        }
    }
    if ((eligible & kLowLanes) != 0 && m_lowRunning.load(std::memory_order_relaxed) >= m_lowCap) {
        eligible = static_cast<LaneMask>(eligible & ~kLowLanes);
    }
    if (eligible == 0) {
        return std::nullopt;
    }
    const bool oldestFirst = worker.dispatches % kFifoPeriod == kFifoPeriod - 1;
    const LaneSelector::Order order = worker.selector.order(eligible);
    for (std::size_t i = 0; i < order.count; ++i) {
        const Lane lane = order.lanes[i];
        const bool low = isLowLane(lane);
        if (low && !tryReserveLowSlot()) {
            continue;
        }
        if (void* address = takeFromLane(worker, lane, oldestFirst)) {
            // Acq_rel: the decrement is ordered after the take that removed the element.
            m_queued[laneIndex(lane)].value.fetch_sub(1, std::memory_order_acq_rel);
            worker.selector.dispatched(lane, eligible);
            ++worker.dispatches;
            return Found{address, lane};
        }
        if (low) {
            releaseLowSlot();
        }
    }
    return std::nullopt;
}

void* ThreadPool::takeFromLane(Worker& worker, Lane lane, bool oldestFirst) {
    ChaseLevDeque<void>& own = worker.deques[laneIndex(lane)];
    if (oldestFirst) {
        if (void* address = popInjection(lane)) {
            return address;
        }
        // The owner may steal from its own deque; that takes the oldest element.
        for (unsigned attempt = 0; attempt <= kStealRetries; ++attempt) {
            const ChaseLevDeque<void>::StealResult result = own.steal();
            if (result.status == ChaseLevDeque<void>::StealStatus::Success) {
                return result.item;
            }
            if (result.status == ChaseLevDeque<void>::StealStatus::Empty) {
                break;
            }
        }
    }
    if (void* address = own.pop()) {
        return address;
    }
    if (!oldestFirst) {
        if (void* address = popInjection(lane)) {
            return address;
        }
    }
    return stealFromOthers(worker, lane);
}

void* ThreadPool::popInjection(Lane lane) {
    InjectionQueue& queue = m_injection[laneIndex(lane)];
    // Acquire: pairs with the release store in enqueue(); a hint that skips the lock when empty.
    if (queue.size.load(std::memory_order_acquire) <= 0) {
        return nullptr;
    }
    LockGuard lock(queue.mutex);
    if (queue.items.empty()) {
        return nullptr;
    }
    void* address = queue.items.front();
    queue.items.pop_front();
    // Relaxed: the mutex orders it; readers treat it as a hint.
    queue.size.store(static_cast<std::int64_t>(queue.items.size()), std::memory_order_relaxed);
    return address;
}

void* ThreadPool::stealFromOthers(Worker& worker, Lane lane) {
    const std::size_t count = m_workers.size();
    if (count < 2) {
        return nullptr;
    }
    const std::size_t start = static_cast<std::size_t>(worker.rng.below(count));
    for (std::size_t i = 0; i < count; ++i) {
        Worker& victim = *m_workers[(start + i) % count];
        if (&victim == &worker) {
            continue;
        }
        ChaseLevDeque<void>& deque = victim.deques[laneIndex(lane)];
        // Cheap relaxed pre-check, so empty victims cost no fence.
        if (deque.sizeApprox() == 0) {
            continue;
        }
        for (unsigned attempt = 0; attempt <= kStealRetries; ++attempt) {
            const ChaseLevDeque<void>::StealResult result = deque.steal();
            if (result.status == ChaseLevDeque<void>::StealStatus::Success) {
                return result.item;
            }
            if (result.status == ChaseLevDeque<void>::StealStatus::Empty) {
                break;
            }
        }
    }
    return nullptr;
}

void ThreadPool::runSegment(Found found) {
    // Ramp up: if eligible work remains and a worker is parked, wake one more.
    // Seq_cst: this load follows this worker's own seq_cst updates (the low-slot
    // release after its previous segment, the m_queued decrement), completing the
    // handshake with a worker that parked because the cap looked full.
    if (m_sleepers.load(std::memory_order_seq_cst) > 0 && hasEligibleWork()) {
        wakeOne();
    }
    t_context.lane = found.lane;
    t_context.inSegment = true;
    std::coroutine_handle<>::from_address(found.address).resume();
    t_context.inSegment = false;
    if (isLowLane(found.lane)) {
        releaseLowSlot();
    }
    finishUnit();
}

/*
 * Parking without lost wakeups. A worker that found nothing:
 *   1. reads the event count (key = m_epoch),
 *   2. announces itself (m_sleepers++),
 *   3. re-checks for eligible work (hasEligibleWork()),
 *   4. waits while m_epoch == key.
 * A producer publishes work (m_queued++ before the push), then reads m_sleepers
 * and, if a worker is parked, bumps m_epoch and notifies. All of steps 2-3 and
 * the producer's counter update and m_sleepers read are seq_cst, so in their
 * single total order either the worker's re-check sees the work (and it does not
 * wait), or the producer sees m_sleepers > 0 and bumps m_epoch after the worker
 * read its key (so the wait returns at once or is woken). The same argument
 * covers a low-lane slot being released (m_lowRunning) and m_exit.
 */
bool ThreadPool::park() {
    const std::uint32_t key = m_epoch.load(std::memory_order_seq_cst);
    m_sleepers.fetch_add(1, std::memory_order_seq_cst);
    if (m_exit.load(std::memory_order_seq_cst)) {
        m_sleepers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    if (!hasEligibleWork()) {
        m_epoch.wait(key, std::memory_order_seq_cst);
    }
    m_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    return true;
}

bool ThreadPool::hasEligibleWork() const noexcept {
    // Seq_cst: the consumer half of the parking handshake (see park()).
    if (m_queued[laneIndex(Lane::Interactive)].value.load(std::memory_order_seq_cst) > 0 ||
        m_queued[laneIndex(Lane::UserVisible)].value.load(std::memory_order_seq_cst) > 0) {
        return true;
    }
    const bool lowQueued = m_queued[laneIndex(Lane::Background)].value.load(std::memory_order_seq_cst) > 0 ||
                           m_queued[laneIndex(Lane::Idle)].value.load(std::memory_order_seq_cst) > 0;
    return lowQueued && m_lowRunning.load(std::memory_order_seq_cst) < m_lowCap;
}

bool ThreadPool::tryReserveLowSlot() noexcept {
    // Relaxed initial read: the CAS below validates it.
    std::int64_t running = m_lowRunning.load(std::memory_order_relaxed);
    while (running < m_lowCap) {
        // Seq_cst on success: part of the parking handshake (hasEligibleWork() reads it).
        if (m_lowRunning.compare_exchange_weak(running, running + 1, std::memory_order_seq_cst,
                                               std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void ThreadPool::releaseLowSlot() noexcept {
    // Seq_cst: a parked worker's re-check must see the freed slot or be woken (see runSegment()).
    m_lowRunning.fetch_sub(1, std::memory_order_seq_cst);
}

void ThreadPool::wakeOne() noexcept {
    // Seq_cst: a worker that read the old value returns from wait(); one that
    // reads the new value re-checks for work after announcing itself.
    m_epoch.fetch_add(1, std::memory_order_seq_cst);
    m_epoch.notify_one();
}

void ThreadPool::finishUnit() noexcept {
    // Seq_cst: Dekker handshake with shutdown() (m_draining) and waitIdle() (m_idleWaiters).
    if (m_outstanding.fetch_sub(1, std::memory_order_seq_cst) != 1) {
        return;
    }
    if (m_idleWaiters.load(std::memory_order_seq_cst) > 0) {
        LockGuard lock(m_idleMutex);
        m_idleCv.notify_all();
    }
    if (m_draining.load(std::memory_order_seq_cst)) {
        signalExit();
    }
}

void ThreadPool::signalExit() noexcept {
    // Seq_cst: park() reads m_exit after announcing itself; the bump wakes the parked ones.
    m_exit.store(true, std::memory_order_seq_cst);
    m_epoch.fetch_add(1, std::memory_order_seq_cst);
    m_epoch.notify_all();
}

void ThreadPool::addTimer(TimerRequest& request, const std::stop_token& token) noexcept {
    const std::coroutine_handle<> handle = request.handle;
    const Lane lane = request.lane;
    bool fireNow = false;
    bool newEarliest = false;
    {
        LockGuard lock(m_timerMutex);
        if (m_timerStopping || token.stop_requested()) {
            request.expired = false;
            fireNow = true;
        } else {
            // The sleeping coroutine is outstanding work until its timer is handed to a queue.
            m_outstanding.fetch_add(1, std::memory_order_seq_cst);
            request.id = ++m_nextTimerId;
            const auto inserted =
                m_timers.emplace(TimerKey{request.deadline.time_since_epoch().count(), request.id}, &request);
            newEarliest = inserted.first == m_timers.begin();
        }
    }
    // `request` may be resumed and destroyed from here on; only copies are used.
    if (fireNow) {
        enqueue(handle, lane);
    } else if (newEarliest) {
        m_timerCv.notify_one();
    }
}

void ThreadPool::cancelTimer(TimerRequest& request) noexcept {
    std::coroutine_handle<> handle;
    Lane lane = Lane::UserVisible;
    {
        LockGuard lock(m_timerMutex);
        const auto it = m_timers.find(TimerKey{request.deadline.time_since_epoch().count(), request.id});
        if (it == m_timers.end() || it->second != &request) {
            return; // Not registered yet (addTimer() will see the stop) or already fired.
        }
        m_timers.erase(it);
        request.expired = false;
        handle = request.handle;
        lane = request.lane;
    }
    enqueue(handle, lane);
    finishUnit(); // The timer's unit moved to the queue.
}

void ThreadPool::collectDueTimers(std::vector<DueTimer>& due, bool all) {
    const IClock::Duration::rep now = m_clock->steadyNow().time_since_epoch().count();
    while (!m_timers.empty()) {
        const auto it = m_timers.begin();
        const bool reached = it->first.first <= now;
        if (!reached && !all) {
            break;
        }
        TimerRequest& request = *it->second;
        // The shutdown flush cuts every sleep short, as documented in shutdown(), even
        // one whose deadline has passed but which the timer thread had not collected yet.
        request.expired = reached && !all;
        due.emplace_back(request.handle, request.lane);
        m_timers.erase(it);
    }
}

void ThreadPool::timerMain() {
    nameThisThread("frost-timer", 0, false);
    std::vector<DueTimer> due;
    for (;;) {
        bool stopping = false;
        {
            UniqueLock lock(m_timerMutex);
            for (;;) {
                stopping = m_timerStopping;
                collectDueTimers(due, stopping);
                if (!due.empty() || stopping) {
                    break;
                }
                if (m_timers.empty()) {
                    m_timerCv.wait(lock.native());
                } else {
                    // Real time to wait for the clock to reach the earliest deadline.
                    // For a ManualClock this is only an upper bound; notifyClockChanged() wakes us early.
                    const IClock::Duration remaining =
                        IClock::Duration(m_timers.begin()->first.first) - m_clock->steadyNow().time_since_epoch();
                    m_timerCv.wait_for(lock.native(), std::min<IClock::Duration>(remaining, kMaxTimerWait));
                }
            }
        }
        for (const auto& [handle, lane] : due) {
            enqueue(handle, lane);
            finishUnit(); // The timer's unit moved to the queue.
        }
        due.clear();
        if (stopping) {
            return;
        }
    }
}

} // namespace frost::sched
