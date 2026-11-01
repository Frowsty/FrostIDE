# ADR 0009: Tracy version

- Status: accepted for the version: Tracy 0.14.1 (item 1). Using Tracy in FrostIDE at all (items 2–9) is
  **conditional on the user's approval** of an exception to the listener policy (PLAN.md 2.2 "Local listeners"
  and 5.2): Tracy's client is an unauthenticated TCP listener, and it can have neither a token nor an ACL. Until
  the user grants that exception, M1 adds nothing (no `vcpkg.json` entry, no `FROST_TRACY` option, no 5.2 row),
  and items 2–9 are a plan, not a decision. The question for the user also covers item 5 as it stands: granting
  the exception means accepting `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING`, so Tracy captures no call
  stacks, no samples and no context switches ("Keeping call stacks" under Alternatives). If the user declines,
  see "If the exception is declined" under Alternatives. This ADR is the dependency ADR for tracy (CLAUDE.md
  "Licences").
- Date: 2026-10-31; revised 2026-11-01 after independent verification (threat model, shutdown order, warnings,
  pinning, and the per-zone cost of the prescribed task zones), and again after the second verification
  (Tracy's symbol worker and DbgHelp, which add `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING` to item 5;
  the shutdown figures; the release SBOM; the override's precondition). The third verification confirmed it;
  its four low findings are fixed (one rounding slip, the scope of the DbgHelp measurement, the lock-1 runs, and
  what giving up call stacks costs).
- Plan reference: PLAN.md section 2 ("Self-diagnostics": Tracy in dev builds), 2.1 ("Dependencies": "tracy
  (pinned in M0)"; startup hygiene; DbgHelp delay-loaded), 2.2 ("Local listeners", "Privacy and network",
  freeze reports), 5.2, budgets B1, B5, B20 and B21, M0 decision "Tracy version: 0.13.1 or 0.14.1"

## Context
PLAN.md uses Tracy for interactive profiling in dev builds. ETW and Perfetto markers stay the budget
instruments. M0 has to choose between Tracy 0.13.1 (2025-12-11) and 0.14.1 (2026-08-22):
- **Port availability.** Our builtin-baseline (vcpkg 2026.10.16, `9eec002d`) resolves `tracy` to 0.14.1#1.
  0.13.1#1 is in vcpkg's version database (git tree `14b48412…`), but the shallow `E:\sdk\vcpkg` clone does not
  contain that tree.
- **Compilers.** Our code is built with clang-cl on STL 14.44 and with clang on libstdc++ 14. The warning flags
  are `/W4 /WX -Wconversion -Wshadow -Wthread-safety` on clang-cl and `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow -Wthread-safety -Werror` on clang, and PRODUCTION targets add `-Wglobal-constructors` and
  `-Wexit-time-destructors` (`cmake/FrostCompilerOptions.cmake`).
- **Startup hygiene and listeners (PLAN.md 2.1, 2.2, 5.2).** No network I/O before L1 and no listener before L0.
  Every listener must be hardened as in 5.2, and every 5.2 row has a token or an ACL; even the dev/test-only
  Automation endpoint is an ACL'd pipe or AF_UNIX socket, "never TCP". Every listener must appear in the
  enumeration test. Tracy's client is a TCP server with no authentication, and its protocol has no place for a
  token. So Tracy needs an exception that only the user can grant (see Status).
- **Coroutines.** `Task<T>` coroutines resume on whichever worker picks them up, so one task's slices run on
  different threads.
- **Crash handling.** Crashpad owns it (PLAN.md 2.2), so Tracy's crash handler must stay off.
- **DbgHelp in-process.** FrostIDE calls DbgHelp itself: `frost.exe` delay-loads it (PLAN.md 2.1), cpptrace
  is a dependency, and the stall watchdog's freeze reports take UI-thread stack samples (PLAN.md 2.2).
  DbgHelp is not thread-safe: upstream's manual requires an application that uses it alongside Tracy to share
  one lock with Tracy (`TRACY_DBGHELP_LOCK`, `manual/tracy.md:1894-1906`).
- **Protocol.** The profiler GUI or `tracy-capture` has to speak the client's protocol version.

### Threat model of the 0.14.1 client
Read from the 0.14.1 sources (`public/client/TracyProfiler.cpp`, line numbers below); no hostile-server test was
run.

**Who can connect.** With item 5's options the client listens on IPv4 127.0.0.1 only, on the first free port
from 8086 (20 tries), and only after `StartupProfiler` (measured on both OSes). It serves one connection at a
time and, in on-demand mode, accepts a new one after a disconnect. That leaves:
- every local process that can open a loopback TCP connection, whichever user or session it runs in: loopback
  TCP has no per-user access control, and the port is easy to find by scanning;
- under WSL mirrored networking, Windows-side processes too: the Windows `tracy-capture` connected to every WSL
  client over 127.0.0.1 in this spike. The reverse direction (WSL processes reaching a Windows client) uses the
  same shared loopback but was not tested;
- not remote hosts (no listener outside 127.0.0.1, and no UDP broadcast);
- not, as far as the code shows, web pages: the client drops any connection whose first 8 bytes are not the
  `TracyPrf` shibboleth (`TracyProfiler.cpp:2000-2002`), and a browser's HTTP or WebSocket request starts with a
  request line. This was not tested.

**What a connected process can make the client do.** The client trusts addresses and sizes that the server
sends (`Profiler::HandleServerQuery`, `TracyProfiler.cpp:3909-4018`):
- **Read any memory of the process.** `ServerQueryString`, `ServerQueryPlotName` and `ServerQueryFrameName`
  (and `ServerQueryFiberName` with fibers) pass the server's 64-bit address to `SendString`, which runs `strlen`
  on it and sends up to 64 KiB back (`:3921`, `:3952`, `:3960`; `TracyProfiler.hpp:937`; the 16-bit length
  check is an `assert`, compiled out in Release, `:3496`). `ServerQuerySourceLocation` dereferences the address
  as a source-location record (`:3949`, `:3589-3602`). In FrostIDE that memory holds open editor buffers (file
  contents), the IDE-bridge token of 5.2, passphrases passing through frost-askpass and the environment.
- **Corrupt memory.** `ServerQueryDataTransfer` allocates a size chosen by the server (`tracy_malloc(ptr + 11)`,
  `:3998`), and every `ServerQueryDataTransferPart` writes 12 more bytes with no bounds or null check
  (`:4001-4004`). That is a heap overflow under the server's control; treat it as possible code execution inside
  FrostIDE.
- **Crash the process:** any unmapped address in the queries above, or `ServerQueryParameter`, which calls the
  parameter callback even when none is registered (`:4422-4428`; the check is an `assert`).
- **Exhaust memory** through large `ServerQueryDataTransfer` allocations.
- **Hold FrostIDE's exit open.** `ShutdownProfiler` waits for the worker thread, and the worker keeps serving
  a connected server until that server ends the connection (item 6).
- **Map the address space:** normal operation sends real pointers (source locations, strings). Without item
  5's `TRACY_NO_CALLSTACK`, the symbol and call-stack-frame queries (`:3958`, `:3975`) also resolve any address
  the server names through DbgHelp or libbacktrace; with it, the client only acknowledges them (`:3694-3723`).
- **Receive everything instrumented:** zone names, zone text and values (task names and ids), messages, plots
  and thread names. With the port defaults, `StartupProfiler` also starts system tracing (context switches,
  sampling) when the process has the rights: on Windows only when elevated, which FrostIDE never is; on Linux
  depending on the kernel's perf and tracing permissions. In WSL (`perf_event_paranoid` 2) it did start its
  system-tracing thread with the earlier configurations (measured; what that thread captured was not
  checked). Item 5's `TRACY_NO_SYSTEM_TRACING` compiles system tracing out (`TracySysTrace.hpp:4-13`), so none
  of that reaches a server.

`TRACY_NO_CODE_TRANSFER` removes only the symbol-code query (`:3977-3980`) and the source-file `fopen` in
`HandleSourceCodeQuery` (`:4461-4462`). It does not stop any of the above. 0.13.1 trusts the same queries
(`ServerQueryString` and `ServerQueryDataTransferPart` read the same way there). The first version of this ADR
said that a connected process can read "no file contents"; that was wrong.

**Consequence.** While a Tracy-enabled FrostIDE runs with Tracy started, any local process (and, for a WSL
instance, any Windows-side process) can read and corrupt FrostIDE's memory. That is acceptable, if at all, only
for a developer's own dev build, started explicitly for one profiling run (item 3), and only with the user's
exception.

### Tracy's symbol worker
The first two versions of this ADR did not cover it. Read from the 0.14.1 sources; what is marked measured is in
Measurements, "Symbol worker and shutdown".
- **When it exists.** Unless `TRACY_NO_CALLSTACK` is defined, `TracyCallstack.h:4-9` turns call-stack support
  on for Windows desktop and Linux. The profiler then starts a symbol worker thread
  (`TracyProfiler.cpp:1686-1689`), whose first act is `InitCallstack` (`:3865-3867`). The worker runs while the
  rest of the IDE keeps running.
- **Windows.** The client imports `dbghelp.dll`, so it is loaded at process start (measured). `InitCallstack`
  (`TracyCallstack.cpp:825-862`) calls `DbgHelpInit` (`:646-698`), which:
  - appends the executable's directory to `_NT_SYMBOL_PATH` with `SetEnvironmentVariableA` (`:660-682`). That
    changes the process environment, which child processes inherit unless their launcher passes its own
    environment block (measured: unset before `StartupProfiler`, `;<exe dir>\` afterwards);
  - calls `SymInitialize(GetCurrentProcess(), NULL, TRUE)` (`:684-688`);
  - then, unless the environment has `TRACY_NO_DBGHELP_INIT_LOAD=1`, loads symbols for every kernel driver and
    every module loaded in the process (`CacheProcessDrivers`, `CacheProcessModules`, `:746-823`, `:847-857`).
    If the DbgHelp in use has SymSrv beside it and `_NT_SYMBOL_PATH` names a symbol server, that preload can
    download symbols over the network, past PLAN.md 2.2's rule that symbol servers stay off until enabled with
    consent (not tested; SymSrv was never loaded in these runs).

  None of these calls takes a lock that FrostIDE's own DbgHelp users could share, unless `TRACY_DBGHELP_LOCK`
  is defined (`:655-658`, `:839-861`). So the worker would call DbgHelp concurrently with cpptrace and the
  freeze-report sampler, which upstream says is unsafe (Context).
- **Linux.** `InitCallstack` (`:1333-1366`) creates a libbacktrace state and reads `/proc/kallsyms`: no DbgHelp
  and no environment change.
- **Shutdown.** Before it exits, Tracy's worker waits for the symbol worker (`TracyProfiler.cpp:2207-2210`), and
  the symbol worker exits only after `InitCallstack` returns. A `ShutdownProfiler` soon after
  `StartupProfiler` therefore waits for the rest of the symbol preload, which grows with the number of loaded
  modules (measured).
- **`TRACY_NO_CALLSTACK` removes all of it.** `TracyCallstack.cpp` compiles to nothing (`:16`), there is no
  symbol worker, and the probes import nothing from `dbghelp.dll` (measured). FrostIDE loses only call-stack
  capture, which no wrapper macro uses (item 8).
- **And needs `TRACY_NO_SYSTEM_TRACING` beside it.** With call stacks off but system tracing on, the server's
  external thread-name queries (`:3966-3970`, sent for context-switch data) go into the symbol queue
  (`:3725-3730`). Without the symbol worker nothing drains that queue: the worker's own drain is also under
  `TRACY_HAS_CALLSTACK` (`:2249-2257`, `:2279-2287`). The queue holds 8,192 entries (`:1541`), and a full queue
  makes `emplace` spin (`tracy_SPSCQueue.h:75-77`), which would stall Tracy's worker and with it
  `ShutdownProfiler`. Not tested: context-switch capture needs elevation on Windows here, and was not checked on
  Linux.

## Decision
1. **Tracy 0.14.1.** This is the version the 2026.10.16 baseline already resolves: port 0.14.1#1, port tree
   `8cbe9bf2`, source SHA-512 `53912d75…8ce5510b` (checked against the downloaded tarball).
2. **What M0 pins, and how M1 pins it.**
   - **M0** pins the version decision (this ADR) and the vcpkg builtin-baseline `9eec002d` (2026.10.16) that is
     already in `vcpkg.json`, which provides the port at 0.14.1#1. That is how this ADR reads PLAN.md's "tracy
     (pinned in M0)". Nothing in `vcpkg.json` names tracy yet, because tracy is not a dependency until M1, and
     only if the exception is granted. PLAN.md is unchanged: a clearer wording of its dependency entry is
     proposed to the user, not applied.
   - An `overrides` entry is possible today without the dependency, and frost-licensecheck accepts it (measured,
     see "Pinning check"). It changes nothing that vcpkg installs while tracy is not a dependency, so it would
     only record intent; it is added together with the dependency in M1 instead.
   - **M1**, with the dependency, adds:
     - `"overrides": [{ "name": "tracy", "version": "0.14.1", "port-version": 1 }]`. An override fixes both the
       upstream version and the port version, whatever the baseline says, so a baseline bump cannot move the
       port. 0.14.1#1 installs this way at the current baseline (measured). **Precondition:** vcpkg needs the
       override's port tree (`8cbe9bf2`) in the local vcpkg clone. The shallow `E:\sdk\vcpkg` clone has it only
       because it is the current baseline's tree. After a baseline bump that changes `ports/tracy`, a shallow
       clone of the new tag would lack it and the install would fail, as the 0.13.1 override fails today
       (Measurements; the bump itself was not tested). The pin then fails closed: it never installs another
       port. Whoever bumps the baseline either makes bootstrap fetch that tree (full history, or a fetch of the
       commit that has it) or moves the override, the EXACT version and this ADR to the new port version;
     - `find_package(Tracy 0.14.1 EXACT CONFIG REQUIRED)`. Tracy's package version file reports 0.14.1, and
       EXACT rejects any other upstream version (measured: a request for 0.13.1 fails configure against the 0.14.1
       install). EXACT does not see the port version: a 0.14.1#2 with other patches or options would pass it. The
       override covers that.
3. **Dev builds only, off unless explicitly enabled for a run.** These are the conditions of the exception.
   - Tracy is linked only when a `FROST_TRACY` CMake option is on. Only the dev presets may turn it on;
     configure fails if it is on in a release or packaging preset. Release and shipped builds contain no Tracy
     code (test in Consequences).
   - Even in a dev build, `StartupProfiler` runs only when the developer asks for it for that run, for example a
     `--tracy` flag. It is never persisted in settings and never inherited by child processes. M1 picks the
     switch.
   - The client is always built with item 5's options: on-demand, `TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`,
     `TRACY_NO_CODE_TRANSFER`, `TRACY_MANUAL_LIFETIME`, `TRACY_NO_CALLSTACK`, `TRACY_NO_SYSTEM_TRACING`, no crash
     handler.
   - Budget and perf runs (`frost-bench`, `verify -Perf`) never start it. This rule is new and awaits the
     user's confirmation too.
   - The developer starts it only on a machine without untrusted local processes or other users, and knows
     that, while it runs, a local process can read the IDE's memory (threat model).
4. **Manifest entry for M1.** M1 adds `{ "name": "tracy", "default-features": false, "features": ["on-demand"] }`,
   which is `tracy[core,on-demand]`, and only behind a manifest feature (for example `"tracy"`). This is
   required, not a recommendation: an unconditional dependency would be installed for every preset, and
   frost-licensecheck builds the SBOM from the installed trees, so every release SBOM would list Tracy and the
   release-build test (Consequences) would fail.
   - Only a dedicated Tracy dev configure preset per OS enables the feature (`VCPKG_MANIFEST_FEATURES`) and
     `FROST_TRACY`. It has its own `VCPKG_INSTALLED_DIR`, as the ASan preset already does, because a manifest
     install removes every package outside its plan and today's `win-clangcl` and `linux-clang` presets share
     one tree between their Debug and Release builds.
   - licensecheck's default scans every `build/vcpkg_installed*` tree, which would include the Tracy dev tree.
     The SBOM of a release or packaging build is therefore written with `--vcpkg-installed` naming only that
     preset's tree.
   - `default-features: false` drops `crash-handler` (the port turns that into `TRACY_NO_CRASH_HANDLER`), because
     Crashpad handles crashes.
   - `fibers` stays off (item 7).
   - `cli-tools` and `gui-tools` never go into the manifest (item 9).
5. **Client options come from a per-port block in each FrostIDE triplet** (`x64-windows-clangcl`,
   `x64-windows-clangcl-asan`, `x64-linux-clang`):
   ```cmake
   # ADR 0009: Tracy client options. Nothing runs before StartupProfiler; loopback only; no
   # UDP broadcast; no source or code transfer to a connected server; no call stacks (so no
   # symbol worker and no DbgHelp) and no system tracing.
   if(PORT STREQUAL "tracy")
       list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
           -DTRACY_MANUAL_LIFETIME=ON -DTRACY_ONLY_LOCALHOST=ON
           -DTRACY_NO_BROADCAST=ON -DTRACY_NO_CODE_TRANSFER=ON
           -DTRACY_NO_CALLSTACK=ON -DTRACY_NO_SYSTEM_TRACING=ON)
   endif()
   ```
   The port exports these as `INTERFACE_COMPILE_DEFINITIONS` of `Tracy::TracyClient` (measured on both OSes:
   `TRACY_ENABLE;TRACY_ON_DEMAND;TRACY_NO_CALLSTACK;TRACY_ONLY_LOCALHOST;TRACY_NO_BROADCAST;
   TRACY_NO_CODE_TRANSFER;TRACY_NO_SYSTEM_TRACING;TRACY_MANUAL_LIFETIME;TRACY_NO_CRASH_HANDLER`). On Windows it
   also exports `WINVER` and `_WIN32_WINNT` as 0x0A00, the same values that `FrostCompilerOptions.cmake` sets.
   The ASan triplet was not built with Tracy in this spike; M1 builds it. Names used below: **nf** is the
   configuration of the first two passes (the block without `TRACY_NO_CALLSTACK` and `TRACY_NO_SYSTEM_TRACING`),
   **nc** adds `TRACY_NO_CALLSTACK` only, and **ns** is the block above, the decision.
   - **Link-time check, and its limit.** Tracy 0.14 builds 8 options into the names of its entry points (for
     example `GetProfiler_CFG_E1_OD1_DI1_ML1_F0_DHT0_TF0_PH0`): `ENABLE`, `ON_DEMAND`, `DELAYED_INIT`,
     `MANUAL_LIFETIME`, `FIBERS`, `DISALLOW_HW_TIMER`, `TIMER_FALLBACK` and `PLATFORM_HEADER`
     (`TracyMangle.hpp:15-71`). A TU compiled with a different value for one of these fails to link; only a
     `TRACY_FIBERS` mismatch was tested (both OSes). The security options (`ONLY_LOCALHOST`, `NO_BROADCAST`,
     `NO_CODE_TRANSFER`, `NO_CALLSTACK`, `NO_SYSTEM_TRACING`, `NO_CRASH_HANDLER`) are not in the names: dropping
     them from a triplet goes unnoticed at link time. M1 therefore adds a configure check (Consequences).
   - **`MANUAL_LIFETIME`:** nothing of Tracy runs before `tracy::StartupProfiler()`. There is no thread and no
     socket until then (measured). Without it, the profiler is a static object whose threads and listener start
     before `main`.
   - **`ONLY_LOCALHOST`** binds IPv4 127.0.0.1 only, and **`NO_BROADCAST`** opens no UDP socket. Without them the
     listener is dual-stack on every interface (`:::8086`) and a UDP broadcast socket is open (measured on the port
     defaults, which lack all of item 5's options).
   - **`NO_CODE_TRANSFER`:** a connected server cannot make the client open source files or copy code bytes. It
     can still read memory (threat model).
   - **`NO_CALLSTACK`:** no symbol worker thread, no DbgHelp calls, no change to `_NT_SYMBOL_PATH`, no symbol
     preload ("Tracy's symbol worker"). Measured, with probes that call no DbgHelp themselves and always started
     with `_NT_SYMBOL_PATH` unset: one thread fewer while started on both OSes, no `dbghelp.dll` import and no
     load by Tracy, `_NT_SYMBOL_PATH` still unset after `StartupProfiler`, and a `ShutdownProfiler` that no
     longer depends on the loaded modules. The alternative, `TRACY_DBGHELP_LOCK`, is under Alternatives.
   - **`NO_SYSTEM_TRACING`:** no context-switch or sampling capture, and no external thread-name queries that
     nothing would answer once call stacks are off ("Tracy's symbol worker"). Measured: in WSL, where the
     other configurations ran a system-tracing thread, one thread fewer; it also removes the 5 clang-cl
     warnings left in Tracy's TU with **nc** (Measurements, "Warnings").
   - **`on-demand`:** zones are dropped until a server connects.
6. **Lifetime: start at most once, and shut down in a fixed order.**
   - `StartupProfiler` runs on a background thread after the first frame, never on the launch path. It takes p50
     215 ms (timer calibration sleeps 200 ms). For Tracy's own state, starting while other threads run is safe:
     `StartupProfiler` publishes the started flag only after the profiler is fully built
     (`TracyProfiler.cpp:1265-1272`). For the rest of the process it is safe only with item 5's
     `TRACY_NO_CALLSTACK`: with the port defaults, the symbol worker it starts calls DbgHelp concurrently with
     FrostIDE's own DbgHelp users and rewrites `_NT_SYMBOL_PATH` ("Tracy's symbol worker"). With item 5, Tracy
     itself neither loads `dbghelp.dll` nor changes `_NT_SYMBOL_PATH`. That was measured in a probe that uses
     no DbgHelp of its own and started with the variable unset. The source agrees for any starting state: the
     client's DbgHelp calls and its only `SetEnvironmentVariableA` call (`TracyCallstack.cpp:682`) are all in
     `TracyCallstack.cpp`, which `TRACY_NO_CALLSTACK` compiles out. In FrostIDE, `dbghelp.dll` is still
     loaded by FrostIDE's own users (the delay load in `frost.exe`, cpptrace, the freeze-report sampler;
     Context), and `_NT_SYMBOL_PATH` keeps whatever value the developer set; Tracy changes neither.
     `StartupProfiler` still loads further system DLLs (9 more modules on Windows, measured; which ones was
     not recorded).
   - **At most once per process.** Each thread keeps its own producer token in a `thread_local` built on first
     use, with a raw pointer into the profiler's queue (`ProducerWrapper`, `:1239-1244`; `:1370`).
     `StartupProfiler` refreshes only the calling thread's token (`:1270`), and `ShutdownProfiler` resets the
     allocator's per-thread flag only on the calling thread (`:1289`). After a shutdown and a second start, every
     other thread that used Tracy before writes through stale pointers. The wrapper's start function refuses a
     second start.
   - **The hazard at shutdown.** `ShutdownProfiler` (`:1278-1291`) clears the started flag and then, with no
     synchronisation, destroys and frees the profiler and its queue and finalizes Tracy's allocator. A zone
     whose constructor saw `TracyIsStarted` true calls `GetProfiler()` again in its destructor
     (`TracyScoped.hpp:78-86`), which then dereferences a freed or null profiler; the same holds for a value,
     name, plot or message call that passed the guard just before shutdown. The guard is check-then-use, so no
     wrapper can make concurrent shutdown safe without a per-zone reference count. Upstream says the same:
     "Using any instrumentation … after shutdown is an error", and "if the profiler has been initialized, it must
     be shut down before the program exits" (manual, `tracy.md:516`; NEWS 0.14.1: "Manual lifetime management
     requires shutdown of the profiler").
   - **Required shutdown order** (M1 implements it in the exit path):
     1. FrostIDE has saved what exit must save (session, hot-exit journal) before any Tracy step, because step 4
        can be slow.
     2. Every thread that can emit Tracy events is stopped and joined: every `frost::sched` worker (so every
        `Task<T>` resume), IO and watcher threads, the stall watchdog and any other instrumented thread. The
        thread that will call `ShutdownProfiler` emits nothing more.
     3. If `TRACY_NO_EXIT=1` was in the environment, Tracy was never started: the start function refuses it,
        because that variable makes the worker wait for a server connection before it can exit
        (`TracyProfiler.cpp:1577-1583`, `:1948-1949`).
     4. `ShutdownProfiler` runs under the exit watchdog. With a server connected it waits until the server ends
        the connection: the worker sends its remaining data and a terminate notice, then keeps answering server
        queries (`:2205-2300`), and `~Profiler` joins it with an infinite wait (`TracyThread.hpp:50`, `:78`).
        When the watchdog's deadline passes (M1 picks it), the process ends without the rest of the shutdown;
        the state is already saved.
   - Unconnected, `ShutdownProfiler` alone takes tens of milliseconds. How many depends on the configuration,
     the time since `StartupProfiler`, the loaded modules and the machine's load (Measurements, "Symbol worker
     and shutdown"). With item 5 (**ns**) it took p50 37.5–62.5 ms on Windows (range 32.3–64.2 ms) and
     20.6–39.8 ms in WSL (range 20.5–40.5 ms) at 5.5% system CPU, with or without Qt loaded. Without
     `TRACY_NO_CALLSTACK` (**nf**, the earlier passes) a shutdown soon after start also waits for the symbol
     preload: p50 151.1 ms on Windows with Qt's three DLLs loaded. The earlier Windows figures (p50 157.5 and
     184.6 ms in the first pass under 43 GB of memory pressure, 163.0–169.4 ms in the rework pass at 36.6% CPU)
     were **nf** shutdowns about 30 ms after start; the same probe took 110.0–113.7 ms at 5.5% CPU. B20 is
     unaffected only because budget runs never start Tracy (item 3).
7. **No `TRACY_FIBERS`. `Task<T>` is instrumented one zone per resume slice.**
   - With fibers, every zone event takes the profiler's global serial-queue lock. On Windows that cost 6.7x per
     zone with four threads and 1.1x with one (measured). The WSL figures (22x and 188x) are not used for the
     decision, because the WSL clients were captured by the Windows `tracy-capture` across the VM boundary.
   - Without fibers, a zone cannot stay open across a `co_await`. Instead, the scheduler opens one zone around
     each `resume()`: a static source location guarded by `TracyIsStarted`, the task id as the zone value, and
     the task's name as the zone name. Its cost was measured separately (Measurements, "Task zones").
   - Fibers are part of the configuration in the mangled names, so they cannot be enabled per module. A developer
     who needs fiber timelines builds a separate tree with `tracy[fibers]`.
8. **FrostIDE code never calls Tracy macros directly.** Under `MANUAL_LIFETIME`, every macro dereferences a null
   profiler until `StartupProfiler` has run. An unguarded `FrameMark` and a `TracyLockable` built before startup
   both crash (measured: 0xC0000005 on Windows, SIGSEGV on Linux). `TracyIsConnected` dereferences it too.
   - M1 adds one wrapper header. Its zone macro is `ZoneNamedN(var, name, TracyIsStarted)`; its value and name
     helpers act on that zone; its frame-mark, plot, message and connected-check helpers test `TracyIsStarted`
     first.
   - The wrapper compiles to nothing without `FROST_TRACY`.
   - `TracyLockable` is not used for any lock that can exist before startup, so `frost::core::Mutex` does not
     embed it.
   - **Warnings.** The wrapper and every TU that uses it see Tracy's headers only as system headers, as CMake
     already emits them for the imported target (`-imsvc` on clang-cl, `-isystem` on clang; the package exports
     `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`). M1 never adds Tracy's include directory as an ordinary include,
     never turns `SYSTEM` off for the target, and never lowers FrostIDE's warning flags or wraps Tracy in
     warning pragmas. Tracy's own TU is compiled by vcpkg with the port's flags, outside FrostIDE's warning set
     (Measurements, "Warnings").
9. **The server side is 0.14.1 too.** That means the profiler GUI and `tracy-capture` built from
   `tracy[gui-tools]` or `tracy[cli-tools]` 0.14.1 in a separate install root outside the repository, or
   upstream's 0.14.1 release binaries. The tools are never linked into or shipped with FrostIDE.

### 0.13.1 compared with 0.14.1
| Criterion | 0.13.1 (port 0.13.1#1) | 0.14.1 (port 0.14.1#1, in our baseline) |
|---|---|---|
| Resolves at our baseline | No. It needs an `overrides` entry, and the manifest install then fails because the shallow clone lacks the port tree. | Yes. An explicit override to 0.14.1#1 also installs (13 s). |
| Protocol | `ProtocolVersion` 76, `BroadcastVersion` 3. The 0.14.1 `tracy-capture` refuses it: "uses incompatible protocol version". | 82 / 3. Captured by the vcpkg 0.14.1 tools in every run. |
| clang-cl, STL 14.44 | The port build fails at clang-cl's default C++14: `copying variable of type 'std::atomic<Thread *>' invokes deleted constructor` (TracyProfiler.cpp:936). It builds with `CMAKE_CXX_STANDARD=17`. | Builds. Tracy's own TU has 7 warnings at the port's flags with **nf**, 5 with **nc** and 0 with **ns** (none are errors there). Our consumers build warning-free with /W4 /WX and the PRODUCTION flags. |
| clang, libstdc++ 14 | The port builds (the default is gnu++17). | Builds with 0 warnings at the port's flags (no `-Wall`). Our consumers build warning-free with -Werror and the PRODUCTION flags. |
| Mismatched client defines | Not detected | Link error for the 8 options in the entry-point names (tested: `TRACY_FIBERS`); not for the security options |
| Manual lifetime | Also needs `TRACY_DELAYED_INIT` (`#error` otherwise) | `TRACY_MANUAL_LIFETIME` alone |
| `TRACY_NO_CODE_TRANSFER` | Does not stop source transfer: `ServerQuerySourceCode` is outside the guard, so a connected server can read any file that is older than the executable and smaller than 256 KiB | Fixed in 0.14.0. `HandleSourceCodeQuery` is guarded. Neither version stops memory reads (threat model). |
| Zones, frame marks, plots, messages, locks | Yes | Yes. Every capture had exact zone, message and plot counts. Frame marks and a lockable mutex were exercised but not counted, because csvexport cannot export locks. |
| Fibers (for `Task<T>`) | Yes | Yes. 0.14.1 attributes memory events to fibers (NEWS). |
| On-demand mode | Yes | Yes. 0.14.1 fixes section setup in on-demand mode. |
| No listener until enabled | `MANUAL_LIFETIME` + `DELAYED_INIT`, `ONLY_LOCALHOST`, `NO_BROADCAST` | `MANUAL_LIFETIME`, `ONLY_LOCALHOST`, `NO_BROADCAST` (measured) |
| Licence | BSD-3-Clause | BSD-3-Clause. Same text, only the copyright year differs. It is on the allowlist, and licensecheck reports 0 violations. |
| Port features | `crash-handler` (default), `on-demand`, `fibers`, `verbose`, `cli-tools`, `gui-tools` | The same set. `cli-tools` adds `capture-daemon` and `merge`. |
| Port patches | `build-tools`, `fix-vendor-versions`, `fix-imgui-patch`, `downgrade-capstone-5` | `fix-debug-install-dirs` (install paths only), `build-tools`, `fix-vendor-versions`, `downgrade-capstone-5`, `imgui-loader`. None touches the client sources under `public/`. The port passes `-DTRACY_ENABLE=ON` (upstream's default is OFF since 0.14.0). |

## Measurements
**Machine.** REF-W1: AMD Ryzen 9 5900X (12 cores, 24 threads), 32 GB, Windows 11 build 26300 with a hypervisor
present. WSL2 Ubuntu 24.04.5, kernel 6.18.54.1, a 6 GB VM with 24 vCPUs, ext4.

**Toolchain.** LLVM 23.1.4: clang-cl with STL 14.44.35207 and SDK 10.0.26100.0, and clang++ with libstdc++
14.2.0. CMake 4.4.5, Ninja 1.13.2, vcpkg tool 2026-09-26 with checkout 2026.10.16.

**Method.**
- vcpkg ran in classic mode outside the repository with `VCPKG_MAX_CONCURRENCY=4` and `--binarysource=clear`,
  using the repository triplets plus that pass's item 5 block (**nf** in the first two passes; **nc** and
  **ns** in the second fix round). Build trees were in the session scratchpad on Windows and in
  `~/frost-tracy-spike` (`~/frost-tracy-spike2` for the second fix round) on ext4 in WSL.
- Consumers were built with the repository toolchain files and `FrostCompilerOptions.cmake`. Timings come from
  Release builds (/O2, -O3).
- In the first two passes, "default" means `tracy[core,on-demand]` with the **nf** options, and "fibers" adds
  `fibers`.

**Load.** First pass (2026-10-31): `lghub_agent` held 43.3 GB of private memory, and other agents were building.
Commit headroom was 1.4–3.4 GB during the Windows runs. Treat the numbers as upper bounds. The rework pass
(2026-11-01, "Task zones") and the second fix round ("Symbol worker and shutdown") ran under the measurement
lock with the conditions quoted there.

| Build proof | Windows (clang-cl) | WSL (clang + libstdc++ 14) |
|---|---|---|
| `vcpkg install tracy[core,on-demand]` | 15 s (17.4 s wall); Tracy's TU: 7 warnings in Debug and 7 in Release | 9.9 s (15.3 s wall); 0 warnings in Debug and Release |
| `vcpkg install tracy[core,on-demand,fibers]` | 14 s (16.2 s wall); 7 + 7 warnings | 11 s (17.6 s wall); 0 + 0 warnings |
| `vcpkg install tracy[core,on-demand]`, **nc** / **ns** (second fix round) | 8.1 s (9.4 s wall), 5 + 5 warnings / 8.6 s (10.0 s wall), 0 + 0 warnings | 7.3 s (13.0 s wall) / 6.9 s (12.9 s wall); 0 + 0 warnings each |
| Consumers (smoke, probe, task probe, symbol probe; Debug and Release) against **nf**, **nc**, **ns** (second fix round) | All build, 0 warnings | All build, 0 warnings |
| Smoke client: one zone and one `FrameMark` per frame, 100 frames, PRODUCTION flags; size Debug / Release | default 699,904 / 266,240 B; fibers 701,952 / 257,024 B; runs OK | default 2,394,320 / 556,896 B; fibers 2,286,048 / 507,824 B; runs OK |
| One TU with `TRACY_FIBERS` undefined against the fibers library | Link error, as designed | Link error, as designed |
| 0.13.1#1 port through an overlay | Fails (C++14 default) | Builds |
| Manifest mode with an override to 0.14.1#1 / 0.13.1#1 | Installs / fails (shallow clone) | n/a |
| `licensecheck --vcpkg-installed` | 13 packages, 0 violations: tracy BSD-3-Clause; the cli-tools dependencies are capstone BSD-3-Clause, zstd `(BSD-3-Clause OR GPL-2.0-only)`, nlohmann-json MIT, ppqsort MIT | 7 packages, 0 violations: tracy BSD-3-Clause, pthreads Apache-2.0 |
| `tracy[cli-tools]` (capture, csvexport) | 3.4 min at one job. The first try at four jobs died with lld-link "out of memory" while other builds were running; the commit headroom at that moment was not logged. | Not built. WSL clients were captured from Windows over mirrored loopback. |

### Warnings
**Tracy's own TU.** The port compiles the whole client as one unity TU, `public/TracyClient.cpp`, which includes
every client `.cpp`. vcpkg builds it with the port's flags, not FrostIDE's: clang-cl's default warnings (no `/W`
level) on Windows, and no warning flags at all on Linux. Counts per build log (`N warnings generated`):

