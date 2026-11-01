# ADR 0002: Scheduler (frost::sched)

- Status: accepted. **Decided 2026-11-01 (ADR 0011, G2)**, on the user's instruction of that day to follow the
  recommendation: `m0/sched` is merged and was independently reviewed, and its M0 exit numbers are met,
  including the exit-scale TSan re-run on 3563873. Until then the status read: proposed
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("Scheduler" row), 3.3 (threading), 6.1 (concurrency tests),
  6.2 (IScheduler → SimExecutor), 7 (M0 deliverables and exit)

## Context
Everything asynchronous in FrostIDE (indexing, parsing, search, project evaluation, agent traffic) runs on
one scheduler. PLAN.md fixes the shape: N−1 work-stealing workers, four lanes (Interactive > UserVisible >
Background > Idle) with aging, Background+Idle capped at cores−4, `Task<T>` coroutines, `std::stop_token`
plus "latest request wins", and a seeded SimExecutor with virtual time for deterministic tests. M0 exits on:

1. Interactive dispatch p99 ≤ 1 ms (submit → start) with 10k Background tasks queued and the Background
   workers saturated.
2. TSan clean over 10k SimExecutor seeds.
3. TSan clean over a real-pool stress run: 1M tasks across all lanes with cancellation, 100 iterations.

Constraints: C++23 in C++26 mode without std::execution (CLAUDE.md), no TBB (PLAN.md), no exceptions across
modules (`std::expected<T, frost::Error>`), all locked state annotated for `-Wthread-safety`, clean room.

## Decision

### Seam
`IScheduler` (scheduler.hpp) is the only type callers use. Its virtual surface is small: `enqueue(handle,
lane)`, `clock()`, `currentLane()`, `stopToken()`, `addTimer`/`cancelTimer`, and `callerDriven`/`driveOne`
(so a blocking join can drive a simulator). On top of it, non-virtually: `schedule(lane)`,
`sleepFor`/`sleepUntil(…, stop_token)`, `spawn(lane, task[, token]) → JoinHandle<T>`, and the free
`syncWait(task)`. `ThreadPool` is the real implementation and `SimExecutor` the double.

### Tasks
- `Task<T>` is lazy (`initial_suspend` is `suspend_always`), move-only, and awaiting it uses symmetric transfer
  in both directions, so arbitrarily long `co_await` chains use constant stack (a 1M-deep chain is tested).
  `T` may be `void` or move-only.
- Exceptions: an exception escaping a `Task<Result<U>>` body becomes `ErrorCode::Internal` with the
  exception's message. For any other `T` it is a contract violation: `reportViolationAndAbort("no exception
  escapes a Task body (caught: …)")`. Code that can throw (third-party, allocation) therefore returns
  `Result<U>` or catches locally.
- A coroutine stays on the lane it last ran on. Awaiting a `Task` runs it inline on the same lane; only
  `schedule()`, timers and `JoinHandle` completions re-dispatch, on the awaiter's lane (UserVisible if the
  awaiter was not on this scheduler).

### Spawn, join, cancellation
- `spawn` starts a refcounted runner frame (two references: the frame and the handle) and returns a
  `JoinHandle<T>` that can be awaited, `join()`ed from a non-worker thread, or `detach()`ed. A join state
  machine (Pending → Awaiting/Blocking → Done) on one atomic makes completion and awaiting race-free.
  A result can be taken once; a second take is a contract violation, not a moved-from value.
- Lifetime: shutdown drains, so once its scheduler is gone every handle whose task only waited on that
  scheduler is ready (a task suspended on another scheduler or an external event is not part of the drain).
  A ready handle may outlive its scheduler, and `join()` then only takes the result. A pending handle must
  not outlive it.
- `spawn(lane, task, token)` (T must be `Result<U>` or `void`) skips the body if `token` is stopped before
  the task starts: the handle reports `cancelledBeforeStart()`, and the result is `ErrorCode::Cancelled`
  (for `void`, nothing). Cancellation while running is cooperative: tasks check their token, and sleeps
  with a token end early (`co_await sleepFor(...)` returns `false`).
- `LatestWins` (latest_wins.hpp) is the generation-counter helper: `next()` stops the previous request's
  token and returns a ticket `{generation, token}`; `commitIfCurrent(ticket, fn)` runs `fn` under the
  helper's lock only if no newer request (or `cancel()`) has happened since.

### Lanes and aging
`LaneSelector` (lane_policy.hpp) is one policy object shared by the pool (one per worker) and the
simulator. Normally the highest-priority eligible lane wins. Each lane below Interactive counts the
dispatches that passed over it while it had eligible work; at `kAgingPasses = {0, 4, 16, 64}` dispatches
(Interactive, UserVisible, Background, Idle) it is aged, and the highest-priority aged lane goes next.
Dispatching a lane resets its counter.

