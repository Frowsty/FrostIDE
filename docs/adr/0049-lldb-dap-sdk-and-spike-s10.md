<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0049: lldb-dap built from the pinned LLVM sources, and spike S10

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 (Debuggers; Build and dependencies), section 6.3 (the SDK leg), M4 (the
  minimal debugger, spike S10); ADR 0029 decision 12; ADR 0001 (spike S5)

## Context
U1's minimal debugger runs lldb-dap on both operating systems. Spike S5 found no usable binary: the official
LLVM 23.1.4 Windows package has no lldb or lldb-dap, and the Linux package's lldb-dap links `libpython3.14`,
which Ubuntu 24.04 lacks. So M4 builds lldb-dap from the pinned LLVM sources on REF-W1 and in WSL and pins the
result (the SDK leg). Spike S10 then asks whether that lldb-dap debugs MSVC-built programs through their PDBs
well enough for U1; if it cannot, M4 stops for the user.

## Decision
1. **Sources.** `llvm-project-23.1.4.src.tar.xz` from the official GitHub release, pinned in
   `sdk/sdk.lock.json` (`sources.llvm`) with SHA-256
   `819ecf7351f1a66f76b2d701d8942faaf5faeb324cb90c9d5b090de142434bb2`. That is the digest GitHub publishes for
   the asset, and `gh attestation verify --bundle` (gh 2.103.3) accepted the release's Sigstore attestation,
   signed by `llvm/llvm-project/.github/workflows/release-sources.yml@refs/tags/llvmorg-23.1.4` (the bundle's
   URL, hash and signer are pinned too; `build-lldb.ps1 -VerifyAttestation` repeats the check). The tarball
   stays in `E:\sdk\_downloads` (and `~/sdk/_downloads`). No new dependency: LLVM is already pinned and
   `Apache-2.0 WITH LLVM-exception` is on the licence allowlist.
2. **Local recipes, no CI.** `tools/build-lldb.ps1` (Windows) and `tools/build-lldb.sh` (WSL) reproduce the
   artifact: unpack cmake, llvm, clang, lldb, third-party and libc without their test trees into
   `<sdk>/_staging/lldb-23.1.4`, configure Release with CMake 4.4.5 and Ninja 1.13.2, build
   `ninja distribution` for `LLVM_DISTRIBUTION_COMPONENTS`, install, compare with the pin, delete the trees.
   Both pass the pinned compilers directly instead of the repository's toolchain files, because those set
   `CMAKE_SYSTEM_NAME`, which makes LLVM treat the build as a cross build and configure a second, toolchain-less
   NATIVE tree for its tablegens.
   - Windows: the pinned clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt with the same `-vctoolsdir`,
     `-winsdkdir` and `-winsdkversion` pins as `cmake/toolchains/windows-clangcl.cmake`; the static CRT (`/MT`),
     so the runtime set needs no VC runtime DLLs; `LLVM_ENABLE_DIA_SDK=OFF`, so LLDB reads PDBs with its native
     reader (the default reader in 23.1.4 anyway); `LLVM_DISABLE_ASSEMBLY_FILES=ON` (BLAKE3's MASM files would
     need ml64.exe); `-j6`, two concurrent links, the whole tree at BelowNormal priority.
   - Linux: the pinned clang and clang++ against libstdc++ 14 (`--gcc-install-dir`), ld.lld, `-j4`,
     `LLVM_PARALLEL_LINK_JOBS=1` and nice 10 for the 6 GB VM; `install-distribution-stripped`; `lldb-server` is
     a component, because LLDB debugs a local process on Linux through it.
   - Both: clang and lldb only, the X86 target, Release without assertions; Python, Lua, SWIG, libedit,
     curses, LZMA, libxml2, zlib, zstd, curl, cpp-httplib, telemetry and LLDB's MCP protocol servers off; no
     tests, docs, examples or benchmarks. The Linux engine has no HTTP client at all. On Windows LLVM's HTTP
     library always links WinHTTP, so the Debuginfod and SymStore symbol locators can still download symbols
     when `DEBUGINFOD_URLS`, `_NT_SYMBOL_PATH`, `_NT_ALT_SYMBOL_PATH` or their `plugin.symbol-locator.*`
     settings name an http(s) server; FrostIDE starts lldb-dap without those variables and with
     `--no-lldbinit`, as the test does, so nothing downloads unless the user asks for it (PLAN.md section 2,
     Privacy and network: symbol servers stay off until enabled).