| 0.14.1 client build | Windows Debug | Windows Release | WSL Debug | WSL Release |
|---|---|---|---|---|
| `tracy[core,on-demand]`, **nf** | 7 | 7 | 0 | 0 |
| `tracy[core,on-demand,fibers]`, **nf** | 7 | 7 | 0 | 0 |
| Manifest mode, override to 0.14.1#1, port defaults | 7 | 7 | not built | not built |
| `tracy[cli-tools]` tree, client TU, port defaults | 7 | 7 (in a log shared with the tools) | not built | not built |
| `tracy[core,on-demand]`, **nc** (second fix round) | 5 | 5 | 0 | 0 |
| `tracy[core,on-demand]`, **ns**, the decision (second fix round) | 0 | 0 | 0 | 0 |

**nc** loses the two `TracyCallstack.cpp` warnings, because `TRACY_NO_CALLSTACK` compiles that file out, and
**ns** also loses the five in `TracyETW.cpp`, which `TracySysTrace.cpp` includes only with system tracing
(`TracySysTrace.cpp:7`, `:65`). The 7 Windows warnings of the other builds are the same in every build:
```text
public/client/TracyCallstack.cpp(632,110): warning: format specifies type 'unsigned int' but the argument has type 'DWORD' (aka 'unsigned long') [-Wformat]
public/client/TracyCallstack.cpp(632,116): warning: format specifies type 'unsigned int' but the argument has type 'DWORD' (aka 'unsigned long') [-Wformat]
public/client/windows/TracyETW.cpp(158,82): warning: format specifies type 'unsigned int' but the argument has type 'ULONG' (aka 'unsigned long') [-Wformat]
public/client/windows/TracyETW.cpp(158,90): warning: format specifies type 'unsigned int' but the argument has type 'ULONG' (aka 'unsigned long') [-Wformat]
public/client/windows/TracyETW.cpp(179,37): warning: expression result unused [-Wunused-value]
public/client/windows/TracyETW.cpp(276,148): warning: format specifies type 'unsigned int' but the argument has type 'unsigned long' [-Wformat]
public/client/windows/TracyETW.cpp(568,100): warning: format specifies type 'unsigned int' but the argument has type 'ULONG' (aka 'unsigned long') [-Wformat]
```
They are format-width mismatches on 32-bit `unsigned long` (harmless on x64 Windows, where it is the same width
as `unsigned int`) and one unused value. They stay inside the vcpkg build and never meet `/WX`. The tools build
adds its own warnings in the server and tool TUs (dev tools only, never linked). The first version of this ADR
reported 0 warnings on Windows; that came from the top-level vcpkg log, which does not echo compiler output.