- The unit is dispatches of the same dispatcher (a worker, or the simulator), not wall time: no clock read
  on the dispatch path, and the policy is exactly reproducible in SimExecutor.
- Guarantee: while lane L has eligible work, a dispatcher passes over it at most
  `maxWait(L) = kAgingPasses[L] + 3` consecutive times (3 for Interactive itself, when the three lower lanes
  come due together). It is checked for every subset of saturated lanes, under random eligibility, and on
  every SimExecutor stress trace by an independent oracle.
- With all four lanes saturated a dispatcher's turns split ≈ 74% / 19% / 6% / 1.5%.

### ThreadPool
- **Workers**: `hardware_concurrency − 1` by default (23 on REF-W1), configurable. Each worker owns one
  Chase-Lev deque per lane. Work enqueued from a worker goes to its own deque; from any other thread (UI,
  timer thread, later the reactor) to a per-lane injection queue (`frost::Mutex` + `std::deque`).
- **Finding work**: the worker asks its selector for a lane order. For each lane it tries its own deque
  (LIFO), the lane's injection queue (FIFO), then steals FIFO from the other workers starting at a random
  victim (`kStealRetries = 2` retries on an aborted steal). Every `kFifoPeriod = 32`nd dispatch of a worker
  takes the oldest element instead (injection queue, then the FIFO end of its own deque), so old work in a
  busy lane is not overtaken forever by newer pushes.
- **Low-lane cap**: at most `lowLaneCap()` Background+Idle *segments* (one resume until the next suspend)
  run at once. The default is `max(1, hardware_concurrency − 4)` (20 on REF-W1), and the effective cap is
  clamped to `[1, workers − 1]` so one worker is always free of low-lane work. Slots are reserved with a CAS
  on `m_lowRunning` before taking low-lane work and released when the segment ends; a suspended coroutine
  (sleeping, awaiting I/O or a join) holds no slot.
- **"Cores" means logical threads**: PLAN.md's "cores−4" is read as `hardware_concurrency − 4`, the same
  unit as the N−1 workers. On an SMT machine a worker running Interactive work may therefore share a
  physical core with a Background segment on its sibling thread. This is revisited with the M1 load
  measurements (indexing while typing), where a physical-core cap can be compared against it.
- **Parking**: idle workers spin (`kSpinRounds = 16` rounds of 32 `pause` instructions with a full search
  each), then park on an event count (`m_epoch`, `std::atomic::wait`). A producer bumps and notifies only if
  a worker is parked and the work is runnable under the cap; a worker that takes work while more eligible
  work remains wakes one more. The seq_cst Dekker handshake between the per-lane queue counters and
  `m_sleepers` (documented in `park()`) rules out lost wakeups.
- **Timers**: one timer thread with an ordered map keyed by (deadline, id), reading the injected `IClock`;
  due timers are enqueued on their awaiter's lane. Cancellation goes through the sleep's `stop_token`
  (callback registered before the timer, checked again under the timer lock). `notifyClockChanged()` lets a
  test clock re-evaluate deadlines.
- **Shutdown = drain**: `shutdown()` (also run by the destructor) requests the pool's stop token, completes
  every pending and future sleep immediately with `false`, runs all remaining work to completion, then joins
  the workers. Nothing is destroyed half-run, so every `JoinHandle` whose task only waits on this pool
  completes; tasks that loop must check `stopToken()` or their own token. An `m_outstanding` counter
  (queued + running segments + pending timers) detects quiescence for `shutdown()` and `waitIdle()`.

### Chase-Lev deque
`ChaseLevDeque<T>` follows Chase and Lev (SPAA 2005) with the C11 orderings of Lê, Pop, Cohen and Zappa
Nardelli, "Correct and Efficient Work-Stealing for Weak Memory Models" (PPoPP 2013), figure 1, written from
the paper. `top`/`bottom` are 64-bit and cache-line separated, the ring grows by doubling, and retired rings
stay allocated until the deque is destroyed (a thief may still read one; memory is bounded by twice the
final capacity). Every atomic access carries a comment justifying its ordering.

ThreadSanitizer does not model `atomic_thread_fence`, so with the paper's relaxed slot accesses it reports a
false race on the element's contents. In TSan builds (`FROST_SCHED_TSAN`) the slot store and load become
release/acquire. That only strengthens the paper's orderings, and on x86-64 both compile to the same moves.
`ChaseLevDeque.PushPublishesElementContentsToThieves` demonstrates it: with the paper's orderings under TSan
it reports a race; with the strengthening it is clean.

