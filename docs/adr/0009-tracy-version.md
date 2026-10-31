# ADR 0009: Tracy version

- Status: accepted. The decision is made in M0, but tracy is not in `vcpkg.json` yet. M1 adds it when it first
  instruments code, and this ADR is the dependency ADR for it (CLAUDE.md "Licences").
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("Self-diagnostics": Tracy in dev builds), 2.1 ("Dependencies": tracy pinned
  in M0; startup hygiene), 2.2 ("Local listeners", "Privacy and network"), 5.2, budgets B1, B5, B20 and B21, M0
  decision "Tracy version: 0.13.1 or 0.14.1"

## Context
PLAN.md uses Tracy for interactive profiling in dev builds. ETW and Perfetto markers stay the budget
instruments. M0 has to choose between Tracy 0.13.1 (2025-12-11) and 0.14.1 (2026-08-22):
- **Port availability.** Our builtin-baseline (vcpkg 2026.10.16, `9eec002d`) resolves `tracy` to 0.14.1#1.
  0.13.1#1 is in vcpkg's version database (git tree `14b48412…`), but the shallow `E:\sdk\vcpkg` clone does not
  contain that tree.
- **Compilers.** Our code is built with clang-cl on STL 14.44 and with clang on libstdc++ 14. The warning flags
  are /W4 /WX and -Werror, and PRODUCTION targets add `-Wglobal-constructors` and `-Wexit-time-destructors`.
- **Startup hygiene and listeners (PLAN.md 2.1, 2.2, 5.2).** No network I/O before L1 and no listener before L0.
  Every listener must appear in the enumeration test. Tracy's client is a TCP server with no authentication.
- **Coroutines.** `Task<T>` coroutines resume on whichever worker picks them up, so one task's slices run on
  different threads.
- **Crash handling.** Crashpad owns it (PLAN.md 2.2), so Tracy's crash handler must stay off.
- **Protocol.** The profiler GUI or `tracy-capture` has to speak the client's protocol version.

## Decision
1. **Tracy 0.14.1.** This is the version the 2026.10.16 baseline already resolves: port 0.14.1#1, port tree
   `8cbe9bf2`, source SHA-512 `53912d75…8ce5510b` (checked against the downloaded tarball). No `overrides` entry is
   needed.
2. **How the version is pinned:**
   - the builtin-baseline in `vcpkg.json`, which is already there;
   - `find_package(Tracy 0.14.1 EXACT CONFIG REQUIRED)` in M1's CMake. Tracy's package version file reports
     0.14.1, and EXACT rejects anything else (measured: a request for 0.13.1 fails configure against the 0.14.1
     install).

   If a baseline bump moves the port, configure therefore stops until this ADR and the developers' profiler
   version are revisited.
3. **Manifest entry for M1.** M1 adds `{ "name": "tracy", "default-features": false, "features": ["on-demand"] }`,
   which is `tracy[core,on-demand]`. Putting it behind a manifest feature that only the dev presets enable is
   recommended, so packaged builds do not install it.
   - `default-features: false` drops `crash-handler` (the port turns that into `TRACY_NO_CRASH_HANDLER`), because
     Crashpad handles crashes.
   - `fibers` stays off (item 6).
   - `cli-tools` and `gui-tools` never go into the manifest (item 8).
