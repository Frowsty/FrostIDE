<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0027: Verify tooling for M1 (parallel ctest, the WSL PATH, perf.ps1; tidy and TSan behind switches)

- Status: accepted; decision 5 amended on 2026-11-02 (tags after the kind label, at the seventh feature-first
  batch)
- Date: 2026-11-02
- Plan reference: PLAN.md section 4 (B28), 6.3, 6.4; ADR 0010 follow-ups; ADR 0011 F2; ADR 0031 (feature-first)

## Context
B28 (ADR 0010) found the dev loop dominated by two avoidable costs: the Windows tests ran one at a time, and
every Linux configure spent 15-27 s in CMake's FindOpenGL (reached through `find_package(Qt6)`), which probed
the 38 to 45 `/mnt/<drive>` PATH entries WSL appends, about 0.1 s per 9P miss. PLAN.md 6.4 also asks for
clang-tidy and a TSan leg in verify. The slice was rescoped for the feature-first phase (ADR 0031) to what makes
that loop faster; tidy and TSan are built but switched off.

## Decision
1. **clang-tidy** (`tools/verify_support.py tidy`, the root `.clang-tidy` and the scoped `src/app` and
   `tests/app` files, whose header comments give each exclusion's reason) checks the changed files' TUs, a
   changed header through one TU that includes it, and every TU under `-Full`. Off by default (decision 9).
2. **The changed-file list** is the files that differ between `merge-base(HEAD, -Base)` and the working tree,
   untracked files included, computed on a copy of the index so the repository's index is never written. It is
   only made when tidy or the TSan quick leg needs it.
3. **Parallel ctest on Windows.** verify.ps1 runs ctest with `-j -TestJobs` (default `-Jobs`). Isolation is declared
   in `frost_add_test` (`cmake/FrostLayers.cmake`), defaulted by the kind label (the first `LABELS` value) so that
   suites merged later are covered without wiring: `integration` takes `RESOURCE_LOCK frost_desktop` (process tests
   start frost.exe or helpers on private desktops, create windows, use per-user state), `perf` takes `RUN_SERIAL`;
   `RESOURCE_LOCK`, `RUN_SERIAL` and `PROCESSORS` keywords add to or replace that. Explicit rows:
   `app_startup_tests` (unit, private desktop and MessageBoxW) holds `frost_desktop`; `frost_process_tests`
   `PROCESSORS 2`; `sched_tests` and `text_tests` `PROCESSORS 2`; the stress suites `PROCESSORS 4`;
   `FrostRuntimeDlls.CopiesOncePerDirectory` `PROCESSORS 4` (a nested ninja build); `ToolchainProbe.Rejects.*` keep
   `frost_build_tree`. Every Windows ctest call passes `--interactive-debug-mode 0`: in ctest's default interactive
   mode each test starts with error mode 0, so a test that fails to start or crashes raises a box on the user's
   desktop whatever desktop it runs on; in mode 0 it inherits `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX` and
   just fails with its status. The WSL leg stays serial.
4. **The WSL PATH filter.** verify.sh drops the `/mnt/<drive>` entries from PATH right after sourcing env.sh
   (entries under the SDK root stay; `verify_support.py strip-path`) and then checks that its tools resolve. No
   apt package (GLU headers would add a dependency) and no machine-wide `/etc/wsl.conf` change.
5. **Module labels.** Every test carries `mod.<directory>` through `frost_add_test_directory`
   (`CMAKE_DIRECTORY_LABELS`); configure fails for a test directory added otherwise, for a test outside its build
   directory (ctest drops the label there) and for a `frost_add_test` whose first `LABELS` value is not a kind or
   whose later values, free-form tags such as `msbuild`, `screenshot` and `clangd`, are not lower-case names other
   than a kind. Tags never change the kind's defaults. The labels reach gtest_discover_tests as one quoted argument:
   spread into its property list, each element would become a property of its own. The TSan quick leg
   (`--tsan-quick`) selects by these labels: the modules of the changed files among io, rpc, sched and text plus
   their dependents. Off by default (decision 9).
6. **`tools/perf.ps1` and `tools/frost_perf.py`** take over ADR 0010's B28 harness: variants rebuild, noop,
   verify and wsl-leg; write-once results under `E:\FrostBuilds\perf\<milestone>\b28\`; a probe journal so a
   killed run is restored with `perf.ps1 -Restore`.
7. **B28's verify runs pass `-Base HEAD`**, so tidy and TSan (when switched on) see only the probe change.
8. **Anchored label filters** (`-LE '^stress$'`, `-L '^fuzz$'`, since `mod.fuzz` exists), and the WSL summary
   split and join keep an item's own ", " inside parentheses.
9. **Feature-first switches.** `-Tidy` and `-TsanQuick` (verify.sh `--tidy`, `--tsan-quick`) are off by default
   until the hardening phase; the summary then says `tidy not-run (off by default)` and, in WSL's quick leg,
   `tsan-quick not-run (off by default)`. Nothing verify checked before this slice is dropped.

## Measurements (2026-11-02, this machine, other agents running)
| What | Before | After |
|---|---|---|
| Windows Debug ctest, 488 tests (`-LE ^stress$`) | 135.3 s serial (CPU 48 % busy) | 52.9 s at `-j 6` (CPU 9 %) |
| Linux re-configure of `linux-clang` in the mirror | 30.45 s (38 `/mnt` entries) | 1.35 s (0 entries) |

At `-j 6` the 124 tests behind `frost_desktop` form the critical path (from 22.6 s to 52.9 s). Earlier soak
runs of this branch (5 x `-j 8`, 2 x `-j 24`) were clean.

The error boxes: on 2026-11-01 22:26-22:29 one parallel run's tests failed to start with 0xC0000142 and the
System log holds 31 "Application Popup" events for them, every one for an executable ctest started itself
(`frost_process_tests`, `io_integration_tests`, `sched_perf_tests`, `text_perf_tests`); the build tools a test
started failed the same way without a box. A probe test printed its error mode: 0x0 under ctest's default, 0x3
with `--interactive-debug-mode 0`, 0x8003 started directly by the hidden-desktop runner. The hidden desktop has
the interactive heap (20480 KB, `SharedSection=1024,20480,768`). The 0xC0000142 trigger was not reproduced: a
probe of 12 isolation-sensitive tests at `-j 4` and the serial and `-j 6` runs above were clean. Commit
exhaustion is a candidate (the System log shows chrome.exe failing with 0xC000012D and 0xC0000142 on 2026-10-31).

## Consequences
- The Windows test leg is about 2.5 times faster; WSL configures no longer pay the FindOpenGL searches.
- Merged later: new `tests/<x>` directories must use `frost_add_test_directory` (configure enforces it); a
  `frost_add_test` names its kind first and may add tags after it (`ui_screenshot_tests` has `LABELS unit
  screenshot`, `build_msbuild_tests` `LABELS integration msbuild`, `lang_clangd_tests` `LABELS integration clangd`;
  tools.verify_support checks them in the build tree); tests registered with `add_test` or `gtest_discover_tests`
  directly (main's `ui_native_tests`) set the lock themselves when they need it.
- The `frost_desktop` lock serialises every integration suite; narrowing it is a measured follow-up.

## Alternatives considered
- A test launcher (CMake `TEST_LAUNCHER`) that sets the error mode: unnecessary once ctest's own
  non-interactive mode was found.
- `RUN_SERIAL` for every process test, or splitting executables per lock: slower or more wiring than one lock.
- GLU development headers or `appendWindowsPath=false` in `/etc/wsl.conf`: a new dependency, or machine-wide.
- Tidy and the TSan quick leg on by default now: they cost the edit-verify loop time ADR 0031 wants spent on
  features.