The flip side: the TSan runs check the strengthened variant, so they cannot check the fence-based
publication path that release builds use. Those orderings (exactly figure 1 of Lê et al.) are checked by
reasoning, recorded beside each atomic, and by tests on real hardware such as
`ChaseLevDeque.LastElementGoesToExactlyOneTaker` and the long owner-plus-thieves stress run, until the
TODO(M1) Relacy-style model check covers them. x86-64 reorders only a store with a later load, so these
tests can catch a missing seq_cst fence in `pop()`/`steal()` but not a too-weak release or acquire; an arm64
leg (PLAN.md, 1.x) would.

### SimExecutor
One dispatcher, no threads, driven by the test (`step`, `runUntilIdle`, `runUntil`, `runFor`, or
`JoinHandle::join()`). Each dispatch asks a `LaneSelector` for the lane and picks one ready element of that
lane with a seeded SplitMix64 (Steele, Lea and Flood, OOPSLA 2014, with Vigna's constants), so the seed
explores every within-lane order the pool may produce while priority and aging hold exactly. It is its own
`IClock`: time stands still while work is ready and jumps to the earliest timer when nothing is. Each
dispatch is folded into an FNV-1a trace hash; the same seed gives the same trace on every platform (a golden
fingerprint of 100 scenario traces is identical with MSVC STL on Windows and libstdc++ on Linux, and the 10k
seed run makes 1,362,189 dispatches on both).
Shutdown matches the pool. All calls must come from the owning thread (checked).

### Locking
All locked state uses `frost::Mutex` with `FROST_GUARDED_BY`: the injection queues, the idle-wait state, the
timer map, and `LatestWins`. Atomics are used only where the comment beside them explains the ordering.

### Tests
- `sched_tests` (unit, every configuration): 61 tests over the deque, the lane policy, tasks, the pool,
  SimExecutor and LatestWins.
- `sched_stress_tests` (label `stress`; skipped in Debug, run in Release, ASan and TSan): 10k SimExecutor
  seeds with an invariant and lane-policy oracle per seed and a determinism re-check on every 97th; the
  real-pool run (1M tasks per iteration across all lanes, a quarter of the batches cancelled halfway, lane
  hops, sleeps); and a long owner-plus-six-thieves deque run. The default is 20 pool iterations and 2M
  deque items; `FROST_STRESS_FULL=1` selects the exit scale of 100 iterations and 20M items. The seed test
  always runs all 10k seeds.
- `sched_perf_tests` (label `perf`): the exit-criterion dispatch test. It reports in every build and asserts
  the criterion only in optimised, non-sanitised builds.

## Measurements
REF-W1: AMD Ryzen 9 5900X (12 cores, 24 threads), 32 GB, Windows 11 Home 26300; WSL Ubuntu 24.04 on the same
machine with 24 threads and a 5.9 GB memory cap. Two other agents were building concurrently during every
run, so these numbers include that background load.

**Interactive dispatch** (`SchedPerf.InteractiveDispatchP99UnderBackgroundSaturation`). Default pool (23
workers, low-lane cap 20). 10,000 Background tasks are queued and kept at that level: each runs a 500 µs busy
slice and respawns itself. The test then measures, from two submitters, the time from
`spawn(Lane::Interactive, …)` to the first instruction of the task, with a random 100–600 µs pause between
samples. The submitters are an external thread (the task goes through the Interactive injection queue) and a
Background slice running on a worker (the task lands in that busy worker's own deque and must be stolen).
Saturation is sampled with every measurement: mean low-lane occupancy was 20.00 of 20 slots, and ≥ 19 slots
were busy with ≥ 9,900 queued in 100% of samples.

| Build | Submitter | p50 | p99 | max | samples |
|---|---|---|---|---|---|
| Windows Release (budgeted) | external | 9.5 µs | 35.7 µs | 542 µs | 2000 |
| Windows Release (budgeted) | from a worker | 9.4 µs | 46.2 µs | 980 µs | 2000 |
| Linux Release (budgeted) | external | 35.5 µs | 118.6 µs | 3594 µs | 2000 |
| Linux Release (budgeted) | from a worker | 36.7 µs | 99.2 µs | 4379 µs | 2000 |
| Windows Debug (report only) | external | 12.5 µs | 99.9 µs | 8842 µs | 300 |
| Windows Debug (report only) | from a worker | 12.3 µs | 133.8 µs | 9107 µs | 300 |
| Linux Debug (report only) | external | 35.9 µs | 124.6 µs | 945 µs | 300 |
| Linux Debug (report only) | from a worker | 36.8 µs | 150.5 µs | 3269 µs | 300 |

The criterion (p99 ≤ 1 ms) holds with a margin of about 20× on Windows and 8× in WSL. The higher WSL median
(~35 µs against ~10 µs) is probably the cost of waking a parked worker in the WSL 2 VM; it was not profiled.

**Stress at the exit scale** (`FROST_STRESS_FULL=1`).

| Build | 10k SimExecutor seeds | 100 × 1M pool tasks | 20M deque items | Total wall |
|---|---|---|---|---|
| Windows Release | 0.2 s, 1,362,189 dispatches, all pass | 24.5 s, 8,601,718 skipped, 220,752 cut short | 5.4 s | 30.1 s |
| Linux TSan (RelWithDebInfo) | 0.6 s, 1,362,189 dispatches, all pass | 144.6 s, 8,459,335 skipped, 317,258 cut short | 36.0 s | 181.7 s |

TSan reported nothing, and in both runs every invariant held: each leaf body ran at most once, exactly the
skipped leaves never ran, and results and cancellations add up.

**Stress at the default scale** (what `verify.sh --full` runs: 10k seeds, 20 × 1M pool tasks, 2M deque items):

| Build | seeds | pool | deque | Total wall |
|---|---|---|---|---|
| Linux TSan (RelWithDebInfo) | 0.6 s | 29.1 s | 3.5 s | 33.3 s |
| Linux ASan+UBSan (Debug) | 2.4 s | 15.7 s | 0.8 s | 19.3 s |
| Linux Release | 0.1 s | 4.7 s | 0.4 s | 5.2 s |

The TSan and ASan unit runs (56 tests at the time) took 0.6 s and 1.7 s.

## Consequences
- Every scheduling decision except within-lane order is shared code (`LaneSelector`), so SimExecutor
  seeds test the same policy the pool runs. The pool's own races (deque, parking, cap, timers, shutdown)
  are covered by the TSan stress runs instead, since the simulator has one dispatcher.
- Aging counts dispatches, so its latency bound depends on segment length. Low-lane code must keep segments
  short (suspend or `co_await schedule(lane)` between chunks of work); a long segment also holds a low-lane
  slot for its whole duration.
- Aging is per worker: the bound holds per dispatcher, not across the pool. No global starvation bound is
  claimed beyond "every worker that keeps dispatching serves the lane within maxWait".
- Shutdown drains, so a task that loops without checking any token blocks shutdown. This is deliberate:
  destroying half-run frames would leave awaiters dangling.
- Timers have Windows' default 15.6 ms timer resolution until they move to the reactor.
- Follow-up work:
  - TODO(M1): MainThreadDispatcher (src/ui) implementing `IScheduler::enqueue` for the UI thread.
  - Done (ADR 0020, `m1/io-reactor-watch`): the asio reactor completes I/O awaiters through `enqueue()`.
    TODO(M1): the pool's timers move to the reactor's timer queue, with high-resolution waits on Windows
    (sched cannot depend on io, so this needs a timer seam in sched; its owner is an open question, ADR 0020).
  - TODO(M1): helper-process priorities (BELOW_NORMAL + EcoQoS, SCHED_IDLE) in the io process launcher;
    worker threads keep normal OS priority.
  - TODO(M1): a Relacy-style model check of the deque (PLAN.md section 6.1).
  - When `tools/perf.ps1` exists, the dispatch test's `RecordProperty` values (`external_p99_us`,
    `worker_p99_us`) feed the perf history.

## Alternatives considered
- **std::execution**: banned by CLAUDE.md. **TBB**: rejected by PLAN.md; its arenas also give no lane cap.
- **A global mutex-protected priority queue**: simplest, but every dispatch of 23 workers would contend on
  one lock, and LIFO locality for continuations would be lost.
- **Workers partitioned by lane**: idle Background workers could not help an Interactive burst, and the cap
  would be static rather than a ceiling.
- **Wall-clock aging**: a clock read per dispatch, and not reproducible in the simulator.
- **The paper's exact relaxed slot orderings under TSan**: TSan reports false races (see above), and
  suppressing them would also hide real ones.
- **Epoch or hazard-pointer reclamation of old rings**: rings only grow, and rarely; keeping them until the
  deque is destroyed is simpler and bounded.
- **Shutdown that drops pending work**: suspended frames' owners are not all reachable, so dropping would
  leak or dangle; draining with the stop token requested is simpler to reason about.
- **Timers on the reactor now**: the reactor does not exist yet (src/io is a separate slice).
- **Exceptions propagating through `co_await`**: would carry exceptions across module boundaries.

## Deviations from PLAN.md
- The low-lane cap "cores−4" is additionally clamped to `workers − 1`, which only matters at 5 or fewer
  hardware threads, so a worker is always free for Interactive work. On REF-W1 it is 20, as planned.
- Aging is defined in dispatches, not time (see above).
- Timers run on a dedicated pool thread until the reactor exists.
- "TSan over 10k SimExecutor seeds" runs the seeds on 8 threads at once (independent simulators), so TSan
  also checks that simulators share no hidden state; the simulator itself is single-threaded by design.
