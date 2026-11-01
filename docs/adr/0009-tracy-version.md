# ADR 0009: Tracy version

- Status: accepted for the version: Tracy 0.14.1 (item 1). Using Tracy in FrostIDE at all (items 2–9) is
  **conditional on the user's approval** of an exception to the listener policy (PLAN.md 2.2 "Local listeners"
  and 5.2): Tracy's client is an unauthenticated TCP listener, and it can have neither a token nor an ACL. Until
  the user grants that exception, M1 adds nothing (no `vcpkg.json` entry, no `FROST_TRACY` option, no 5.2 row),
  and items 2–9 are a plan, not a decision. If the user declines, see "If the exception is declined" under
  Alternatives. This ADR is the dependency ADR for tracy (CLAUDE.md "Licences").
- Date: 2026-10-31; revised 2026-11-01 after independent verification (threat model, shutdown order, warnings,
  pinning, and the per-zone cost of the prescribed task zones).
- Plan reference: PLAN.md section 2 ("Self-diagnostics": Tracy in dev builds), 2.1 ("Dependencies": tracy's
  version chosen in M0; startup hygiene), 2.2 ("Local listeners", "Privacy and network"), 5.2, budgets B1, B5,
  B20 and B21, M0 decision "Tracy version: 0.13.1 or 0.14.1"

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
- **Map the address space** through symbol and call-stack-frame queries (`:3958`, `:3975`); normal operation
  sends real pointers anyway.
- **Receive everything instrumented:** zone names, zone text and values (task names and ids), messages, plots
  and thread names. `StartupProfiler` also starts system tracing (context switches, sampling) when the
  process has the rights: on Windows only when elevated, which FrostIDE never is; on Linux it depends on the
  kernel's perf and tracing permissions, which were not checked here.

`TRACY_NO_CODE_TRANSFER` removes only the symbol-code query (`:3977-3980`) and the source-file `fopen` in
`HandleSourceCodeQuery` (`:4461-4462`). It does not stop any of the above. 0.13.1 trusts the same queries
(`ServerQueryString` and `ServerQueryDataTransferPart` read the same way there). The first version of this ADR
said that a connected process can read "no file contents"; that was wrong.

**Consequence.** While a Tracy-enabled FrostIDE runs with Tracy started, any local process (and, for a WSL
instance, any Windows-side process) can read and corrupt FrostIDE's memory. That is acceptable, if at all, only
for a developer's own dev build, started explicitly for one profiling run (item 3), and only with the user's
exception.

## Decision
1. **Tracy 0.14.1.** This is the version the 2026.10.16 baseline already resolves: port 0.14.1#1, port tree
   `8cbe9bf2`, source SHA-512 `53912d75…8ce5510b` (checked against the downloaded tarball).