**What our code sees.** Two consumers were compiled with the exact PRODUCTION command lines of the consumer
build (Debug and Release, no-fibers **nf** install, then the **nc** and **ns** installs in the second fix round;
flags as in Context): `smoke.cpp`, the 25-line smoke client (one
zone and one `FrameMark` per frame), and `surface.cpp`, which uses every macro the M1 wrapper would expand to (a
guarded `ZoneNamedN` with `ZoneValueV`, `ZoneNameV` and `ZoneTextV`, a nested guarded zone, `FrameMark`,
`FrameMarkNamed`, `TracyPlot` with `int64_t` and `double`, `TracyMessageL`, `TracyMessage`, `TracyIsConnected`).
Each was compiled twice: with Tracy's include directory as CMake emits it for `Tracy::TracyClient`, and as an
ordinary `-I` include. A second pass with errors turned back into warnings and no error limit listed every
diagnostic.

| Tracy include | Windows clang-cl | WSL clang |
|---|---|---|
| System, as CMake emits it (`-imsvc`, `-isystem`); **nf**, **nc** and **ns** | 0 warnings; both sources build in both configurations | 0 warnings; both build |
| Ordinary `-I`, **nf** | 3 warnings in Tracy's headers; `/WX` fails the build | 1 warning in Tracy's headers; `-Werror` fails the build |
| Ordinary `-I`, **nc** and **ns** | 1 warning in Tracy's headers; `/WX` fails the build | 1 warning; `-Werror` fails the build |

