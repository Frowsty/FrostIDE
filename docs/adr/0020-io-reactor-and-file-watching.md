# ADR 0020: The io reactor, file watching, async child pipes and the gitignore-aware scan

- Status: accepted for `m1/io-reactor-watch`. No budget, test or warning is weakened; `tests/perf/budgets.json`
  is unchanged.
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 (the "File watching" row: ReadDirectoryChangesW on IOCP with the Ex variant
  where it works, rescan on overflow, inotify with limit detection and a polling fallback, a gitignore-aware
  parallel scan, 500k files), section 2.1 (start-up hygiene: no static initialisers), section 3.3 (threads: 1-2
  reactor threads for all pipes and sockets, a dedicated watcher thread), section 3.5 (the trust model: children
  only through IProcessLauncher), section 6.2 (IFileWatcher -> MemFs with a virtual clock); budgets B20, B21 and
  B23; ADRs 0002 (scheduler), 0003 (io skeleton) and 0011 (G3: English OS error text).

## Context
M0 left frost::io synchronous (ADR 0003): blocking file calls, blocking pipes, no watching. Every M1 feature after
this slice needs more. The File System view, settings reload and the SaveManager's external-change detection need
batched, coalesced change notifications for trees of up to 500k files that never block the UI and degrade instead
of failing when the OS runs out of watch resources. The git integration, clangd and build logs need child pipes
that stream with bounded memory and can be cancelled. The first open of a root (B23) needs a parallel directory
walk that applies `.gitignore` files without starting git. Constraints:
- Clang only, C++23 in C++26 mode, `Result<T>` errors, wide Win32 APIs, no exceptions across modules.
- PLAN.md 2.1: frost_io carries no static initialisers of its own; anything a dependency adds must be pinned.
- B21 (no stall over 50 ms): no destructor or `co_await` path may block a scheduler thread on the OS.
- Tests must be deterministic: a watcher double on SimExecutor's virtual clock (PLAN.md 6.2), fault injection.
- Clean room: the gitignore matcher is written from the rules as gitignore(5) describes them, never from git's
  or libgit2's sources, documentation text or test vectors; asio's BSL-1.0 headers may be read.

## Decision

### 1. asio, standalone and header-only, private to frost_io
- asio 1.32.0 (vcpkg port `asio`, BSL-1.0, licence check below), `ASIO_STANDALONE`, `ASIO_NO_DEPRECATED`,
  `ASIO_NO_DEFAULT_LINKED_LIBS`. It is a PRIVATE dependency of frost_io: no public header includes it, and among
  the sources only `reactor_impl.hpp` does (included first by the translation units listed in
  `tests/cmake/check_static_init.cmake`). Tests never include it.
- The static initialisers asio brings are pinned by the test `io_no_static_init` (section "Static initialisers"
  below lists them): only the asio translation units may have an initialiser section, and every function they
  call must match an expected pattern. Each is a service id, a thread-local call-stack key, an error category or
  Winsock's start-up object; none does I/O. `reactor.cpp` constructs asio's `winsock_init<>::manual` from a
  `.CRT$XCL` entry, which the CRT runs before any `.CRT$XCU` initialiser: asio's automatic start-ups then only
  move a counter, so `WSAStartup` never runs, and with ws2_32 delay-loaded ws2_32.dll never loads
  (`FileWatcherWindows.WinsockIsNeverLoaded`). This leans on asio::detail; it goes when asio offers a switch that
  disables its automatic Winsock initialisation (the test also checks the guard).

### 2. The Reactor
- One `asio::io_context` (IOCP on Windows, epoll on Linux) run by 1 or 2 threads (`ReactorOptions::threads`).
  The app may create a second Reactor for watchers: that is PLAN.md 3.3's dedicated watcher thread.
- Every object bound to it (watch states, the inotify instance, async processes) owns a shared state with its own
  strand; its OS handles are touched only there. Completions resume coroutines through `IScheduler::enqueue`, so
  no reactor thread runs user code. States are `shared_ptr`-owned, so a late completion never touches freed
  memory. SIGPIPE is blocked on reactor threads.
