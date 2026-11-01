# ADR 0022: Crash and freeze reporting, logging (crashpad, cpptrace, spdlog)

- Status: accepted for `m1/crash-diag` (feature-first phase, ADR 0031). No budget, test or warning is weakened.
- Date: 2026-11-01
- Plan reference: PLAN.md section 3 ("Crash and freeze reporting", "Self-diagnostics", "Privacy and network"),
  D2 (clang only), D26 (no upload until opt-in), section 7 M1 exit ("--selftest=crash and --selftest=freeze
  artifacts symbolize in verify -Full"), budgets B1, B5, B19, B20, B21; ADR 0012 (launcher plus frost_app.dll);
  feature matrix row I3.

## Context
The user dogfoods FrostIDE at work (ADR 0032): a crash or a hang must leave a report and a log that can be handed
over, nothing may be uploaded (D26), and self-tests that crash on purpose must never put a WER or CRT box on the
desktop. Everything we compile, third-party code included, is built by the pinned clang (D2). Child processes
start only through `io::IProcessLauncher`.

## Decision
1. **Dependencies** (vcpkg.json; licensecheck passes):
   - `crashpad` (Apache-2.0 AND BSD-3-Clause: crashpad, mini_chromium, lss on Linux), an overlay port in
     `third_party/vcpkg-ports/crashpad` at the baseline's revisions (crashpad efdc820b, mini_chromium e5169551).
     Windows builds with the pinned clang-cl/lld-link through mini_chromium's own clang toolchain
     (`mini_chromium_is_clang=true`, `clang_path=$FROST_LLVM_ROOT`) against the MSVC 14.44 toolset and SDK
     10.0.26100.0 that sdk.lock.json pins (never vswhere's "latest"). `Toolchain.CrashpadBuiltByClang` checks the
     `.llvm_addrsig` section in crashpad's libraries, which cl.exe never emits.
   - `cpptrace` 1.0.4 (MIT), overlay without libdwarf (LGPL) and zstd: `CPPTRACE_GET_SYMBOLS_WITH_NOTHING`, WinAPI
     unwinding on Windows, `_Unwind` plus cxxabi on Linux. It records addresses and modules only; nothing spawns
     addr2line or loads dbghelp. Symbolization happens offline (`tools/symbolize.py` with llvm-symbolizer).
   - `spdlog` 1.17 (MIT) without default features, so it formats with `std::format` and fmt is not added. Used
     header-only from one diag source file, without its registry or its file sinks.
   - Transitive: zlib (Zlib); curl on Linux (curl AND ISC AND BSD-3-Clause), linked into the handler only and
     never used (no `--url`). Host build tools, never linked: GN (`vcpkg-tool-gn` overlay that declares GN's
     BSD-3-Clause licence, checked against gn.googlesource.com LICENSE), vcpkg-gn, vcpkg-get-python-packages
     (setuptools from PyPI at build time), vcpkg-cmake-get-vars.
2. **Overlay patches to crashpad.** `frost-getopt.patch` replaces the public-domain getopt the Windows handler
   bundles (a dedication, not an SPDX licence on the allowlist) with an Apache-2.0 `getopt_long` written for
   FrostIDE from the POSIX specification and the glibc manual. `frost-pipe-dacl.patch` gives the handler's pipe a
   DACL for the current user and SYSTEM only with a medium no-write-up label (upstream also admits AppContainers
   and untrusted integrity and falls back to a descriptor without a DACL).
3. **Module.** New Qt-free module `src/diag` (frost::diag, deps core and io; third-party libraries linked PRIVATE).
   Logging's front end `frost/core/log.hpp` (levels, `ISink`, a noexcept `std::format` facade, B5: one relaxed
   load when disabled) lives in core so every module can log; the spdlog backend is in diag. Qt hooks live in
   `src/app/diagnostics_qt.cpp`.
4. **Handler topology.** One crashpad handler per frost process, started by `IProcessLauncher` (never
   `CrashpadClient::StartHandler`), with `killOnClose` (a job object on Windows), null standard streams, a clean
   environment, `--no-periodic-tasks`, annotations `prod`, `ver`, `plat`, and never `--url`; crashpad's forwarding
   to the system crash reporter is disabled. Windows: `--pipe-name=\\.\pipe\frost-crashpad-<pid>-<128-bit
   BCryptGenRandom hex>`, polled with WaitNamedPipeW for at most 5 s, then `SetHandlerIPCPipe`. Linux: one end of
   a `socketpair` passed as fd 3 (`--initial-client-fd=3`, new `ProcessSpec::posixInheritedFds`), then
   `SetHandlerSocket` with the handler's pid (so crashpad sets PR_SET_PTRACER under ptrace_scope=1).
   The client lives in frost_app.dll (Linux: frost); the static-CRT launcher is unchanged and still imports only
   KERNEL32. After a short-link restart only the restarted process starts a handler, from the directory
   frost_app.dll was loaded from, with an extended (`\\?\`) image path; `frost.restarted_from` names the real
   install directory. An elevated process starts no handler (the log says why).
5. **When.** GUI: fatal handlers and the async log at once (cheap, B1); the stall watchdog and, on the
   "frost-diag" thread, the crash handler and retention only after the first frame (no listener before L0,
   PLAN 2.2). `--version`, `--help`, `--selftest` and `--selftest=onscreen` are unchanged (no handler, no log file).
6. **Fatal paths.** Access violations, SIGSEGV, stack overflow and `quick_enforce`'s trap reach crashpad directly.
   `FROST_PRE`/`FROST_POST`/`FROST_ASSERT`/`enforce` (core's violation handler, `Violation::terminating`),
   `std::terminate`, `abort()` (Windows `_set_abort_behavior(0, ...)` plus SIGABRT), the CRT invalid-parameter and
   purecall handlers and Qt fatal messages in the GUI call `crashNow(reason)`: annotations (reason, expression,
   file:line, cpptrace frames in preallocated buffers) and `DumpAndCrash` with exit code 0xE0464154 on Windows,
   `raise(SIGABRT)` on Linux. With no active reporter `crashNow` writes the reason to stderr and terminates with
   the same status without reaching WER. Linux installs last-chance handlers that make the process non-dumpable
   before re-raising, so no kernel core goes to wsl-capture-crash, apport or systemd-coredump.
7. **Logging.** `<cache>/logs/frost-<UTC>-<pid>-<n>.log`, an spdlog async logger on its own one-thread pool
   ("frost-log", 1024 slots, overrun-oldest so the UI thread never blocks; drops are counted). Diag's own sink
   writes each record straight to an OS handle opened exclusively (`CREATE_NEW`, share-read only; POSIX
   `O_EXCL|O_NOFOLLOW`, 0600, `flock(LOCK_SH)`), rotates at 8 MiB and keeps the process's last 4 files. UTC
   timestamps. Level Info, overridden by `FROST_LOG_LEVEL=trace|debug|info|warn|error|critical|off`. Flush at exit
   has a 50 ms deadline (B20).
8. **Stall watchdog.** A stall is a busy stretch of the UI event loop (`awake` to `aboutToBlock`) with no progress
   (event start, awake, end of a `ScopedUiIdle`) for more than 50 ms; nested loops, modal size/move/menu loops
   and `ScopedUiIdle` count as idle. The UI thread counts stalls exactly (B21: one clock read and one atomic
   store per hook). A "frost-watchdog" thread polls every 10 ms while busy, parks after 1 s idle, samples the UI
   thread from 25 ms (Windows: suspend, copy the stack, resume, unwind the copy with RtlVirtualUnwind, after
   Chromium base/profiler, BSD-3-Clause; Linux: a real-time signal and an async-signal-safe unwind) and writes
   `<cache>/crashes/freezes/freeze-<UTC>-<pid>-<seq>.json` (schema `frost-freeze-report/1`, modules identified
   by PDB70 GUID/age or GNU build-id, frames as module index plus RVA; no document text). Interim report at 2 s,
   at most 20 per session, 10 s apart; late wakes mark a stretch unreliable; nothing is reported under a
   debugger. Linux binaries link with `--build-id=sha1`.
9. **Self-tests.** `--selftest=crash` starts the handler synchronously (10 s bound), prints
   `selftest: crash-reporter ok pid=... handler=... database=...`, then writes to 0xF00D in
   `frost::diag::selfTestCrash` (marker `FROST-SELFTEST-CRASH-SITE`); the handler writes the dump. `--selftest=freeze`
   runs an offscreen QApplication, blocks the UI thread 300 ms in `selfTestFreeze` and prints
   `selftest: freeze-report <path> samples=<n> duration-ms=<d>`. Both are command-line modes, so the existing
   dialog guards apply (`SEM_NOGPFAULTERRORBOX`, CRT reports to stderr, no abort message box); tests run them on a
   private desktop.
10. **Retention.** Over `<cache>/crashes` (crashpad database and freezes), `<cache>/logs` and `<cache>/traces`:
    delete items older than 30 days, then, while the total exceeds 1,000,000,000 bytes, the oldest first. Only
    crashpad reports (through its database), `freeze-*.json`, `frost-*.log` and regular files in traces/ are
    deleted; symlinks and junctions are never followed; active logs are skipped. Runs after the handler starts,
    then every 24 h of uptime.
11. **Cache root.** `%LOCALAPPDATA%\FrostIDE\caches` (absolute drive or UNC only); Linux `$XDG_CACHE_HOME` if
    absolute, else `$HOME/.cache`, then `FrostIDE`. POSIX crash and log directories are 0700.

## Measurements
Windows 11 (the development machine), Debug, 2026-11-01: the crashpad port builds with clang-cl in 1.8 min;
`FrostProcess.SelfTestCrashWritesASymbolizableDump` takes 0.12 s, `FrostProcess.SelfTestFreezeWritesAReport`
0.38 s, `FrostProcess.SelfTestCrashFromEveryDirectoryLength` 1.6 s. Handler start-time percentiles and the B1
before/after comparison are deferred to the hardening phase (ADR 0031).

## Consequences
- A crash leaves a minidump under `<cache>/crashes/crashpad` and a log under `<cache>/logs`; a hang leaves a JSON
  freeze report. Nothing leaves the machine.
- New listener: the Windows handler pipe (current user and SYSTEM, started after L0). Recorded here only until the
  M2 listener enumeration (I9) picks it up.
- Known gaps: crashes inside the launcher before frost_app.dll loads; `__fastfail` (/GS, CFG, heap corruption)
  reaches WER, not crashpad (registering crashpad's WER module is a persistent HKCU write, not done in M1).
- TODO(M1): adopt the shared cache-root resolver (m1/storage, m1/settings). TODO(M3): engine crash handlers and
  per-worker terminate handlers and alternate signal stacks. TODO(M5): Collect Logs, opt-in upload, symbol archive.
- The overlays are revisited at every vcpkg baseline bump and deleted once upstream builds with clang-cl against
  a pinned toolset and ships no public-domain code.

## Alternatives considered
- `CrashpadClient::StartHandler`: starts the child itself, bypassing `IProcessLauncher`.
- crashpad built by cl.exe: violates D2.
- cpptrace with libdwarf: LGPL.
- spdlog's file sinks: narrow-character paths and no multi-process-safe rotation.
- Freeze minidumps through `DumpWithoutCrash`: no stack walker for the samples and a larger privacy footprint.
- Suspending the UI thread and walking its stack in place: can deadlock on the heap or loader lock.
- A fixed 10 ms watchdog tick: wakes the CPU when idle and depends on the Windows timer resolution.