The warnings with an ordinary include, the same for both sources and both configurations, all from our
`-Wconversion` (paths under `include/tracy/`):
```text
client/TracyCallstack.hpp(110,48): warning: implicit conversion changes signedness: 'int' to '__size_t' (aka 'unsigned long long') [-Wsign-conversion]
client/TracyCallstack.hpp(111,73): warning: implicit conversion changes signedness: 'int32_t' (aka 'int') to 'unsigned long' [-Wsign-conversion]
common/TracyQueue.hpp(438,39): warning: implicit conversion loses integer precision: 'int' to 'uint8_t' (aka 'unsigned char') [-Wimplicit-int-conversion]
```
The first two are in a Windows-only call-stack branch, which `TRACY_NO_CALLSTACK` removes; the third appears
on both OSes and in every configuration. No diagnostic points into our
sources in either mode, so the macro expansions themselves are clean. Tracy's headers are clean under our flags
only because they are system headers: that is why item 8 keeps them so, and why the frost-lint rule keeps the
wrapper the only place that includes them.

### Sockets and threads
| Sockets and threads (probe: 3 s per phase) | Windows | WSL |
|---|---|---|
| Port defaults (no item 5 options, crash handler on), every phase from before `main` | 7 threads; TCP `:::8086`; UDP `0.0.0.0:55380` | not measured |
| **nf**, before `StartupProfiler` | 4 threads; no sockets | 1 thread; no sockets |
| **nf**, after `StartupProfiler` | 7 threads; TCP `127.0.0.1:8086`; no UDP | 5 threads; TCP `127.0.0.1:8086`; no UDP |
| **nf**, after `ShutdownProfiler` | 4 threads; no sockets | 1 thread; no sockets |
| **nc** and **ns** (second fix round), before / after `StartupProfiler` / after `ShutdownProfiler` | 4 / 6 / 4 threads; TCP `127.0.0.1:8086` only while started; no UDP | **nc** 1 / 4 / 1, **ns** 1 / 3 / 1 threads; TCP `127.0.0.1:8086` only while started; no UDP |