- A lifecycle mutex guards a `shutDown` flag and the registry. After `shutdown()` binds fail with Cancelled and
  posts from outside a strand are refused; `shutdown()` posts every state's own shutdown, lets the context drain
  (outstanding overlapped operations count until their cancellation completes) and joins the threads.
- Exit order (reactor.hpp): shut the reactor down, destroy the bound objects, shut the schedulers down, destroy
  the Reactor. ThreadPool refuses foreign enqueues once its shutdown begins, which forces this order.
- Callers that are caller-driven (SimExecutor) are refused by the real watcher and launcher (InvalidArgument):
  they cannot take wakeups from reactor threads. Their doubles (MemFsWatcher, ScriptedProcessLauncher) accept them.
- Not done here: moving ThreadPool's timers onto the reactor (ADR 0002's follow-up). sched cannot depend on io,
  so that needs a timer seam in sched; it stays TODO(M1) in thread_pool.hpp with its owner as question Q1.

### 3. IFileWatcher: batches, coalescing, debouncing, rename pairing
- `watch(root, options)` returns an `IDirectoryWatch` whose `next()` hands out `FileChangeBatch` values: the net
  changes since the previous batch, sorted, each path once, never empty. The header (file_watcher.hpp) states
  the application rules a consumer follows (lift renamed subtrees, delete removed ones, re-read the rest); the
  watcher keeps no mirror of the tree.
- One pure, single-threaded `ChangeCoalescer` per watch turns raw events into net entries: per-path start and
  end facts, identity changes, rename pairing by cookie (a MovedFrom not followed by its MovedTo is a removal),
  subtree subsumption, and degradation to Rescan where the net effect is not decidable. Its rules are in
  change_coalescer.hpp. A property test (10k seeds in the unit run, 200k more in four stress shards) applies
  the batches of random MemFs mutation sequences to a model consumer (`WatchMirror`) and requires it to end
  equal to the tree, with an identity model checking every reported rename.
- `WatchQueue` (shared by every backend and the double) debounces: a batch is released after `debounce` (50 ms)
  of quiet, or `maxDelay` (500 ms) after its first event in a storm. More than `maxPendingPaths` (65,536)
  pending paths collapse into Rescan(root). Exclusions drop events strictly below excluded directories, and names
  containing writeFileAtomic's ".frost-tmp-" marker are filtered unless `reportFrostTempFiles`.
- Resource limits never fail `watch()`: the watch switches to `WatchMode::Polling` and `status()` says why
  (`DegradeReason`, with the limit value), for the UI to show.

### 4. The Windows backend
- `ReadDirectoryChangesExW` (ReadDirectoryNotifyExtendedInformation, so every entry has its type) on a directory
  handle assigned to the reactor's IOCP, then plain `ReadDirectoryChangesW` when the Ex call is refused at call
  time (ReFS and some redirectors refuse it), then polling (Unsupported). Two buffers alternate, so a completed
  buffer is parsed after the next read is issued: 256 KiB on fixed drives, 64 KiB elsewhere (the SMB limit).
- Overflow (0 bytes or ERROR_NOTIFY_ENUM_DIR) is Rescan(root). Other errors retry after 100 ms; three in a row
  with the root intact degrade to polling (RepeatedErrors). Root identity (volume serial and file id) is
  re-checked after every completion and every 2 s, so a renamed or replaced root ends the watch with
  Removed(root), then NotFound.
- Names containing '~' may be 8.3 aliases and are expanded with GetLongPathNameW while the entry exists.

### 5. The Linux backend
- One inotify instance per RealFileWatcher, read on the reactor (epoll) and shared by its watches; one watch
  descriptor per directory, added before the directory is listed. Directories that appear later get a set-up
  task that adds their descriptors and reports Created for what it lists (events before the descriptor existed
  were never queued). A directory IN_MOVED_FROM whose IN_MOVED_TO has not arrived within 20 ms left the tree.