4. **Client options come from a per-port block in each FrostIDE triplet** (`x64-windows-clangcl`,
   `x64-windows-clangcl-asan`, `x64-linux-clang`):
   ```cmake
   # ADR 0009: Tracy client options. Nothing runs before StartupProfiler; loopback only; no
   # UDP broadcast; a connected server cannot read files through the client.
   if(PORT STREQUAL "tracy")
       list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
           -DTRACY_MANUAL_LIFETIME=ON -DTRACY_ONLY_LOCALHOST=ON
           -DTRACY_NO_BROADCAST=ON -DTRACY_NO_CODE_TRANSFER=ON)
   endif()
   ```
   The port exports these as `INTERFACE_COMPILE_DEFINITIONS` of `Tracy::TracyClient` (measured: `TRACY_ENABLE;
   TRACY_ON_DEMAND;TRACY_ONLY_LOCALHOST;TRACY_NO_BROADCAST;TRACY_NO_CODE_TRANSFER;TRACY_MANUAL_LIFETIME;
   TRACY_NO_CRASH_HANDLER`). On Windows it also exports `WINVER` and `_WIN32_WINNT` as 0x0A00, the same values
   that `FrostCompilerOptions.cmake` sets. Tracy 0.14 builds its configuration into the names of its entry points
   (for example `GetProfiler_CFG_E1_OD1_DI1_ML1_F0_DHT0_TF0_PH0`), so a TU compiled with different defines fails
   to link. This was measured on both OSes.
   - **`MANUAL_LIFETIME`:** nothing of Tracy runs before `tracy::StartupProfiler()`. There is no thread and no
     socket until then (measured). Without it, the profiler is a static object built before `main` that listens
     on all interfaces and broadcasts over UDP (measured).
   - **`ONLY_LOCALHOST`** binds IPv4 127.0.0.1 only, and **`NO_BROADCAST`** opens no UDP socket.
   - **`NO_CODE_TRANSFER`:** a connected server cannot read source files through the client.
   - **`on-demand`:** zones are dropped until a server connects.
5. **Dev builds only, and started only on request.**
   - Tracy is linked only when a `FROST_TRACY` CMake option is on. The dev presets turn it on; packaged builds
     never do.
   - Even then, `StartupProfiler` runs only when the developer asks for it. M1 picks the switch, for example a
     `--tracy` flag.
   - It runs on a background thread after the first frame, never on the launch path. It takes p50 215 ms
     (timer calibration sleeps 200 ms).
   - Budget runs (`frost-bench`, `verify -Perf`) never start it.
6. **No `TRACY_FIBERS`. `Task<T>` is instrumented one zone per resume slice.**
   - With fibers, every zone event takes the profiler's global serial-queue lock. That costs 6.7x per zone on
     Windows and 188x in WSL with four threads (measured).
   - Without fibers, a zone cannot stay open across a `co_await`. Instead, the scheduler opens one zone around
     each `resume()`, named after the task and carrying its task id as the zone value.
   - Fibers are part of the configuration in the mangled names, so they cannot be enabled per module. A developer
     who needs fiber timelines builds a separate tree with `tracy[fibers]`.
7. **FrostIDE code never calls Tracy macros directly.** Under `MANUAL_LIFETIME`, every macro dereferences a null
   profiler until `StartupProfiler` has run. An unguarded `FrameMark` and a `TracyLockable` built before startup
   both crash (measured: 0xC0000005 on Windows, SIGSEGV on Linux).
   - M1 adds one wrapper header. Its zone macro is `ZoneNamedN(var, name, TracyIsStarted)`, and its frame-mark,
     plot and message macros test `TracyIsStarted` first.
   - The wrapper compiles to nothing without `FROST_TRACY`.
   - `TracyLockable` is not used for any lock that can exist before startup, so `frost::core::Mutex` does not
     embed it.
8. **The server side is 0.14.1 too.** That means the profiler GUI and `tracy-capture` built from
   `tracy[gui-tools]` or `tracy[cli-tools]` 0.14.1 in a separate install root outside the repository, or
   upstream's 0.14.1 release binaries. The tools are never linked into or shipped with FrostIDE.

