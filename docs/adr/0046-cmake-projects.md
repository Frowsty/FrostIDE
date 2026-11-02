<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0046: CMake projects (toolchains, profiles, configure, build)

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("CMake"), section 3.2 (src/project, src/build), section 3.5 (trust,
  launching); ADR 0011 (process launching), ADR 0021 (settings, yyjson), ADR 0035 (build service), ADR 0036
  (compile commands)

## Context
M2 opens a folder with a CMakeLists.txt the way CLion does: profiles, a background configure, a target tree
from the File API, and builds with diagnostics. The pieces must stay Qt-free, start children only through
`IProcessLauncher` with absolute paths, run nothing before trust, and look like `BuildService` so the W1 build
windows can bind to them. No new dependency: the presets and File API JSON go through yyjson (ADR 0021),
linked privately into frost::project.

## Decision
1. **Tool discovery.** cmake and ninja come from `tools.cmake.path` / `tools.ninja.path` (application-only
   `executes` keys), then the CMake and Ninja that Visual Studio bundles (instances from vswhere, newest
   first), then the absolute entries of PATH. A wrong settings path is an error, not a fallback. CMake 3.21 or
   later is required (presets v3, the File API objects used).
2. **MSVC environment.** On Windows, for every generator except Visual Studio ones, the environment comes
   from `vcvarsall.bat <arch>` (x64 by default; a preset's external architecture wins), run through the
   absolute `%SystemRoot%\System32\cmd.exe` because the launcher refuses batch files: `cmd /d /u /c set &&
   echo <marker> && call "<vcvarsall>" <arch> >nul && set`, each word its own argument, so the launcher's CRT
   quoting is what cmd sees. Paths with `" % ^ !` and unknown architectures are refused. The difference of
   the two `set` listings is applied to configure and build children. It is cached in memory and on disk per
   (instance, architecture) with the instance's installationVersion, and captured again when that changes.
3. **Presets vs profiles.** CMakePresets.json and CMakeUserPresets.json (version 2+, include, inherits,
   conditions, `${...}` / `$env{}` / `$penv{}` macros) are read natively. When a visible enabled configure
   preset exists, the profiles are the presets (each with its first build preset); otherwise the
   `cmake.profiles` setting's records; otherwise one Debug profile (Ninja, `cmake-build-debug`). Configure
   passes the resolved switches (`-S -B -G -D...`), not `--preset`, so what FrostIDE shows is what runs, and
   `CMAKE_EXPORT_COMPILE_COMMANDS=ON` always comes last. clangd reads compile_commands.json from the active
   profile's build directory.
4. **Trust.** Configure runs project code and a build runs its steps, so both need the folder trusted. An
   explicit Reload or build of an untrusted folder waits in NeedsTrust; automatic configures (open, file
   change) do nothing. Reading presets and an existing File API reply runs nothing and is allowed before
   trust. A build directory outside the folder is allowed only in an application-scope profile whose `root`
   is that folder; a preset or a project setting pointing outside makes the profile unusable. CMake's paths
   resolve `..` lexically as CMake does, on POSIX too, so `${sourceDir}/../x` is outside on every OS.
   `cmake.profiles` is an `executes` key, so its project-scope values wait for trust (ADR 0021).
5. **Build shape.** `CMakeService` mirrors `BuildService`: one job at a time on one service thread, the same
   `BuildState` pipeline, Save All before builds, `BuildOutputKind` lines and the resolved diagnostic index;
   configurations are profiles and projects are targets. Build is `cmake --build <dir> [--config c]
   [--target t] [-j n]`, Clean is `--target clean`, Rebuild is Clean then Build; a build without a current
   model configures first. Configure output is parsed for CMake's message blocks (resolved against the source
   directory), build output with the MSVC/clang-cl parser and a new GNU/Clang parser (resolved against the
   build directory). `BuildSession` takes a parser factory for this; MSBuild keeps its parser. A change to a
   CMakeLists.txt, a project `*.cmake` input or a presets file reconfigures after a 500 ms quiet period.

## Consequences
- The W1 workbench binds the CMake tool window and the Solution Explorer through frost::ui_cmake
  (`CMakeController`, `ICMakeBackend`, `CMakeTreeModel`); the build windows need a small adapter.
- Hardening (sanitizers, fuzzing the presets and File API readers, malicious-repository and symlink tests,
  perf of large replies) is deferred to the hardening phase (ADR 0031).
