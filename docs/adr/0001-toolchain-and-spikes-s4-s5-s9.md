# ADR 0001: Pinned toolchain integration (spikes S4, S5, S9)

- Status: accepted
- Date: 2026-10-31
- Amended: 2026-11-01 (decision 1: how clang-cl's pinning options are spelled); 2026-11-01 by ADR 0029 (the
  lldb-dap that S5 found missing or unusable is built from the pinned LLVM sources in M4, for the first usable
  build's minimal debugger, instead of being sourced in M6; nothing else changes)
- Plan reference: PLAN.md section 2 (Compilers and STL, Build and dependencies, Static analysis), section 2.1, M0 spikes S4, S5, S9

## Context
M0 pins LLVM 23.1.4, MSVC STL 14.44.35207, Windows SDK 10.0.26100.0, Qt 6.12.0 and libstdc++ 14 (`sdk/sdk.lock.json`).
Three spikes check that the pinned set works together and record what it contains:
- S4: clang-cl/lld-link + STL 14.44 + Qt msvc2022_64 link and run together.
- S5: which tools the official LLVM 23.1.4 packages contain, per OS.
- S9: the baseline inspection counts that frost-featurecheck gates against later.

## Decision
1. **STL/SDK pinning on Windows uses the vctoolsdir, winsdkdir and winsdkversion options** for both clang-cl and
   lld-link (`cmake/toolchains/windows-clangcl.cmake`). clang-cl takes them joined to their value with a `-`
   prefix (`-vctoolsdir<dir>`, `-winsdkdir<dir>`, `-winsdkversion<version>`); lld-link takes `/vctoolsdir:<dir>`,
   `/winsdkdir:<dir>` and `/winsdkversion:<version>`. The toolset directory is located with vswhere, the SDK root
   from the `KitsRoot10` registry value. `/vctoolsversion` is not used: it only takes effect together with
   `/winsysroot`, and on its own it disables clang-cl's MSVC detection (headers then resolve to a nonexistent
   `Microsoft Visual Studio 10.0` directory and `<version>` is not found).

   Amendment (2026-11-01): until then clang-cl got `/vctoolsdir <dir> /winsdkdir <dir> /winsdkversion:<version>`.
   clang-cl 23.1.4 ignores `/winsdkversion:<version>`: under `clang-cl -###`, `/winsdkversion:10.0.99999.0`
   still includes `Include\10.0.26100.0\ucrt`, while `-winsdkversion10.0.99999.0` includes
   `Include\10.0.99999.0\ucrt`. The compile-side SDK version was therefore never pinned; clang-cl picked the
   newest SDK under the pinned root. REF-W1 has only 10.0.26100.0 installed, so no build used another SDK. The
   separate-value form also made sccache 0.19 read `<dir>` as a second input file and refuse to cache every
   compile (`cmake/FrostCompilerCache.cmake`). The toolchain rewrites the old spelling in existing build trees'
   cached `CMAKE_<LANG>_FLAGS` once (TODO(M1) there to remove that migration).
2. **The official LLVM release packages are used on both OSes** for compiling, linking, sanitizers, fuzzing,
   clangd, clang-tidy and clang-format. Debugging is the exception (see S5): the Windows package has no lldb, and the
   Linux package's lldb-dap links `libpython3.14`, which Ubuntu 24.04 lacks.
3. **`toolchain_probe`** (`tests/toolchain_probe`) compiles and runs every allowlisted C++26 feature, and has
   compile-fail tests proving that our flags reject `#embed` (`-Wc23-extensions` under `-Werror`), contracts syntax
   and reflection. Clang 23 *accepts* expansion statements and `export module` units in C++26 mode, so those two
   bans are enforced by frost-lint, with fixtures in `tests/toolchain_probe/banned/`.
4. **WSL 2 runs with a 6 GB memory cap and 8 GB swap on E:** (`%USERPROFILE%\.wslconfig`, plus mirrored networking
   for C1). Without the cap, the VM's page cache during the LLVM extraction exhausted the host commit limit and
   the VM was terminated. The Linux LLVM pin is the `.tar.xz` (≈100 MB to decompress) rather than the `.tar.zst`,
   whose 2 GiB window is the same problem that bsdtar refuses on Windows.