3. **The artifact and its pin.** Windows installs `E:\sdk\lldb-23.1.4` (`bin\lldb-dap.exe`, `bin\lldb.exe`,
   `bin\liblldb.dll`, `bin\lldb-argdumper.exe`); Linux installs `~/sdk/lldb-23.1.4` (`bin/lldb-dap`,
   `bin/lldb`, `bin/lldb-server`, `bin/lldb-argdumper`, `lib/liblldb.so*`). Every file's SHA-256 (and on Linux
   every symbolic link's target) is pinned in `windows.lldb.files` and `linux.lldb.files`; a recipe run or a
   check fails on a file that differs, is missing or is not pinned. The import library is dropped (nothing
   links liblldb).
4. **Bootstrap.** `tools/bootstrap.ps1` and `tools/bootstrap-wsl.sh` gain an `lldb` step, last: an installed
   artifact is checked against the pin (`build-lldb.ps1 -VerifyOnly`, `build-lldb.sh --verify-only`), a missing
   one is built by the recipe (14 minutes on REF-W1, 19 in WSL). `FROST_SKIP_LLDB=1` skips it in WSL.
5. **No Python scripting on either OS, as this slice specified.** No libpython to ship or match. S10's answer to
   ADR 0001's open choice: on Windows the default formatters do not need Python (every MSVC STL formatter in
   LLDB 23.1.4 is C++, `MsvcStl*.cpp`). On Linux the libstdc++ container formatters (vector, map, set, list,
   forward_list, deque, the unordered containers, variant) are Python (`gnu_libstdcpp.py`); without it a
   `std::vector` or `std::map` shows `size=0` and no elements (Measurements), while `std::string`, `unique_ptr`,
   `optional` and the other C++ formatters work. Whether Linux gets Python before U1 or waits for M6's STL
   formatters (PLAN.md M6) is the user's call (Consequences).
6. **Test.** `sdk_lldb_tests` (`tests/sdk`, label `integration`, tag `lldb`) starts the pinned lldb-dap at its
   absolute path through frost::io's launcher, with `--no-lldbinit` and without `_NT_SYMBOL_PATH`,
   `_NT_ALT_SYMBOL_PATH` and `DEBUGINFOD_URLS`, on a private desktop on Windows, and runs initialize, launch,
   setBreakpoints, configurationDone, the stop, stackTrace, threads, continue, exited and disconnect on the
   fixture debuggee (`tests/sdk/fixtures/lldb_debuggee`, built with the test toolchain: a PDB from lld-link on
   Windows, DWARF on Linux). It skips with a message when the artifact is not installed.
   `tools/s10_dap_spike.py` is the spike's driver (an MSVC `cl /Zi /Od` build on Windows, `clang++ -g` on
   Linux); `tests/tools/test_build_lldb.py` checks the recipes parse and their verify mode refuses a bad tree.

   The debuggee fixture bypasses the compiler launcher because its breakpoints require debug records naming the
   current checkout. On Windows a cached object from another worktree produced a PDB naming that worktree, so
   LLDB could not bind the current source path. The test requires the adapter to verify each breakpoint. This
   is not confined to the fixture: an application object was also observed with another worktree's source path.
   Correct source identity for cached debug builds needs a separate toolchain follow-up.

## Measurements
REF-W1 (Windows 11) and WSL (Ubuntu 24.04, 6 GB), 2026-11-02.