- Limit detection: ENOSPC or ENOMEM from `inotify_add_watch` (max_user_watches) and EMFILE or ENFILE from
  `inotify_init1` (max_user_instances, the descriptor limit) degrade to polling with the limit in
  `status().reasonValue`; mid-life the watch first reports Rescan(root). statfs of the root selects polling on
  9P (WSL's /mnt drives), NFS, SMB/CIFS and FUSE.

### 6. Polling, and the double
- Polling diffs periodic snapshots (at least `pollInterval`, 2 s by default) taken with scanTree (decision 9;
  `withStatus`, no `.gitignore`, the watch's exclusions as the descend filter). Directories that could not be
  listed keep their previous snapshot entries, so a permission flap is not reported as a mass removal. Polling
  does not pair renames.
- `MemFsWatcher` observes MemFs's mutation hook (MemFs reports inotify-like sequences) and releases batches on the
  scheduler's clock, so under SimExecutor every batch boundary is a function of virtual time. It shares
  WatchQueue and ChangeCoalescer with the real backends, and offers injected overflows, failures and degradation.

### 7. Async child pipes: IAsyncProcess through IProcessLauncher::launchAsync
- `launchAsync(spec, {reactor, scheduler})` validates exactly as `launch` does and starts the same child; its
  Stdio::Pipe streams are bound to the reactor (overlapped named pipes as `windows::stream_handle`, non-blocking
  pipes as `posix::stream_descriptor`). Every OS operation runs on the process's strand.
- Each output stream keeps one read in flight into a 64 KiB buffer whenever that buffer is empty; reads copy out
  of it. A child that writes faster than the consumer reads blocks on the full pipe: memory stays bounded. A
  cancelled read abandons only its own wait, never the OS read, so no byte is lost. A stop during a write cancels
  the OS write through an asio cancellation slot and then closes stdin (the byte count written is unknowable).
- Exit: Windows waits on a SYNCHRONIZE duplicate of the process handle (`windows::object_handle`). POSIX waits on
  a pidfd (readable on exit), or polls `waitid(WEXITED | WNOWAIT | WNOHANG)` on a 10 ms to 50 ms timer where
  pidfd_open is missing. asio registers descriptors edge-triggered and its `async_wait` is not speculative, so an
  exit that happened before the descriptor was armed would be missed: the state probes once after arming. The
  child is never reaped while the object lives (so the pid cannot be recycled under it), never with
  `waitpid(-1)`; after release it is reaped by pid once it has exited.
- `terminate()` kills the job (Windows) or the process group (POSIX) and reports exit code 137 to every
  waiter. Destruction never blocks (B21): it posts the release to the strand, which kills the tree with
  killOnClose and otherwise lets the child run (and reaps it later). `communicate()` drains both pipes and feeds
  stdin as concurrent subtasks with an optional deadline that terminates the child (5 s grace for the exit).
- `ScriptedProcessLauncher::launchAsync` is the double: scripted output arrives in chunks on the scheduler's
  clock (`chunkBytes`, `chunkInterval`), hung children wait for terminate, and every launch is recorded.

### 8. The gitignore matcher: IgnoreRules and IgnoreStack
- `IgnoreRules::parse(text, base, source)` reads one ignore file: a UTF-8 byte-order mark is dropped, lines end
  at LF with a CR before it removed, blank lines and lines starting with '#' are skipped, unescaped trailing
  spaces are trimmed, then '!' (negation), a trailing '/' (directories only) and anchoring (any other '/'; a
  leading one is removed) are taken off. Patterns that can never match (a trailing backslash, an unterminated
  bracket expression, an unknown `[:class:]`, an empty body) are kept as invalid rules and reported as
  diagnostics with their line.
- Matching is byte-wise (case folding is ASCII-only, `IgnoreOptions::caseInsensitive` for core.ignorecase). A
  pattern compiles to tokens (literal, `?`, bracket set, `*`, a directory globstar `**/`, a trailing `/**`) and is
  matched by simulating its NFA over a bit set of token positions: O(tokens x bytes) for any pattern, so a
  pathological pattern cannot make the scan exponential. Literal, suffix (`*.o`) and match-everything patterns
  take fast paths; the others first check their literal tail. Unanchored patterns match the last component.
- `IgnoreStack` is a persistent stack of rule files sharing frames (pushing is O(1) and never copies), the
  deepest frame deciding: within a file the last matching rule wins, and a file with no matching rule defers to
  the one above. `matchWithParents` adds the rule that nothing below an ignored directory can be re-included.
- Behaviour on the forms the rules leave open was settled by running git 2.56.0 as a black box (decision 10's
  oracle and probes): `***` between slashes is a globstar, a `**` not between slashes is a single `*`, a pattern
  ending in a backslash or with an unterminated `[` never matches, `[[x]` holds '[' and 'x', `[]x]` holds ']',
  and a CR before the LF is not part of the pattern.

### 9. scanTree and listDirectoryWithDiagnostics
- `scanTree(fs, scheduler, root, options)` (Budget: B23) keeps up to `parallelism` directory listings in flight as
  tasks on `options.lane`, joins them in start order, reads each directory's `.gitignore` (a regular file only)
  onto that directory's IgnoreStack and never descends ignored directories. `.git` is skipped by name; atomic-save
  temp files are skipped unless `skipFrostTempFiles` is false. Entries come back sorted by path, so the result
  does not depend on the schedule. Problems that do not end the walk are `ScanIssue`s: unlistable directories,
  names the platform cannot represent, unreadable `.gitignore` files and never-matching patterns.
- `IFileSystem::listDirectoryWithDiagnostics` returns the entries plus a `DirListingIssue` per name that
  `listDirectory` drops (Windows names that are not valid UTF-16) or lists as raw bytes (POSIX and MemFs names
  that are not valid UTF-8). This resolves ADR 0003's TODO(M1) on reporting such names.
- Async `IFileSystem` variants are not planned: epoll cannot make regular-file I/O asynchronous, and the scan's
  parallelism comes from scheduler tasks instead (ADR 0003 amended).

### 10. Smaller items
- The 8.3 working-directory fallback (ADR 0003's TODO(M1)): when Windows refuses a current directory longer than
  MAX_PATH (LongPathsEnabled = 0), the launcher retries once with GetShortPathNameW's form of it, without the
  verbatim prefix, when that is short enough; otherwise it reports InvalidArgument as before.
- English OS error text (ADR 0011, G3): POSIX errno text comes from `strerror_l` with a "C" locale made once by
  `newlocale`, so LC_MESSAGES (which Qt sets from the environment) no longer translates it; a test runs under a
  translated locale where one is installed.
- The tests' process helpers moved to `tests/io/process_env.hpp` (shared by the blocking, async and fault
  suites).

## Measurements
Machine: the reference Windows 11 x64 machine and WSL 2 (Ubuntu 24.04, ext4) on it; Debug builds unless noted;
the numbers are from the slice's own test logs (`[stress]` and `[perf]` lines), and are reports, not gates.

Watcher stress (`WatcherStress`, Windows Debug, NTFS, RDCW-Ex):

| Scenario | Time | Raw events (cumulative) | Rescans (cumulative) | Peak pending |
|---|---|---|---|---|
| Create 50k files, 500 directories of 100 | 30.95 s | 100,505 | 0 | 50,463 |
| Rename all 50k in place | 22.46 s | 200,508 | 1 | 65,520 |
| Delete the 500 directories | 5.91 s | 251,011 | 1 | 50,401 |
| 10k creates, 64 KiB buffer / 256 KiB buffer | 5.03 s / 4.95 s | | 0 / 0 | |
| 20 forced overflow rounds of 2,000 files | | | 20 (one per round) | |

An earlier run of the same build on a busier machine took 41.13 s, 32.72 s and 9.58 s for the three phases
with the same event and rescan counts. The times are dominated by the Debug test writing the files. The one rescan, in the rename phase, is the
`maxPendingPaths` collapse (the test's consumer drains only when a phase ends, and 50k renames leave 100k
pending paths), not a lost kernel event: the kernel buffer did not overflow at 10k creates with either buffer
size, and every phase ended with the consumer's mirror equal to the disk.

Async pipes (`AsyncPipeFaults`, the `[stream]` and `[cancel]` lines; io_test_child as the child):

| Measurement | Windows Debug | WSL Debug |
|---|---|---|
| Stream 256 MiB of stdout through 64 KiB reads | 1,899 ms (135 MiB/s), 4,096 reads | not run yet |
| Cancelled read to Cancelled, 200 rounds: p50 / p99 / max | 0.110 / 0.223 / 0.329 ms | not run yet |

Every streamed byte is checked against the child's pattern. Optimised builds stream 1 GiB in the same test.
The stress suite also floods both pipes while only stdout is read (the child must stall, then resume with
nothing lost once stderr is read) and cancels reads at random points (`ReadsCancelledAtRandomPointsLoseNoByte`).
The slice was finished in the feature-first phase (ADR 0031), so the stress label, the WSL column and these two
scenarios have not been run as a whole yet; the hardening phase runs them and fills in the numbers.

Gitignore matcher: the oracle suite (`GitOracle.*`, git 2.56.0) compares 92 paths (files and directories; 57
decided by a rule, 59 with core.ignorecase) of a corpus of 41 patterns in four ignore files against
`git check-ignore --no-index -v -n` (the deciding file and line, negations included), and the scan's files
against `git ls-files --others --exclude-standard` (23 files, 21 with core.ignorecase), on both platforms: no
difference. `IgnoreRules.PathologicalPatternsStayLinear` matches 40-star patterns (`*a*a...*c*b`) against
names of 20,000 bytes, which a backtracking matcher could not finish, in 0.26 s (Windows Debug). The fuzz
target `io_ignore_rules` compares every line's verdict with a separate memoised backtracking matcher; its
corpus replay runs in every test tree.

### Static initialisers
`io_no_static_init` (Windows COFF, Debug and Release; Linux ELF, Debug) prints the observed set every run.
The COFF checker also recognizes the exact optimized LLVM 23.1.4 x64 lowering of Asio 1.32.0's
already permitted `winsock_init_instance`. Release inlines its constructor: an atomic increment returns
the previous Winsock use count, and only a previous count of zero branches to `WSAStartup`; both paths
register the same Asio destructor through `atexit`. The earlier `.CRT$XCL` manual guard increments that
same counter and is never destroyed, so this cold branch cannot execute. Debug leaves these operations
inside the named Asio initializer. A data relocation in this code is not an additional initializer callee.

`tests/cmake/static_init_coff.py` recognizes this lowering only by its complete instruction bytes,
offsets and relocation sequence, scoped to the archive member and initializer function. It requires
exactly one `.CRT$XCU` pointer to the initializer, after exactly one reactor `.CRT$XCL` pointer to the
actual `startWinsockManually` function. The guard's complete optimized body must increment the identical
counter, store a zero result on its first use, and perform no other work. A changed branch, counter,
destructor, import, extra instruction or relocation, missing guard, or earlier initializer is not accepted.
Nonmatching bodies retain the existing callee inspection; recognizer errors fail the gate. The negative
fixtures in `tests/cmake/test_static_init_coff.py` cover those changes. This adds no general permission for
`WSAStartup`, `atexit`, or arbitrary data references, changes no production optimization flags, and retains
the separate `FileWatcherWindows.WinsockIsNeverLoaded` runtime assertion. A compiler or Asio update that
changes the lowering requires inspecting its new machine code before updating this closed signature.

Observed in Debug on both platforms:
- Windows: 17 initialiser callees, all in asio objects: 8 service ids (scheduler, win_iocp_io_context,
  null_reactor, strand_executor_service, strand_service, win_iocp_handle_service, win_object_handle_service and
  the steady_clock deadline_timer_service), 3 thread-local call stacks (TlsAlloc each), the 4 error categories,
  `winsock_init_instance` (inert behind the guard) and the guard itself, which only `reactor` carries in
  `.CRT$XCL`.
- Linux: 32 callees in async_process, async_process_posix, file_watcher_linux, polling_watcher, reactor and
  real_file_watcher: the same service ids (epoll_reactor and reactive_descriptor_service instead of the IOCP
  ones) and call stacks (pthread_key_create each), with their constructors, destructors and `__cxa_atexit`
  registrations.
None performs I/O or loads a library. Their start-up cost has not been measured yet (hardening phase).

### Licence check
`python tools/licensecheck.py`: "licensecheck: 9 package(s), 0 violation(s)" on Windows. asio's licence is
BSL-1.0, on the SPDX allowlist, so no exception entry is needed.

## Consequences
- Every later M1 slice can watch trees, stream tool output and scan a root without blocking a thread. The File
  System view owns one recursive watch per project root and fans batches out; settings and the SaveManager use
  that fan-out or cheap non-recursive watches (guidance in file_watcher.hpp).
- frost_io now has threads (the reactor's) and an exit order. Getting the order wrong is a contract violation
  the destructors check, not a hang.
- TODO dispositions in src/io (every TODO(M1) there is resolved or moved with a reason):
  - resolved: listing diagnostics (ADR 0003), async pipes (ADR 0003, process.hpp), the 8.3 working-directory
    fallback (ADR 0003), English strerror text (os_error.cpp), the reactor half of ADR 0002's follow-up
    (scheduler.hpp), async IFileSystem variants (not planned, ADR 0003 amended);
  - moved to the SaveManager slice (PLAN.md I2), as they change save semantics: refusing a read-only target on
    POSIX, and sweeping stale `.frost-tmp-*` files (scanTree and the watchers hide them meanwhile);
  - moved to the crash and logging slice (PLAN.md I3), since frost::core has no logging yet: logging the
    resolved launch (process.hpp, process_win.cpp, process_posix.cpp);
  - kept outside src/io: ThreadPool's timers on the reactor (thread_pool.hpp, question Q1).
- Follow-ups: the optional USN journal (PLAN.md 2) is not done (TODO(M2) if B23's trees measurement needs it);
  macOS FSEvents stays unsupported until a macOS test leg exists; polling does not pair renames.

## Alternatives considered
- **libuv (MIT).** A C event loop with its own threads, handles and callbacks; frost would wrap every handle twice
  (libuv's and the scheduler's) and its file watching (one watch per directory on Linux, no Ex variant on
  Windows) would still need the coalescer. asio's strands map directly onto per-object states.
- **Boost.Asio.** The same code with a Boost dependency; standalone asio has the same licence and no Boost.
- **Hand-written IOCP and epoll loops.** Less dependency, more code in the riskiest part (cancellation and
  shutdown races); asio's are widely used and BSL-1.0.
- **QFileSystemWatcher.** Qt outside src/ui is forbidden by the layering, and it reports per-path signals without
  batching, overflow handling or rename pairing (PLAN.md rejects it).
- **One watch per consumer with its own OS handle.** Simple, but 500k-file trees would duplicate inotify
  descriptors per consumer; one watch per root with fan-out, and shared inotify descriptors, were chosen.
- **Asking git (`git check-ignore`, `git ls-files`) at scan time.** A process per scan, git required, and no
  answer for folders that are not repositories. git is the tests' oracle only.
- **libgit2's or git's matcher.** Excluded by the clean-room rule (GPL); the matcher is written from the rules.
- **A backtracking glob matcher.** The obvious implementation is exponential on patterns like `*a*a*a*b`; the NFA
  simulation is linear in the path. The fuzz target keeps a memoised backtracking matcher as its reference only.
- **Async `IFileSystem` on the reactor.** Not possible for regular files on epoll, and IOCP file I/O would split
  the two platforms' semantics; scheduler tasks give the scan its parallelism.