The "port defaults" row was a `tracy[cli-tools]` client, which lacks every item 5 option, not only
`MANUAL_LIFETIME`: the all-interface listener and the UDP socket come from the missing `ONLY_LOCALHOST` and
`NO_BROADCAST`, and only "from before `main`" comes from the default lifetime. The Windows **nf** rows were first
measured on the fibers build; a re-run on the no-fibers build (2026-11-01) gave the same result in every phase.
In WSL both builds were measured and agree. In the second fix round all three configurations were measured
again in one session (Windows also checked whether `dbghelp.dll` was loaded: with **nf** in every phase, from
process start; with **nc** and **ns** never); **nf** gave the rows above again on both OSes. The thread that
**nc** lacks is the symbol worker; the one **ns** also lacks in WSL is the system-tracing thread.

### Per-zone cost (first pass)
Unconnected cost: 10 runs per cell, 10M zones per run on one thread, p50 (min–max). "Not started" is a zone
guarded by `TracyIsStarted` while it is false, and the loop is included, so it is an upper bound. "Started, no
server" is an unguarded `ZoneScopedN` with a static name.

| Case | Windows default | Windows fibers | WSL default | WSL fibers |
|---|---|---|---|---|
| Not started, ns per zone | 1.67 (1.58–1.71) | 2.01 (1.82–2.04) | 1.44 (1.36–1.61) | 1.17 (1.13–1.23) |
| Started, no server, ns per zone | 2.99 (2.83–3.11) | 3.09 (2.81–3.58) | 2.98 (2.81–3.35) | 2.37 (2.31–2.80) |
| `StartupProfiler`, ms | 218.4 (210.7–222.7) | 215.7 (211.8–219.2) | 216.3 (214.0–241.5) | 215.3 (214.3–229.9) |
| `ShutdownProfiler`, ms | 157.5 (151.3–167.7) | 184.6 (158.7–255.4) | 44.5 (42.5–52.5) | 50.3 (45.9–52.8) |

Connected cost: 5 runs per cell, 250,000 unguarded `ZoneScopedN` zones per thread, mean ns per zone over the
threads, p50 (min–max). `tracy-capture` 0.14.1 ran on Windows; WSL clients were reached across the VM boundary
over mirrored loopback, so the WSL rows also measure that path.

| Client | Threads | default | fibers | fibers / default |
|---|---|---|---|---|
| Windows | 1 | 31.9 (30.2–32.5) | 35.7 (33.6–39.8) | 1.1x |
| Windows | 4 | 36.0 (34.2–39.4) | 240.8 (210.2–252.5) | 6.7x |
| WSL | 1 | 50.3 (48.4–76.9) | 1,094 (1,084–1,137) | 22x |
| WSL | 4 | 78.9 (70.6–94.1) | 14,857 (14,603–14,976) | 188x |

- **Capture contents.** Every capture reported the exact zone count: the benchmark zones plus 100 `frame.work`
  zones, plus, with fibers, 100 cross-thread `task.span` fiber zones. All 42 saved traces, from Windows and WSL
  clients, held 100 messages and 100 `probe.plot` points with values 0..99 (`csvexport -m` and `-u -p`).
- **Zone-count check.** With 50 dependent multiplies inside each zone (4 threads × 100,000 zones, 1 run each),
  csvexport's statistics match exactly: 400,000 benchmark zones and 100 `frame.work`, and with fibers 100
  `task.span`. The cost was 61.5 ns per zone for default and 602.2 ns per zone for fibers.
- **Why csvexport shows fewer empty zones.** Without the spin, csvexport counts 0–10% fewer zones than were
  captured, because the server's statistics skip zero-length zones: 2.4–2.9% (Windows default), 1.3–1.7%
  (Windows fibers), 7.6–9.7% (WSL default) and 0% (WSL fibers). Zone durations cluster at about 10 ns steps on
  this machine, with some 1, 9 and 19 ns values (duration histogram in the raw logs).