## Measurements
Machine: REF-W1 (24 threads, 32 GB), Windows 11 10.0.26300, WSL 2 3.0.3 (kernel 6.18.54), Ubuntu 24.04.

**S4 (link):** passed. `frost.exe` (clang-cl 23.1.4, `/MD`, STL 14.44.35207, SDK 10.0.26100.0, lld-link) links
Qt6Core/Gui/Widgets 6.12.0 msvc2022_64. It passes `--version`, `--selftest` (UTF-8 ACP, embedded manifest, Qt
platform, PerMonitorV2 DPI awareness, main window) and the offscreen MainWindow tests. No
`_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` is needed. The Linux leg (clang 23.1.4 + libstdc++ 14 + Qt 6.12.0 gcc_64)
passes the same suite.

**S5 (package contents):**

| Tool / component | Windows package | Linux package |
|---|---|---|
| clang / clang-cl, lld | yes | yes |
| clangd, clang-tidy, clang-format, clang-include-cleaner | yes | yes |
| diagtool, clang-tblgen, clang-scan-deps | yes | yes |
| llvm-rc, llvm-mt | yes | n/a |
| lldb, lldb-dap, lldb-server | **no** | yes, but lldb-dap needs `libpython3.14.so.1.0` (Ubuntu 24.04 ships 3.12) |
| ASan, UBSan, libFuzzer, profile runtimes | yes | yes (also TSan, MSan, HWASan) |
| Clang/LLVM CMake packages + static libraries (clangDaemon, clangTidy) | yes | yes |
| libc++ | no | no (MSan's instrumented libc++ must be built; `verify.sh -Nightly`) |

**S9 (baseline inspection counts, LLVM 23.1.4):**

| Source | Count | Method |
|---|---|---|
| Clang warning groups | 1202 total; 1157 contain at least one diagnostic; 45 are GCC-compatibility no-ops | `diagtool tree` for the groups; a group counts as implemented if it, or a group below it, owns a flag in `diagtool list-warnings` |
| Clang warning diagnostics | 2236 with a flag, 56 without | `diagtool list-warnings` |
| clang-tidy checks listed | 602 | `clang-tidy -checks=* --list-checks` |
| clang-tidy aliases | 162 (97 of them `clang-analyzer-*`) | "Check aliases" table in `clang-tools-extra/docs/clang-tidy/checks/list.rst` at `llvmorg-23.1.4` |
| clang-tidy primary checks (excluding `clang-analyzer-*`) | **406** (matches the plan's expectation) | the documented primary table; equals the listed non-CSA checks minus their aliases |
| CSA checkers | 112 stable, 44 alpha, 49 developer | `clang -cc1 -analyzer-checker-help`, `-alpha`, `-developer` |

These counts go into `tests/features/matrix.json` once frost-featurecheck lands, as the reference for the
inspection registry (M7).

**B28 (dev loop):** not yet measured under representative conditions. The first sample was taken with three
agents building in parallel and commit headroom near 5 GB. The measurement is repeated on a quiet machine before
the M0 exit.

## Consequences
- An MSVC toolset update installs a new `VC/Tools/MSVC/<ver>` directory, and the build keeps using 14.44.35207
  until `sdk.lock.json` changes. The 14.52 move needs VS 2026 Build Tools (user-elevated install) and LLVM ≥22,
  which we already have.
- M4 must source lldb-dap, for the first usable build's minimal debugger (ADR 0029, decision 12; before ADR 0029
  this was M6's open item): on both OSes it is built from the pinned LLVM sources into E:\sdk and `~/sdk`; on
  Linux without Python scripting, or against a matching libpython, as M4's spike S10 decides.
- frost-lint carries the only enforcement for expansion statements and named modules. If a later clang starts
  rejecting them, the compile-fail probes can take over.
- Host commit headroom is the main constraint on parallel builds (see `-Jobs`/`FROST_JOBS` in the verify scripts).

## Alternatives considered
- `/winsysroot` with a copied MSVC+SDK tree: fully hermetic, but it duplicates about 3 GB and adds a sync step;
  vctoolsdir and winsdkdir give the same pinning for the single installed VS.
- Relying on clang-cl auto-detection with no flags: works today, but a VS update could silently switch the STL.
- Building LLVM ourselves now: unnecessary for M0, since the official packages cover everything except debugging.