2. **What M0 pins, and how M1 pins it.**
   - **M0** pins the version decision (this ADR) and the vcpkg builtin-baseline `9eec002d` (2026.10.16) that is
     already in `vcpkg.json`, which provides the port at 0.14.1#1. Nothing in `vcpkg.json` names tracy yet,
     because tracy is not a dependency until M1, and only if the exception is granted. PLAN.md's dependency list
     now says so ("version chosen in M0").
   - An `overrides` entry is possible today without the dependency, and frost-licensecheck accepts it (measured,
     see "Pinning check"). It changes nothing that vcpkg installs while tracy is not a dependency, so it would
     only record intent; it is added together with the dependency in M1 instead.
   - **M1**, with the dependency, adds:
     - `"overrides": [{ "name": "tracy", "version": "0.14.1", "port-version": 1 }]`. An override fixes both the
       upstream version and the port version, whatever the baseline says, so a baseline bump cannot move the
       port (0.14.1#1 installs this way, measured);
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
     `TRACY_NO_CODE_TRANSFER`, `TRACY_MANUAL_LIFETIME`, no crash handler.
   - Budget and perf runs (`frost-bench`, `verify -Perf`) never start it. This rule is new and awaits the
     user's confirmation too.
   - The developer starts it only on a machine without untrusted local processes or other users, and knows
     that, while it runs, a local process can read the IDE's memory (threat model).
4. **Manifest entry for M1.** M1 adds `{ "name": "tracy", "default-features": false, "features": ["on-demand"] }`,
   which is `tracy[core,on-demand]`. Putting it behind a manifest feature that only the dev presets enable is
   recommended, so packaged builds do not install it.
   - `default-features: false` drops `crash-handler` (the port turns that into `TRACY_NO_CRASH_HANDLER`), because
     Crashpad handles crashes.
   - `fibers` stays off (item 7).
   - `cli-tools` and `gui-tools` never go into the manifest (item 9).
5. **Client options come from a per-port block in each FrostIDE triplet** (`x64-windows-clangcl`,
   `x64-windows-clangcl-asan`, `x64-linux-clang`):
   ```cmake
   # ADR 0009: Tracy client options. Nothing runs before StartupProfiler; loopback only; no
   # UDP broadcast; no source or code transfer to a connected server.
   if(PORT STREQUAL "tracy")
       list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
           -DTRACY_MANUAL_LIFETIME=ON -DTRACY_ONLY_LOCALHOST=ON
           -DTRACY_NO_BROADCAST=ON -DTRACY_NO_CODE_TRANSFER=ON)
   endif()
   ```
   The port exports these as `INTERFACE_COMPILE_DEFINITIONS` of `Tracy::TracyClient` (measured: `TRACY_ENABLE;
   TRACY_ON_DEMAND;TRACY_ONLY_LOCALHOST;TRACY_NO_BROADCAST;TRACY_NO_CODE_TRANSFER;TRACY_MANUAL_LIFETIME;
   TRACY_NO_CRASH_HANDLER`). On Windows it also exports `WINVER` and `_WIN32_WINNT` as 0x0A00, the same values
   that `FrostCompilerOptions.cmake` sets. The ASan triplet was not built with Tracy in this spike; M1 builds it.
   - **Link-time check, and its limit.** Tracy 0.14 builds 8 options into the names of its entry points (for
     example `GetProfiler_CFG_E1_OD1_DI1_ML1_F0_DHT0_TF0_PH0`): `ENABLE`, `ON_DEMAND`, `DELAYED_INIT`,
     `MANUAL_LIFETIME`, `FIBERS`, `DISALLOW_HW_TIMER`, `TIMER_FALLBACK` and `PLATFORM_HEADER`
     (`TracyMangle.hpp:15-71`). A TU compiled with a different value for one of these fails to link; only a
     `TRACY_FIBERS` mismatch was tested (both OSes). The security options (`ONLY_LOCALHOST`, `NO_BROADCAST`,
     `NO_CODE_TRANSFER`, `NO_CRASH_HANDLER`) are not in the names: dropping them from a triplet goes unnoticed at
     link time. M1 therefore adds a configure check (Consequences).
   - **`MANUAL_LIFETIME`:** nothing of Tracy runs before `tracy::StartupProfiler()`. There is no thread and no
     socket until then (measured). Without it, the profiler is a static object whose threads and listener start
     before `main`.
   - **`ONLY_LOCALHOST`** binds IPv4 127.0.0.1 only, and **`NO_BROADCAST`** opens no UDP socket. Without them the
     listener is dual-stack on every interface (`:::8086`) and a UDP broadcast socket is open (measured on the port
     defaults, which lack all of item 5's options).
   - **`NO_CODE_TRANSFER`:** a connected server cannot make the client open source files or copy code bytes. It
     can still read memory (threat model).
   - **`on-demand`:** zones are dropped until a server connects.
6. **Lifetime: start at most once, and shut down in a fixed order.**
   - `StartupProfiler` runs on a background thread after the first frame, never on the launch path. It takes p50
     215 ms (timer calibration sleeps 200 ms). Starting while other threads run is safe: `StartupProfiler`
     publishes the started flag only after the profiler is fully built (`TracyProfiler.cpp:1265-1272`).
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
   - Unconnected, `ShutdownProfiler` alone took p50 158–185 ms on Windows and 45–50 ms in WSL across both
     passes (Measurements). B20 is unaffected only because budget runs never start Tracy (item 3).
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
| clang-cl, STL 14.44 | The port build fails at clang-cl's default C++14: `copying variable of type 'std::atomic<Thread *>' invokes deleted constructor` (TracyProfiler.cpp:936). It builds with `CMAKE_CXX_STANDARD=17`. | Builds. Tracy's own TU has 7 warnings at the port's flags in every build (none are errors there). Our consumers build warning-free with /W4 /WX and the PRODUCTION flags. |
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
  using the repository triplets plus the item 5 block. Build trees were in the session scratchpad on Windows and
  in `~/frost-tracy-spike` on ext4 in WSL.
- Consumers were built with the repository toolchain files and `FrostCompilerOptions.cmake`. Timings come from
  Release builds (/O2, -O3).
- "default" means `tracy[core,on-demand]` with the item 5 options. "fibers" adds `fibers`.

**Load.** First pass (2026-10-31): `lghub_agent` held 43.3 GB of private memory, and other agents were building.
Commit headroom was 1.4–3.4 GB during the Windows runs. Treat the numbers as upper bounds. The rework pass
(2026-11-01, "Task zones") ran under the measurement lock with the conditions quoted there.

| Build proof | Windows (clang-cl) | WSL (clang + libstdc++ 14) |
|---|---|---|
| `vcpkg install tracy[core,on-demand]` | 15 s (17.4 s wall); Tracy's TU: 7 warnings in Debug and 7 in Release | 9.9 s (15.3 s wall); 0 warnings in Debug and Release |
| `vcpkg install tracy[core,on-demand,fibers]` | 14 s (16.2 s wall); 7 + 7 warnings | 11 s (17.6 s wall); 0 + 0 warnings |
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
| `tracy[core,on-demand]` (final install) | 7 | 7 | 0 | 0 |
| `tracy[core,on-demand,fibers]` (final install) | 7 | 7 | 0 | 0 |
| Manifest mode, override to 0.14.1#1 | 7 | 7 | not built | not built |
| `tracy[cli-tools]` tree, client TU | 7 | 7 (in a log shared with the tools) | not built | not built |

The 7 Windows warnings are the same in every build:
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
build (Debug and Release, no-fibers install; flags as in Context): `smoke.cpp`, the 25-line smoke client (one
zone and one `FrameMark` per frame), and `surface.cpp`, which uses every macro the M1 wrapper would expand to (a
guarded `ZoneNamedN` with `ZoneValueV`, `ZoneNameV` and `ZoneTextV`, a nested guarded zone, `FrameMark`,
`FrameMarkNamed`, `TracyPlot` with `int64_t` and `double`, `TracyMessageL`, `TracyMessage`, `TracyIsConnected`).
Each was compiled twice: with Tracy's include directory as CMake emits it for `Tracy::TracyClient`, and as an
ordinary `-I` include. A second pass with errors turned back into warnings and no error limit listed every
diagnostic.

| Tracy include | Windows clang-cl | WSL clang |
|---|---|---|
| System, as CMake emits it (`-imsvc`, `-isystem`) | 0 warnings; both sources build in both configurations | 0 warnings; both build |
| Ordinary `-I` | 3 warnings in Tracy's headers; `/WX` fails the build | 1 warning in Tracy's headers; `-Werror` fails the build |

The warnings with an ordinary include, the same for both sources and both configurations, all from our
`-Wconversion` (paths under `include/tracy/`):
```text
client/TracyCallstack.hpp(110,48): warning: implicit conversion changes signedness: 'int' to '__size_t' (aka 'unsigned long long') [-Wsign-conversion]
client/TracyCallstack.hpp(111,73): warning: implicit conversion changes signedness: 'int32_t' (aka 'int') to 'unsigned long' [-Wsign-conversion]
common/TracyQueue.hpp(438,39): warning: implicit conversion loses integer precision: 'int' to 'uint8_t' (aka 'unsigned char') [-Wimplicit-int-conversion]
```
The first two are in a Windows-only branch; the third appears on both OSes. No diagnostic points into our
sources in either mode, so the macro expansions themselves are clean. Tracy's headers are clean under our flags
only because they are system headers: that is why item 8 keeps them so, and why the frost-lint rule keeps the
wrapper the only place that includes them.

### Sockets and threads
| Sockets and threads (probe: 3 s per phase) | Windows | WSL |
|---|---|---|
| Port defaults (no item 5 options, crash handler on), every phase from before `main` | 7 threads; TCP `:::8086`; UDP `0.0.0.0:55380` | not measured |
| Item 5 options, before `StartupProfiler` | 4 threads; no sockets | 1 thread; no sockets |
| Item 5 options, after `StartupProfiler` | 7 threads; TCP `127.0.0.1:8086`; no UDP | 5 threads; TCP `127.0.0.1:8086`; no UDP |
| Item 5 options, after `ShutdownProfiler` | 4 threads; no sockets | 1 thread; no sockets |

The "port defaults" row was a `tracy[cli-tools]` client, which lacks every item 5 option, not only
`MANUAL_LIFETIME`: the all-interface listener and the UDP socket come from the missing `ONLY_LOCALHOST` and
`NO_BROADCAST`, and only "from before `main`" comes from the default lifetime. The Windows item 5 rows were first
measured on the fibers build; a re-run on the recommended no-fibers build (2026-11-01) gave the same result in
every phase. In WSL both builds were measured and agree.

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
  Windows, and 215.2–215.9 ms and 46.9–49.7 ms in WSL, in line with the first pass.
- **Conditions.** The `tracy` measurement lock was held from 01:33:44 to 01:35:15 on 2026-11-01. At its start:
  free commit 25.2 GB of 64.1 GB, free physical memory 9.8 GB, system CPU 36.6%, none of our builds running
  (the `measure-begin` record is in the raw logs). The user's own processes were running and were left alone:
  four FrostEditor instances, FrostServices and about 30 FrostRuntime processes from `E:\r11`, Discord and
  Wallpaper Engine. `lghub_agent` held 178 MB of private memory at this point, so the first pass's 43 GB
  pressure was gone. The WSL VM had booted at 01:35 (load average 0.23–0.29, 4.0 GB of 5.9 GB free).

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
    `TRACY_ONLY_LOCALHOST`, `TRACY_NO_BROADCAST`, `TRACY_NO_CODE_TRANSFER`, `TRACY_MANUAL_LIFETIME` and
    `TRACY_NO_CRASH_HANDLER`, and not `TRACY_FIBERS` outside a fibers tree, because the mangled names do not
    cover the security options (item 5);
  - the wrapper header (item 8), plus a frost-lint rule that rejects `#include <tracy/...>` anywhere else;
  - the explicit per-run switch, the once-only start function and the shutdown order in the exit path
    (items 3 and 6);
  - one zone per resume slice in the scheduler (item 7);
  - a unit test that every wrapper macro is safe before `StartupProfiler`;
  - a shutdown test in the dev build: start Tracy, run `Task<T>` work with wrapper zones on every scheduler
    worker, stop and join the scheduler, `ShutdownProfiler`, exit; run under the ASan configurations (Windows
    ASan, Linux ASan+UBSan) and TSan; plus a test that a second start is refused;
  - a **release-build test**: every release and packaging configuration on both OSes contains no Tracy code.
    Tracy is not in the link inputs or the SBOM, and the binaries contain no Tracy symbol or string (no
    `GetProfiler_CFG_` symbol, no `TracyPrf` handshake string).
- **Listener test (M2).** The listener-enumeration test (PLAN.md 2.2) runs a dev build with Tracy compiled in.
  Without the opt-in it sees no Tracy port (no TCP listener from 8086 to 8105, nothing beyond the expected list)
  and no UDP socket. With the opt-in it sees exactly one TCP listener on 127.0.0.1 and no UDP, and none after
  `ShutdownProfiler`. Both OSes.
- **A 5.2 row, only after approval.** If the user grants the exception, M1 adds a Tracy row to PLAN.md 5.2:
  TCP on 127.0.0.1, no authentication by design (an approved exception, ADR 0009); dev builds only, never in
  release or shipped builds; off unless explicitly enabled for a run; on-demand, no broadcast, no code transfer;
  started after the first frame; the tests above. Until then 5.2 is unchanged.
- **Several processes.** Each process that starts Tracy takes the first free port from 8086 (20 tries) unless
  `TRACY_PORT` is set. The supervisor sets `TRACY_PORT` per engine when engines are instrumented (M3), and each
  engine needs its own explicit opt-in.
- **Budgets.** No budget target changes. The rule that budget runs never start Tracy (item 3) is what keeps
  B1, B20 and B21 safe:
  - B1 is a 150 ms p50 first frame, and B21 allows no stall over 50 ms. `StartupProfiler` takes about 215 ms.
  - B20 allows 150 ms p50 and 300 ms p99 from exit to process gone. On Windows, `ShutdownProfiler` alone takes p50
    158–185 ms, max 255 ms, unconnected; connected, it waits for the server (item 6).
  - Compiled in but not started, a zone costs about 2 ns or less (p50 1.2–2.0 ns, loop included), with or
    without the value and name helpers. Connected without fibers, the first probe's unguarded zone costs p50
    32–79 ns (max 94 ns; the WSL figures include the cross-VM capture path), and the prescribed task zone p50
    59.5 ns on one thread and 75.8 ns on four (max 82.8 ns, Windows). So even connected, 1,000 task zones on the
    keystroke-to-paint path cost about 0.08 ms of B5's 3 ms p50.
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
- **Vendoring the Tracy sources.** No vcpkg patch touches the client sources, so vendoring only adds upkeep. It
  would also put Tracy's TU under FrostIDE's warning flags. Rejected. Patching the client to authenticate the
  server is no option either: the protocol has no place for a token, so the profiler GUI would need the same
  patch.
- **If the exception is declined.** Then FrostIDE does not link Tracy, PLAN.md's "Tracy in dev builds" is
  dropped, and interactive profiling uses the budget instruments (ETW provider "FrostIDE", Perfetto export) and
  external sampling profilers. No FrostIDE code depends on Tracy before M1, so nothing has to be undone.