- **Build.** Windows: 4337 Ninja steps in 12.9 minutes (-j6, BelowNormal), 14 minutes for the whole recipe;
  1.4 GB build tree and 394 MB of unpacked sources, both deleted. WSL: 4346 steps, 19 minutes (-j4, one link
  job, nice 10). The source tarball is 179 MB.
- **Artifact.** Windows: 85.9 MB (`liblldb.dll` 83.5 MB, `lldb-dap.exe` 1.6 MB, `lldb.exe` 0.5 MB,
  `lldb-argdumper.exe` 0.3 MB); imports only system DLLs (advapi32, crypt32, dbghelp, kernel32, ntdll, ole32,
  oleaut32, psapi, rpcrt4, shell32, version, winhttp, ws2_32). Linux: 101 MB stripped (`liblldb.so.23.1.4`
  91.3 MB, `lldb-server` 11.9 MB, `lldb-dap` 1.4 MB); needs only libstdc++, libm, libgcc_s and libc, RUNPATH
  `$ORIGIN/../lib`. `lldb-dap --version` prints `LLVM version 23.1.4` and `lldb version 23.1.4`.
- **Spike S10** (`tools/s10_dap_spike.py`; the debuggee is `tests/sdk/fixtures/lldb_debuggee`, built with
  `cl.exe /std:c++17 /EHsc /Zi /Od /MT` and `link /DEBUG:FULL` on Windows, and with the pinned
  `clang++ -std=c++17 -g -O0` against libstdc++ 14 on Linux). Gated checks: Windows 46/46, Linux 44/44. R2a's
  Notepad++ is not built on this machine, so the spike ran on the fixture only.

| Step | Windows (MSVC, PDB, native reader) | Linux (clang, DWARF) |
|---|---|---|
| Launch with arguments and a cwd with a space | the program sees argc 3 and the cwd; exit code 42, then `terminated` | same |
| Line breakpoints in main.cpp and worker.cpp | bound (`verified`); stop reason `breakpoint` | same |
| stackTrace | names, files and lines, through `std::invoke`, `std::thread::_Invoke` and `thread_start` (worker) and `invoke_main`, `__scrt_common_main_seh` (main) to kernel32 and ntdll | the same through `std::__invoke_impl`, `std::thread::_State_impl::_M_run` and `start_thread`, and `__libc_start_main`, `_start` |
| threads (a second `std::thread`) | 3 (main, the worker, a loader pool thread) | 2 |
| int, pointer, array, struct locals | values and children | same |
| `std::string`, `std::wstring` | `"hello frost"`, `L"wide frost"` (children: the raw `_Mypair`) | `Summary Unavailable` with plain `-g` (libstdc++'s types are incomplete under clang's limited debug info); `"hello frost"` with `-fstandalone-debug` |
| `std::vector<int>`, `std::map<std::string, int>` | `size=5` with `[0]`..`[4]` = 2, 3, 5, 7, 11; `size=2` with two `std::pair` children | `size=0`, no children (Python formatters) |
| `std::unique_ptr<int>`, `std::optional<int>` | `99` (child `pointer`); ` Has Value=true ` (child `Value` = 5) | same |
| evaluate | arithmetic, members, `primes[2]`, `*boxed` and a call into the program (`debuggee::sumWithBias`) work; STL member calls (`primes.size()`, `greeting.size()`, `ranks.size()`) fail with "Couldn't look up symbols" (`ranks.size()` succeeded once in a probe); `maybe.value()` is ambiguous (PDB records no ref-qualifiers) | the same work, and so do the `size()` calls; `primes[2]` and `maybe.value()` fail with "Couldn't look up symbols" (inline members the program never calls are not emitted) |
| next, stepIn, stepOut | correct lines and functions, 1 ms each | same, at most 4 ms |
| Null dereference | stops, reason `exception`, "Exception 0xc0000005 ... Access violation writing location 0x00000000", top frame at the faulting line | stops, reason `exception`, "signal SIGSEGV: address not mapped to object (fault address=0x0)", same frame |
| Unhandled C++ exception | stops at the second chance, `Exception 0xe06d7363`, stack `RaiseException`, `_CxxThrowException`, `main` at the throw; no type or `what()` | stops on SIGABRT from `std::terminate`, with `__cxa_throw` and `main` at the throw in the stack |
| `std::abort()` | no stop: the fixture turns `_CALL_REPORTFAULT` off, so the CRT raises nothing and exits with 3; a function breakpoint on `abort` stops in `abort.cpp` | stops on SIGABRT; the function breakpoint works too |
| Timings | initialize 25 ms; first stop 10-30 ms after the launch response; six sessions in 5-8 s | initialize 7 ms; first stop 51 ms; six sessions in 1.8 s |