- **Why fibers cost so much.** The fibers cost is the serial queue. `TRACY_FIBERS` sends every zone event
  through `Profiler::QueueSerial()`, which takes `m_serialLock` (`std::shared_mutex` under `_MSC_VER`, otherwise
  `std::mutex`). The sender thread also polls that lock with `try_lock`. Windows shows little cost at one thread
  (1.1x) and a large one at four (6.7x), which points at contention. Why WSL is so much slower even at one
  thread (22x) was not investigated; the WSL client's sender talks to a server on the Windows side of the VM
  boundary, which the Windows client does not, so the WSL ratios are not a property of fibers alone.
- The time the probe waited for the server to connect is not reported: it comes from the harness, which starts
  `tracy-capture` 500 ms (Windows) or 1500 ms (WSL) after the probe.

### Task zones (rework pass)
The first pass measured unguarded `ZoneScopedN` zones with static names, not the zone item 7 prescribes. A
second probe, `tracy_taskprobe` (no-fibers install, TEST flags, Release), times three variants of that zone, each
with a static source location: **guard** is `ZoneNamedN(z, "sched.resume", TracyIsStarted)`; **value** adds
`ZoneValueV(z, taskId)`; **name** adds `ZoneNameV(z, taskName, 17)` as well, which is the prescribed zone. The
first probe ran again in the same session as a baseline. ns per zone, p50 (min–max): 10 runs per unconnected
cell (10M zones on one thread, loop included), 5 runs per connected cell (250,000 zones per thread, mean over the
threads, `tracy-capture` 0.14.1 on Windows, `TRACY_PORT` 18096). Connected runs were taken on Windows only,
because WSL clients can only be captured across the VM boundary here.

| Zone | Windows, not started | Windows, started, no server | Windows, connected, 1 thread | Windows, connected, 4 threads | WSL, not started | WSL, started, no server |
|---|---|---|---|---|---|---|
| Baseline: first probe (guarded when not started, unguarded otherwise) | 1.74 (1.67–1.85) | 3.11 (3.01–3.29) | 33.7 (32.2–34.4) | 38.1 (37.3–40.0) | 1.51 (1.45–1.59) | 3.36 (3.11–3.94) |
| guard | 1.81 (1.65–1.90) | 4.10 (3.95–4.23) | 32.8 (31.7–34.1) | 39.3 (37.6–41.0) | 1.23 (1.19–1.37) | 3.03 (2.97–3.23) |
| value | 1.78 (1.68–1.94) | 3.78 (3.51–3.85) | 42.2 (39.8–46.3) | 51.2 (48.1–52.0) | 1.22 (1.16–1.33) | 3.03 (2.89–3.06) |
| name (prescribed) | 1.83 (1.69–1.97) | 3.75 (3.59–3.87) | 59.5 (57.7–70.4) | 75.8 (73.6–82.8) | 1.50 (1.40–1.63) | 3.43 (3.16–3.87) |

- Every connected capture of the task probe held exactly 250,000 or 1,000,000 zones (the baseline adds its 100
  `frame.work` zones), and every run exited 0.
- Not started, the value and name helpers return at once, so every variant costs about the guard alone
  (1.2–1.8 ns, within 0.3 ns of each other on each OS). Started without a server, the guarded variants cost
  3.0–4.1 ns against 3.1–3.4 ns for the unguarded baseline: the out-of-line `TracyIsStarted` call adds at most
  about 1 ns.
- Connected, the guard costs nothing measurable. The zone value adds about 9–12 ns, and the zone name another
  17–25 ns, because the client allocates and copies the name for every zone. If M1 needs cheaper zones, the
  name can be set only on a task's first slice, or carried by the value alone.
- In the same runs, `StartupProfiler` took p50 215.0–220.3 ms and `ShutdownProfiler` p50 163.0–169.4 ms on
  Windows, and 215.2–215.9 ms and 46.9–49.7 ms in WSL, in line with the first pass. These are **nf** figures
  under this pass's load; "Symbol worker and shutdown" shows what the shutdown time depends on.
- **Conditions.** The `tracy` measurement lock was held from 01:33:44 to 01:35:15 on 2026-11-01. At its start:
  free commit 25.2 GB of 64.1 GB, free physical memory 9.8 GB, system CPU 36.6%, none of our builds running
  (the `measure-begin` record is in the raw logs). The user's own processes were running and were left alone:
  4 FrostEditor (each with a `FrostEditor.com` process), 2 FrostServices and 25 FrostRuntime processes from
  `E:\r11`, Discord and Wallpaper Engine (the first version of this paragraph said "about 30" FrostRuntime
  processes; the recorded list has 25). `lghub_agent` held 178 MB of private memory at this point, so the first pass's 43 GB
  pressure was gone. The WSL VM had booted at 01:35 (load average 0.23–0.29, 4.0 GB of 5.9 GB free).

### Symbol worker and shutdown (second fix round)
The ShutdownProfiler figures above came from probes that load almost no DLLs and shut down about 30 ms after
`StartupProfiler`, so they say nothing about the symbol worker or a Qt process. A new probe, `tracy_symprobe`
(Release, TEST flags), was built against **nf**, **nc** and **ns** on both OSes. Each run:
1. optionally loads `Qt6Core`, `Qt6Gui` and `Qt6Widgets` 6.12.0, the Qt modules `frost.exe` links
   (`LoadLibraryExW` on Windows, `dlopen` in WSL);
2. records the module count, the thread count, whether `dbghelp.dll` and `symsrv.dll` are loaded, and
   `_NT_SYMBOL_PATH`;
3. times `StartupProfiler`, sleeps 0 or 5,000 ms, records the same state again (untimed; on Windows this
   includes a system-wide thread snapshot, so "0 ms" is not an immediate shutdown);
4. times `ShutdownProfiler`, unconnected.

The probe calls no DbgHelp function and never sets `_NT_SYMBOL_PATH`, and every Windows run started with the
variable unset. So the `dbghelp.dll` and `_NT_SYMBOL_PATH` rows below show only what Tracy does. A FrostIDE
process loads `dbghelp.dll` through its own users, and a developer may have set the variable (item 6).

5 runs per cell, in 5 interleaved rounds (each round runs every cell once). Then 3 runs of **nf** with Qt and a
15,000 ms sleep on Windows, to check that 5 s is past the preload. The task probe's prescribed zone ("name",
started without a server, 10M zones, then `ShutdownProfiler` about 30 ms after start) ran in the same rounds, 5
runs per configuration, as did 5 connected runs per configuration and thread count on Windows. ms (or ns per
zone), p50 (min–max):

| `ShutdownProfiler`, unconnected | **nf** | **nc** | **ns** |
|---|---|---|---|
| Windows, no extra DLLs (24–25 modules with **nf**, 21–22 without), sleep 0 | 64.5 (61.6–87.2) | 61.6 (32.4–63.4) | 62.5 (32.4–64.2) |
| Windows, Qt loaded (51 / 50 modules), sleep 0 | 151.1 (149.8–166.5) | 58.5 (32.9–62.0) | 57.3 (32.3–61.3) |
| Windows, no extra DLLs, sleep 5 s | 42.8 (38.8–56.1) | 54.1 (39.6–56.0) | 41.2 (38.2–55.7) |
| Windows, Qt loaded, sleep 5 s | 49.6 (40.2–55.7) | 41.6 (39.1–53.8) | 37.5 (34.6–55.8) |
| Windows, Qt loaded, sleep 15 s (3 runs) | 41.0 (38.6–41.8) | not run | not run |
| Windows, task probe, ~30 ms after start | 112.1 (110.0–113.7) | 55.4 (54.7–56.0) | 56.1 (54.6–56.6) |
| WSL, no extra libraries (7 modules), sleep 0 | 60.7 (60.0–61.5) | 20.6 (20.6–20.7) | 20.6 (20.5–20.6) |
| WSL, Qt loaded (46 modules), sleep 0 | 60.5 (59.2–62.4) | 20.6 (20.6–20.7) | 20.6 (20.5–20.6) |
| WSL, no extra libraries, sleep 5 s | 33.1 (21.9–40.4) | 34.2 (29.5–35.2) | 36.2 (30.9–38.5) |
| WSL, Qt loaded, sleep 5 s | 30.3 (22.2–36.8) | 31.9 (23.7–35.9) | 37.9 (33.2–40.5) |
| WSL, task probe, ~30 ms after start | 36.7 (36.2–39.1) | 33.9 (32.8–34.2) | 39.8 (39.6–40.3) |

