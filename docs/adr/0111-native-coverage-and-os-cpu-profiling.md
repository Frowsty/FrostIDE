# 0111: Native LLVM coverage and installed OS CPU profilers

Status: implementation-first source; runtime acceptance deferred.

RUN-12 invokes the pinned existing LLVM llvm-profdata and llvm-cov tools. The stage payload includes
these LLVM executables alongside clangd; LLVM remains under the existing Apache-2.0 WITH LLVM-exception
dependency decision. FrostIDE does not infer or rewrite instrumentation flags. A C++ run configuration
must already resolve an instrumented executable, or actual raw profiles are explicitly imported. Real
baseline mappings, raw profiles and exported LCOV supply the results. Observed binary/source hashes are
retained, but an arbitrary external binary's build provenance is not claimed. Independent coverage gutter
counts expire on source edits/reloads and coexist with diagnostics/breakpoints.

RUN-13 invokes installed Linux perf or installed Windows WPR/xperf through IProcessLauncher/RunSession.
These are external tools, not linked, copied, downloaded or redistributed dependencies. This introduces
no bundled GPL or proprietary code and no installer or elevation step. Missing tools/recording permission
remain explicit failures. The perf/kernel tooling licence remains the installed system's responsibility;
Windows Performance Toolkit remains a separately installed Microsoft component.

Normal selected Native/CMake/MSBuild preparation uses existing Save All/build/task owners. CPU sampling
runs for an explicit 1–300 second interval, ending the owned process tree at its boundary. Linux perf owns
its recorded command and descendants. Windows uses a fresh named WPR instance, with -instancename last
on every start/stop/cancel command. Explicit consent explains that the Windows report is system-wide;
the actual owned target PID is retained. Only that instance is retired, including after cancellation or
project-trust revocation; no global/default recorder stop is used. The short installed-OS recorder cleanup
command has retirement authority independent of project execution authority. The recorder's captured
image identity is checked again before cleanup.

The product preserves genuine perf/xperf report text instead of guessing undocumented/version-dependent
columns or inventing resolved symbols/counts. Local xperf symbol paths and local symcache are explicit;
symbol servers and debuginfod are not enabled. Profiles and reports remain in a user-chosen local artifact
directory, not an implicitly retained IDE cache. Original binary/trace identities are revalidated around
analysis. Empty/unavailable/unsupported output is visible. No runtime acceptance or performance result
is claimed by the authored hot-function fixtures.

Protocol sources: [LLVM coverage](https://llvm.org/docs/CommandGuide/llvm-cov.html),
[llvm-profdata](https://llvm.org/docs/CommandGuide/llvm-profdata.html),
[WPR instance ownership](https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options),
[xperf sampled profile](https://learn.microsoft.com/previous-versions/windows/desktop/xperf/profile),
[local symbol configuration](https://learn.microsoft.com/windows-hardware/test/wpt/symbols), and
[perf report](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-report.txt).