- **Client behaviour.** lldb-dap answers the first `threads` request after a launch from the list it cached at
  launch time (one thread on Windows), whenever that request comes. A client must send it right after
  configurationDone, as VS Code does; the test and the spike do. `launch`'s response arrives after
  configurationDone's. Function breakpoint stops report reason `breakpoint`, not `function breakpoint`.
- **Integration test.** `sdk_lldb_tests` passes on Windows (2/2, 1.1 s and 1.3 s, the lld-link PDB debuggee) and
  in WSL; the clang resource headers are not needed for the expressions above.

## Consequences
- U1's Windows debug step works with lldb-dap: breakpoints, stepping, the stack, threads, locals with the
  MSVC STL defaults, evaluation and crash stops all work on a cl.exe /Zi program, so M4 continues. The gaps
  (STL member calls in expressions, no C++ exception type, `abort()` without a stop) are within PLAN.md's
  promise of lldb-dap's default formatters and go to M6 (exception breakpoints) and M10 (DbgEng, natvis).
- On Linux, without Python, containers show `size=0` in U1, and `std::string` needs `-fstandalone-debug` (or
  libstdc++'s debug info) in programs built with clang. The options are Python 3.12 with SWIG for the Linux
  build (a new build and run-time dependency, an ADR), the user-side flag, or M6's formatters; the user decides.
- The lldb pins are per-file hashes of a local build, so a rebuild that does not reproduce them byte for byte
  fails the check. Bit-reproducibility has not been tested (deferred to the hardening phase); a different but
  correct rebuild needs a new pin and a note here.
- The engine adds 86 MB (Windows) and 101 MB (Linux) to the SDK, and later to the installers.
- Deferred to the hardening phase (ADR 0031): the no-network and listener tests for a debug session (PLAN.md M4),
  the DAP conformance and transcript tests, variable goldens, the R2a Notepad++ run, sanitizers and stress runs
  around the test, and a rebuild comparison.

## Alternatives considered
- **lldb-dap with Python.** On Windows it adds a Python DLL to ship for nothing (the MSVC STL formatters are
  C++). On Linux it would bring the libstdc++ container formatters, linked against Ubuntu 24.04's
  `libpython3.12` (the official package's 3.14 does not exist there); kept as the fallback if M6's own
  formatters slip (Consequences).
- **The official Linux package's lldb-dap with a matching libpython 3.14.** Ubuntu 24.04 does not ship it; a
  private copy is a second interpreter to pin and patch for one feature.
- **The repository's toolchain files for the LLVM build.** They set `CMAKE_SYSTEM_NAME`, which turns on LLVM's
  cross-build path (a NATIVE tablegen tree configured without the toolchain); passing the same pins directly
  avoids it.
- **The DIA SDK PDB reader.** Needs `msdia140.dll` from Visual Studio at run time and its registration; the
  native reader is LLDB's default in 23.1.4 and needs nothing.
- **A dynamic CRT (`/MD`).** Would need the matching VC runtime DLLs beside lldb-dap; `/MT` matches the official
  LLVM Windows release and keeps the runtime set to LLDB's own files.