| Process state (the same in every run of a cell) | **nf** | **nc** | **ns** |
|---|---|---|---|
| Windows threads before / started / after shutdown | 4 / 7 / 4 | 4 / 6 / 4 | 4 / 6 / 4 |
| Windows modules added by `StartupProfiler` | 9 or 10 | 9 | 9 |
| Windows `dbghelp.dll` loaded before / while started | yes / yes (imported) | no / no | no / no |
| Windows `symsrv.dll` loaded while started | no | no | no |
| Windows `_NT_SYMBOL_PATH` before / while started | unset / `;<probe dir>\` | unset / unset | unset / unset |
| WSL threads before / started / after shutdown | 1 / 5 / 1 | 1 / 4 / 1 | 1 / 3 / 1 |
| WSL modules added by `StartupProfiler` | 0 | 0 | 0 |

| Prescribed task zone ("name"), Windows | **nf** | **nc** | **ns** |
|---|---|---|---|
| Started, no server, ns per zone | 3.20 (3.18–3.29) | 2.96 (2.94–3.00) | 2.95 (2.94–2.99) |
| Connected, 1 thread, ns per zone | 50.1 (46.2–50.8) | 49.1 (46.7–50.2) | 48.6 (47.5–64.5) |
| Connected, 4 threads, ns per zone | 55.9 (54.5–56.0) | 55.8 (54.3–56.8) | 54.7 (54.1–56.3) |
| WSL: started, no server, ns per zone | 2.66 (2.55–2.71) | 2.70 (2.67–2.81) | 2.09 (2.04–2.13) |

- **The symbol preload is what made the Windows figures large.** With **nf**, a shutdown soon after start
  waited for the preload: p50 64.5 ms with 24–25 modules, 151.1 ms with Qt's three DLLs loaded (51 modules),
  and 112.1 ms in the task probe, whose shutdown comes about 30 ms after start, sooner than the symbol probe's
  (which first records the state). After 5 or 15 s the preload was done and **nf** fell to p50 41–50 ms. **nc** and **ns** took 32–64 ms in every Windows cell, whatever the
  modules. A real FrostIDE process loads more than these three Qt DLLs (platform plugin, graphics stack), so
  **nf** would wait longer than measured here; that was not measured.
- **WSL.** **nf** took about 60 ms soon after start (the libbacktrace setup and `/proc/kallsyms` read), and
  **nc** and **ns** 20.6 ms; after 5 s all three took 22–41 ms. Loading Qt made no difference, because
  libbacktrace loads nothing up front.
- **The remaining time** (20.5–64.2 ms with **ns**) is, as far as the source shows, Tracy's worker and
  compression threads noticing the shutdown and being joined; it was not broken down further.
- **Thread counts.** The thread that **nc** lacks on both OSes is the symbol worker. In WSL, **nf** and **nc**
  also ran a system-tracing thread (`StartSystemTracing` starts one when `SysTraceStart` succeeds,
  `TracyProfiler.cpp:1162-1180`; it is the only thread `TRACY_NO_SYSTEM_TRACING` removes), which **ns** does
  not. On Windows, without elevation, system tracing starts no thread.
- **Zones are no dearer.** In the same session the prescribed zone cost about the same with all three
  configurations on Windows (within 0.3 ns unconnected and 1.5 ns at the p50 connected). In WSL, **ns** zones
  were 0.6 ns cheaper unconnected, perhaps because no system-tracing thread runs; not investigated. The
  connected figures (p50 49–56 ns) are lower than the rework pass's 59.5 and 75.8 ns, which ran at 36.6% system
  CPU; the second verification measured 47.0–56.0 ns at 11.1%. The not-started cost was not measured again:
  the guard (`TracyIsStarted`) does not touch call stacks or system tracing.
- **Captures.** Every connected run on Windows captured exactly 250,000 or 1,000,000 zones, and 3 runs of the
  WSL **ns** task probe (1 thread, 250,000 zones, captured from Windows) also captured exactly 250,000 zones
  (61.4–73.8 ns per zone across the VM boundary). Every probe and capture exited 0.
- **Conditions.** The `tracy-2` measurement lock was held from 02:20:26 to 02:28:56 on 2026-11-01. At its
  start: free commit 31.4 GB of 64.1 GB, free physical memory 10.3 GB, system CPU 5.5%, none of our builds
  running. The user's own processes were left alone: Discord (6 processes), Wallpaper Engine and
  `lghub_agent` (189 MB private); no `E:\r11` process was running. The WSL VM had restarted at 02:25 (load
  average 0.00 at 02:25:35, 0.09 at the end). This hold is "lock 2" below. An earlier hold of the same lock
  ("lock 1", 02:14:56 to 02:17:16) ran only the WSL part for **nf** and **nc**, because Windows PowerShell
  refused the unsigned Windows scripts. At its start system CPU was 10.2%; the WSL VM had been up 4 minutes,
  and its load average was 0.95 when the runs began and 4.79 at the end. Lock 1's 50 runs are not in the
  tables above; "Lock 1 against lock 2" compares them.

**Lock 1 against lock 2 (WSL).** The same WSL cells, p50 (min–max), lock 1 / lock 2 (lock 2's figures are the
ones in the tables above):

| WSL, lock 1 / lock 2 | **nf** | **nc** |
|---|---|---|
| Symbol probe, no extra libraries, sleep 0, ms | 61.5 (57.7–75.6) / 60.7 (60.0–61.5) | 22.2 (20.6–38.9) / 20.6 (20.6–20.7) |
| Symbol probe, Qt loaded, sleep 0, ms | 60.9 (59.2–78.2) / 60.5 (59.2–62.4) | 21.1 (20.6–40.6) / 20.6 (20.6–20.7) |
| Symbol probe, no extra libraries, sleep 5 s, ms | 23.5 (22.0–39.1) / 33.1 (21.9–40.4) | 29.7 (22.7–40.6) / 34.2 (29.5–35.2) |
| Symbol probe, Qt loaded, sleep 5 s, ms | 31.6 (25.6–39.0) / 30.3 (22.2–36.8) | 30.4 (24.4–39.9) / 31.9 (23.7–35.9) |
| Task probe, `ShutdownProfiler` ~30 ms after start, ms | 49.9 (36.0–90.1) / 36.7 (36.2–39.1) | 33.7 (31.0–35.6) / 33.9 (32.8–34.2) |
| Task probe, prescribed zone, started, no server, ns per zone | 4.48 (2.63–4.90) / 2.66 (2.55–2.71) | 2.80 (2.68–4.60) / 2.70 (2.67–2.81) |

Lock 1 corroborates the medians soon after start and the spread after 5 s, but not the tight ranges of lock 2
or the **nf** task probe:
- Soon after start, the symbol probe's medians agree within 1.6 ms. Lock 1 had slow single runs that lock 2
  did not: **nc** reached 38.9 and 40.6 ms (lock 2: 20.6–20.7 ms) and **nf** 75.6 and 78.2 ms (lock 2: at
  most 62.4 ms).
- After 5 s, the ranges over the four **nf** and **nc** cells agree (22.0–40.6 against 21.9–40.4 ms), but
  the medians differ by up to 9.5 ms.
- In the task probe, **nf** shut down at p50 49.9 ms with one run at 90.1 ms (lock 2: p50 36.7 ms, at most
  39.1 ms), and its zone cost p50 4.48 ns against 2.66 ns. The **nc** medians agree (33.7 against 33.9 ms,
  2.80 against 2.70 ns), though one lock-1 zone run took 4.60 ns.

The tables and every figure quoted from them use lock 2 only. Lock 1 ran under more load: system CPU 10.2%
against 5.5% at the start of each hold, and a VM load average of 0.95 and 4.79 at the start and end of its
runs, against 0.00 and 0.09. Whether that caused its outliers was not investigated.

### Pinning check
- **Override without the dependency.** vcpkg 2026-09-26 in manifest mode, `install --dry-run` with the
  repository triplets and scratch install roots, once on the repository's `vcpkg.json` plus
  `"overrides": [{ "name": "tracy", "version": "0.14.1", "port-version": 1 }]` (no tracy dependency) and once on
  the unmodified file. Both plans are identical (gtest 1.18.0, vcpkg-cmake, vcpkg-cmake-config), both exit 0,
  and vcpkg prints nothing about the unused override.
- **frost-licensecheck** on the worktree, once with the override written into `vcpkg.json` (restored byte for
  byte afterwards) and once with the original file, against the main tree's `build/vcpkg_installed`: 6
  packages, 0 violations both times. From the manifest, licensecheck reads only whether `dependencies` is
  empty; it does not look at `overrides`.
- **Port version on disk.** The installed tree records it: `share/tracy/vcpkg.spdx.json` has `versionInfo`
  `0.14.1#1`, which licensecheck copies into the SBOM.
- **EXACT.** `find_package(Tracy 0.13.1 EXACT ...)` against the 0.14.1 install fails configure; only this
  upstream-version mismatch was tested.