### 0.13.1 compared with 0.14.1
| Criterion | 0.13.1 (port 0.13.1#1) | 0.14.1 (port 0.14.1#1, in our baseline) |
|---|---|---|
| Resolves at our baseline | No. It needs an `overrides` entry, and the manifest install then fails because the shallow clone lacks the port tree. | Yes. An explicit override to 0.14.1#1 also installs (13 s). |
| Protocol | `ProtocolVersion` 76, `BroadcastVersion` 3. The 0.14.1 `tracy-capture` refuses it: "uses incompatible protocol version". | 82 / 3. Captured by the vcpkg 0.14.1 tools in every run. |
| clang-cl, STL 14.44 | The port build fails at clang-cl's default C++14: `copying variable of type 'std::atomic<Thread *>' invokes deleted constructor` (TracyProfiler.cpp:936). It builds with `CMAKE_CXX_STANDARD=17`. | Builds with 0 warnings. Consumers build with /W4 /WX and the PRODUCTION flags. |
| clang, libstdc++ 14 | The port builds (the default is gnu++17). | Builds with 0 warnings. Consumers build with -Werror. |
| Mismatched client defines | Not detected | Link error (configuration in the entry-point names) |
| Manual lifetime | Also needs `TRACY_DELAYED_INIT` (`#error` otherwise) | `TRACY_MANUAL_LIFETIME` alone |
| `TRACY_NO_CODE_TRANSFER` | Does not stop source transfer: `ServerQuerySourceCode` is outside the guard, so a connected server can read any file that is older than the executable and smaller than 256 KiB | Fixed in 0.14.0. `HandleSourceCodeQuery` is guarded. |
| Zones, frame marks, plots, messages, locks | Yes | Yes. Every capture had exact zone, message and plot counts. Frame marks and a lockable mutex were exercised but not counted, because csvexport cannot export locks. |
| Fibers (for `Task<T>`) | Yes | Yes. 0.14.0 attributes memory events to fibers. |
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
  using the repository triplets plus the item 4 block. Build trees were in the session scratchpad on Windows and
  in `~/frost-tracy-spike` on ext4 in WSL.
- Consumers were built with the repository toolchain files and `FrostCompilerOptions.cmake`. Timings come from
  Release builds (/O2, -O3).
- "default" means `tracy[core,on-demand]` with the item 4 options. "fibers" adds `fibers`.

**Load.** `lghub_agent` held 43.3 GB of private memory, and other agents were building. Commit headroom was
1.4–3.4 GB during the Windows runs. Treat the numbers as upper bounds.

| Build proof | Windows (clang-cl) | WSL (clang + libstdc++ 14) |
|---|---|---|
| `vcpkg install tracy[core,on-demand]` | 15 s (17.4 s wall), 0 warnings | 9.9 s (15.3 s wall), 0 warnings |
| `vcpkg install tracy[core,on-demand,fibers]` | 14 s (16.2 s wall), 0 warnings | 11 s (17.6 s wall), 0 warnings |
| Smoke client: one zone and one `FrameMark` per frame, 100 frames, PRODUCTION flags; size Debug / Release | default 699,904 / 266,240 B; fibers 701,952 / 257,024 B; runs OK | default 2,394,320 / 556,896 B; fibers 2,286,048 / 507,824 B; runs OK |
| One TU with `TRACY_FIBERS` undefined against the fibers library | Link error, as designed | Link error, as designed |
| 0.13.1#1 port through an overlay | Fails (C++14 default) | Builds |
| Manifest mode with an override to 0.14.1#1 / 0.13.1#1 | Installs / fails (shallow clone) | n/a |
| `licensecheck --vcpkg-installed` | 13 packages, 0 violations: tracy BSD-3-Clause; the cli-tools dependencies are capstone BSD-3-Clause, zstd `(BSD-3-Clause OR GPL-2.0-only)`, nlohmann-json MIT, ppqsort MIT | 7 packages, 0 violations: tracy BSD-3-Clause, pthreads Apache-2.0 |
| `tracy[cli-tools]` (capture, csvexport) | 3.4 min at one job. The first try at four jobs died with lld-link "out of memory" at 1.4 GB commit headroom. | Not built. WSL clients were captured from Windows over mirrored loopback. |

| Sockets and threads (probe: 3 s per phase) | Windows | WSL |
|---|---|---|
| Port defaults (no item 4 options), every phase from before `main` | 7 threads; TCP `:::8086`; UDP `0.0.0.0:55380` | not measured |
| Item 4 options, before `StartupProfiler` | 4 threads; no sockets | 1 thread; no sockets |
| Item 4 options, after `StartupProfiler` | 7 threads; TCP `127.0.0.1:8086`; no UDP | 5 threads; TCP `127.0.0.1:8086`; no UDP |
| Item 4 options, after `ShutdownProfiler` | 4 threads; no sockets | 1 thread; no sockets |

Unconnected cost: 10 runs per cell, 10M zones per run on one thread, p50 (min–max). "Not started" is a guarded
zone with `TracyIsStarted` false, and the loop is included, so it is an upper bound.

| Case | Windows default | Windows fibers | WSL default | WSL fibers |
|---|---|---|---|---|
| Not started, ns per zone | 1.67 (1.58–1.71) | 2.01 (1.82–2.04) | 1.44 (1.36–1.61) | 1.17 (1.13–1.23) |
| Started, no server, ns per zone | 2.99 (2.83–3.11) | 3.09 (2.81–3.58) | 2.98 (2.81–3.35) | 2.37 (2.31–2.80) |
| `StartupProfiler`, ms | 218.4 (210.7–222.7) | 215.7 (211.8–219.2) | 216.3 (214.0–241.5) | 215.3 (214.3–229.9) |
| `ShutdownProfiler`, ms | 157.5 (151.3–167.7) | 184.6 (158.7–255.4) | 44.5 (42.5–52.5) | 50.3 (45.9–52.8) |

Connected cost: 5 runs per cell, 250,000 zones per thread, mean ns per zone over the threads, p50 (min–max).
`tracy-capture` 0.14.1 ran on Windows.

| Client | Threads | default | fibers | fibers / default |
|---|---|---|---|---|
| Windows | 1 | 31.9 (30.2–32.5) | 35.7 (33.6–39.8) | 1.1x |
| Windows | 4 | 36.0 (34.2–39.4) | 240.8 (210.2–252.5) | 6.7x |
| WSL | 1 | 50.3 (48.4–76.9) | 1,094 (1,084–1,137) | 22x |
| WSL | 4 | 78.9 (70.6–94.1) | 14,857 (14,603–14,976) | 188x |

- **Capture contents.** Every capture reported the exact zone count: the benchmark zones plus 100 `frame.work`
  zones, plus, with fibers, 100 cross-thread `task.span` fiber zones. All 42 saved traces, from Windows and WSL
  clients, held 100 messages and 100 `probe.plot` points with values 0..99 (`csvexport -m` and `-u -p`).
  Windows clients waited about 333 ms for the server to connect.
- **Zone-count check.** With 50 dependent multiplies inside each zone (4 threads × 100,000 zones, 1 run each),
  csvexport's statistics match exactly: 400,000 benchmark zones and 100 `frame.work`, and with fibers 100
  `task.span`. The cost was 61.5 ns per zone for default and 602.2 ns per zone for fibers.
- **Why csvexport shows fewer empty zones.** Without the spin, csvexport counts 2–10% fewer zones than were
  captured, because the server's statistics skip zero-length zones. The timer steps in 10 ns on this machine
  (duration histogram in the raw logs).
- **Why fibers cost so much.** The fibers cost is the serial queue. `TRACY_FIBERS` sends every zone event
  through `Profiler::QueueSerial()`, which takes `m_serialLock` (`std::shared_mutex` under `_MSC_VER`, otherwise
  `std::mutex`). The sender thread also polls that lock with `try_lock`. Why WSL suffers so much more was not
  investigated.

## Consequences
- **M1 work list:**
  - the manifest entry (item 3) and the triplet block in all three triplets (item 4);
  - `FROST_TRACY` and `find_package(Tracy 0.14.1 EXACT ...)`;
  - the wrapper header (item 7), plus a frost-lint rule that rejects `#include <tracy/...>` anywhere else;
  - one zone per resume slice in the scheduler (item 6);
  - a unit test that every wrapper macro is safe before `StartupProfiler`.
- **Listener test (M2).** The listener-enumeration test runs a dev build with Tracy compiled in. Without the
  opt-in it expects no Tracy socket. With the opt-in it expects exactly one TCP listener on 127.0.0.1 and no UDP.
- **A new listener row for M1.** The Tracy port has no authentication. Any local process can connect, and under
  WSL mirrored networking so can Windows-side processes: the Windows `tracy-capture` read the WSL clients over
  127.0.0.1 here. A connected process can read zone names, messages and plots, but no file contents. This is
  acceptable only because Tracy is dev-only and opt-in. M1 adds a Tracy row to PLAN.md 5.2: TCP on 127.0.0.1;
  dev builds only; started on request after the first frame; no broadcast; no code transfer.
- **Several processes.** Each process that starts Tracy takes the first free port from 8086 (20 tries) unless
  `TRACY_PORT` is set. The supervisor sets `TRACY_PORT` per engine when engines are instrumented (M3).
- **Budgets.** No budget target changes. The rule that budget runs never start Tracy (item 5) is what keeps
  B1, B20 and B21 safe:
  - B1 is a 150 ms p50 first frame, and B21 allows no stall over 50 ms. `StartupProfiler` takes about 215 ms.
  - B20 allows 150 ms p50 and 300 ms p99 from exit to process gone. On Windows, `ShutdownProfiler` alone takes p50
    158–185 ms, max 255 ms.
  - Compiled in but not started, a zone costs about 2 ns or less (p50 1.2–2.0 ns, loop included). Connected
    without fibers, it costs p50 32–79 ns (max 94 ns), so 1,000 zones per frame cost under 0.1 ms of B5's 3 ms.
- **Timer.** 0.14.0 added `TRACY_DISALLOW_HW_TIMER` for WSL2 and VMs. The WSL runs here used the hardware timer,
  and their counts and durations were consistent, so it stays off. Revisit if WSL traces show time running
  backwards.
- **Traces stay local.** The in-process client writes no files. Captures are saved by the developer's profiler,
  and the retention rules (PLAN.md 2.2) apply to any trace FrostIDE itself stores under `<cache>/traces`.
- **Upgrading** to 0.15 or later means revisiting this ADR (protocol, configuration names, options), bumping
  the EXACT version and updating the developers' profiler at the same time.

## Alternatives considered
- **0.13.1.** It would need all of the following:
  - `"overrides": [{ "name": "tracy", "version": "0.13.1", "port-version": 1 }]` in `vcpkg.json`;
  - a full vcpkg clone in bootstrap, or a copy of the port as an overlay;
  - `-DCMAKE_CXX_STANDARD=17` in the triplet block for clang-cl;
  - `TRACY_DELAYED_INIT`;
  - a 0.13.1 profiler, because the tools at our baseline are 0.14.1 and refuse its protocol.

  It also lacks the link-time configuration check, and its `TRACY_NO_CODE_TRANSFER` does not stop source
  transfer. It offers nothing that 0.14.1 lacks. Rejected.
- **The default lifetime (no `MANUAL_LIFETIME`).** A static profiler runs before `main`. It adds three threads,
  listens on every interface (dual-stack `:::8086`) and broadcasts over UDP. That breaks PLAN.md 2.1 startup
  hygiene and the 2.2 listener rules. Rejected.
- **`TRACY_FIBERS` by default.** Cross-thread task spans work (100 of 100 `task.span` zones), but the global
  serial-queue lock costs 6.7x on Windows and 188x in WSL at four threads. It stays a separate opt-in build
  (item 6).
- **Vendoring the Tracy sources.** No vcpkg patch touches the client sources, so vendoring only adds upkeep.
  Rejected.