## Consequences
- **M1 work list (only after the exception is granted):**
  - the manifest entry and override (items 2 and 4) and the triplet block in all three triplets (item 5), with
    the ASan triplet built and tested for the first time;
  - `FROST_TRACY`, refused by configure outside the dev presets, and `find_package(Tracy 0.14.1 EXACT ...)`;
  - a configure check that `Tracy::TracyClient`'s `INTERFACE_COMPILE_DEFINITIONS` contains `TRACY_ON_DEMAND`,
    `TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`, `TRACY_NO_CODE_TRANSFER`, `TRACY_MANUAL_LIFETIME`,
    `TRACY_NO_CALLSTACK`, `TRACY_NO_SYSTEM_TRACING` and `TRACY_NO_CRASH_HANDLER`, and not `TRACY_FIBERS` outside
    a fibers tree, because the mangled names do not cover the security options (item 5);
  - the Tracy dev preset per OS with its own install tree and the `tracy` manifest feature (item 4);
  - the wrapper header (item 8), plus a frost-lint rule that rejects `#include <tracy/...>` anywhere else;
  - the explicit per-run switch, the once-only start function and the shutdown order in the exit path
    (items 3 and 6);
  - one zone per resume slice in the scheduler (item 7);
  - a unit test that every wrapper macro is safe before `StartupProfiler`;
  - a shutdown test in the dev build: start Tracy, run `Task<T>` work with wrapper zones on every scheduler
    worker, stop and join the scheduler, `ShutdownProfiler`, exit; run under the ASan configurations (Windows
    ASan, Linux ASan+UBSan) and TSan; plus a test that a second start is refused;
  - in the same dev-build test on Windows: `dbghelp.dll` is not among the Tracy client's imports and is not
    loaded by `StartupProfiler`, and `_NT_SYMBOL_PATH` is the same before and after it (what the spike's
    `tracy_symprobe` checked);
  - a **release-build test**: every release and packaging configuration on both OSes contains no Tracy code.
    Tracy is not in the link inputs or in that preset's SBOM (licensecheck with `--vcpkg-installed` naming the
    preset's own tree, item 4), and the binaries contain no Tracy symbol or string (no `GetProfiler_CFG_`
    symbol, no `TracyPrf` handshake string).
- **Listener test (M2).** The listener-enumeration test (PLAN.md 2.2) runs a dev build with Tracy compiled in.
  Without the opt-in it sees no Tracy port (no TCP listener from 8086 to 8105, nothing beyond the expected list)
  and no UDP socket. With the opt-in it sees exactly one TCP listener on 127.0.0.1 and no UDP, and none after
  `ShutdownProfiler`. Both OSes.
- **A 5.2 row, only after approval.** If the user grants the exception, M1 adds a Tracy row to PLAN.md 5.2:
  TCP on 127.0.0.1, no authentication by design (an approved exception, ADR 0009); dev builds only, never in
  release or shipped builds; off unless explicitly enabled for a run; on-demand, no broadcast, no code transfer,
  no call stacks, no system tracing; started after the first frame; the tests above. Until then 5.2 is
  unchanged.
- **Several processes.** Each process that starts Tracy takes the first free port from 8086 (20 tries) unless
  `TRACY_PORT` is set. The supervisor sets `TRACY_PORT` per engine when engines are instrumented (M3), and each
  engine needs its own explicit opt-in.
- **Budgets.** No budget target changes. The rule that budget runs never start Tracy (item 3) is what keeps
  B1, B20 and B21 safe:
  - B1 is a 150 ms p50 first frame, and B21 allows no stall over 50 ms. `StartupProfiler` takes about 215 ms.
  - B20 allows 150 ms p50 and 300 ms p99 from exit to process gone. Unconnected on Windows, `ShutdownProfiler`
    alone took p50 up to 62.5 ms (max 64.2 ms) with item 5. Without `TRACY_NO_CALLSTACK` it took p50 up to
    151.1 ms (max 166.5 ms) with Qt loaded at low load, and p50 up to 184.6 ms (max 255.4 ms) under the first
    pass's load. The figure is no fixed property: it grows with load and, without `TRACY_NO_CALLSTACK`, with
    the loaded modules (item 6). Connected, it waits for the server. Even with item 5 it can take over 40% of
    B20's p50 by itself.
  - Compiled in but not started, a zone costs about 2 ns or less (p50 1.2–2.0 ns, loop included), with or
    without the value and name helpers. Connected without fibers, the first probe's unguarded zone costs p50
    32–79 ns (max 94 ns; the WSL figures include the cross-VM capture path), and the prescribed task zone p50
    59.5 ns on one thread and 75.8 ns on four (max 82.8 ns, Windows, rework pass at 36.6% CPU; p50 49–56 ns at
    5.5% CPU in the second fix round, with item 5 as with the earlier configuration). So even connected, 1,000
    task zones on the keystroke-to-paint path cost about 0.08 ms of B5's 3 ms p50.
- **Timer.** 0.14.0 added `TRACY_DISALLOW_HW_TIMER` for WSL2 and VMs. The WSL runs here used the hardware timer,
  and their counts and durations were consistent, so it stays off. Revisit if WSL traces show time running
  backwards.
- **Traces stay local.** The in-process client writes no files. Captures are saved by the developer's profiler,
  and the retention rules (PLAN.md 2.2) apply to any trace FrostIDE itself stores under `<cache>/traces`. A
  trace holds task names, messages and whatever else is instrumented, so it is treated like a log.
- **Upgrading** to 0.15 or later means revisiting this ADR (protocol, configuration names, options, and the
  threat model: whether the server queries are still trusted), bumping the override and the EXACT version, and
  updating the developers' profiler at the same time.

## Alternatives considered
- **0.13.1.** It would need all of the following:
  - `"overrides": [{ "name": "tracy", "version": "0.13.1", "port-version": 1 }]` in `vcpkg.json`;
  - a full vcpkg clone in bootstrap, or a copy of the port as an overlay;
  - `-DCMAKE_CXX_STANDARD=17` in the triplet block for clang-cl;
  - `TRACY_DELAYED_INIT`;
  - a 0.13.1 profiler, because the tools at our baseline are 0.14.1 and refuse its protocol.

  It also lacks the link-time configuration check, and its `TRACY_NO_CODE_TRANSFER` does not stop source
  transfer. It offers nothing that 0.14.1 lacks. Rejected.
- **The default lifetime (no `MANUAL_LIFETIME`).** A static profiler is built before `main` and starts three
  threads and its listener there. That breaks PLAN.md 2.1 startup hygiene and the "no listener before L0" rule,
  whatever the other options. (The all-interface listener and the UDP socket measured with the port defaults
  come from the missing `ONLY_LOCALHOST` and `NO_BROADCAST`, not from the lifetime model.) Rejected.
- **Never calling `ShutdownProfiler`** (ending the process with the profiler running) avoids the shutdown
  hazard but is what upstream calls an error for manual lifetime, and it leaves Tracy's threads running while
  the C runtime tears down. Kept only as the watchdog's fallback in item 6.
- **`TRACY_FIBERS` by default.** Cross-thread task spans work (100 of 100 `task.span` zones), but the global
  serial-queue lock costs 6.7x on Windows at four threads. It stays a separate opt-in build (item 7).
- **Keeping call stacks, with `TRACY_DBGHELP_LOCK`.** Upstream's way to share DbgHelp: FrostIDE would provide
  the lock functions, and every in-process DbgHelp user would have to take the same lock, including cpptrace,
  whose DbgHelp calls FrostIDE does not control (whether cpptrace can take an external lock was not checked).
  Tracy would still rewrite `_NT_SYMBOL_PATH` and preload symbols for every module, possibly from a symbol
  server. FrostIDE's zones never capture call stacks (item 8), but that does not make this worthless. Item 5
  drops system tracing chiefly because, without call stacks, its thread-name queries would go unanswered
  ("Tracy's symbol worker"). Keeping call stacks could keep system tracing, and the two give Tracy's sampled
  call stacks and context-switch data:
  - on Linux, where the kernel's perf and tracing permissions allow it, with software sampling through
    `PERF_SAMPLE_CALLCHAIN` (`TracySysTrace.cpp:826-871`). In WSL the system-tracing thread did start with
    **nf**; what it captured was not checked;
  - on Windows only when elevated, which FrostIDE never is.

  Item 5 gives this up. Sampling and context switches are lost only on Linux, since Windows never had them
  for FrostIDE, and external sampling profilers have to cover them. Rejected for item 5. Granting the
  exception means accepting that loss (Status). If the user wants sampled call stacks in Tracy, item 5 has
  to be revisited first. On Linux that brings the symbol worker back, with its slower shutdown (WSL p50
  about 60 ms against 20.6 ms soon after start) and the address resolution of the threat model. On Windows
  sampling needs elevation anyway. Not evaluated further.
- **`TRACY_NO_DBGHELP_INIT_LOAD=1` or `TRACY_SYMBOL_OFFLINE_RESOLVE`.** The environment variable skips only the
  preload (`TracyCallstack.cpp:847-857`): `SymInitialize`, the `_NT_SYMBOL_PATH` rewrite and the unshared
  DbgHelp use remain. The verifier measured it shortening Windows `ShutdownProfiler` from 121.8–142.7 ms to
  51.6–54.1 ms (3 + 3 runs, twice). `TRACY_SYMBOL_OFFLINE_RESOLVE` skips `DbgHelpInit` and the symbol loads
  (`:646-648`, `:700-703`) but keeps the symbol worker and the module enumeration. Neither is needed once call
  stacks are off. Rejected.
- **Vendoring the Tracy sources.** No vcpkg patch touches the client sources, so vendoring only adds upkeep. It
  would also put Tracy's TU under FrostIDE's warning flags. Rejected. Patching the client to authenticate the
  server is no option either: the protocol has no place for a token, so the profiler GUI would need the same
  patch.
- **If the exception is declined.** Then FrostIDE does not link Tracy, PLAN.md's "Tracy in dev builds" is
  dropped, and interactive profiling uses the budget instruments (ETW provider "FrostIDE", Perfetto export) and
  external sampling profilers. No FrostIDE code depends on Tracy before M1, so nothing has to be undone.
