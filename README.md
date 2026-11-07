# FrostIDE

FrostIDE is a native C++23 and Qt 6 IDE for Windows 11 and Linux, in early development. Its goal is the
workflows of JetBrains Rider and CLion, implemented independently and without the JVM. The targets are MSVC
solutions (`.sln`, `.slnx`, `.vcxproj`), CMake projects, Lua and the FrostEngine game engine. These are goals,
not claims of finished support.

---

## Where it stands

An audit of the code on 2026-11-07 ([ADR 0077](docs/adr/0077-parity-roadmap-v2.md)) set the baseline:

- **Feature matrix.** Of the 191 rows in [`tests/features/matrix.json`](tests/features/matrix.json), 1 is
  green (proved by an acceptance test), 40 are in progress and 150 are planned.
- **Estimated readiness.** About 22% of CLion and 13% of Rider.
- **Order of work.** C++ comes first: MSVC solutions and CMake on par with CLion. Then Lua, then
  FrostEngine integration, then the later tiers: C#/.NET, Unity, Unreal and other languages.

The plan is the R-ladder in [PLAN.md](PLAN.md), section 7 "Roadmap v2", with every epic in Appendix C.

**What is solid.** The audit found these parts real and tested:
- The piece-tree text engine and undo tree.
- The scheduler and storage.
- The git core, which drives the git CLI.
- The ConPTY and libvterm terminal.
- The MSBuild compile-flag bridge, and the CMake preset and File API readers.
- The LSP plumbing (clangd), and crash and freeze diagnostics.

**What is being removed or disabled** (milestone R0, "Stabilise and honest baseline"). The audit found
features that showed results no tool produced:
- Unit Tests window results.
- The NuGet package list.
- The IL view.
- Decompiled sources written into the workspace.
- Text-based refactorings.
- Code Vision usage counts.

They are being removed, or shown as unavailable with the epic that will deliver them. The harm register in
[docs/progress.md](docs/progress.md) tracks each one.

**The truth rule** ([ADR 0078](docs/adr/0078-definition-of-done-v2.md)). The IDE shows a result only when it
came from a real tool's output: a process started through `IProcessLauncher`, or an LSP or DAP message. A
missing tool shows "tool unavailable", never invented data. `frost-lint` checks this in the code: result
types may be built only in allowlisted parsers, and string literals may not imitate tool output. The
independent review of each change checks what a lint cannot.

---

## Building from source

FrostIDE builds with `clang-cl` on Windows and `clang++` on Linux, using CMake presets and Ninja.

### Windows (PowerShell)

```powershell
powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1
. .\tools\env.ps1
cmake --preset win-clangcl
cmake --build build/win-clangcl --config Release --target frost
```

To run the test suite on Windows:

```powershell
ctest --preset win-clangcl-debug --output-on-failure
```

### Linux / WSL

```bash
bash tools/bootstrap-wsl.sh
source ~/sdk/env.sh
cmake --preset linux-clang
cmake --build --preset linux-clang-debug
ctest --preset linux-clang-debug --output-on-failure
```

### Verification

There is no CI. Each change is verified locally with `tools\verify.ps1` (Windows, plus the WSL leg through
`tools/verify.sh`). The scripts print a summary line for the commit's `Verified:` footer, and after a
passing run they record a verify-stamp tied to the tree they checked.

A local `commit-msg` hook rejects a commit on `main` whose footer has no matching stamp. Install it once per
clone, from the repository root:

```bash
git config core.hooksPath tools/hooks
```

---

## Repository layout

| Directory | Description |
| :--- | :--- |
| `src/core`, `src/io`, `src/sched` | Error handling, atomic and safe file I/O, file watching and the work-stealing thread pool |
| `src/text`, `src/syntax`, `src/document` | Piece-tree text buffer, Tree-sitter syntax engine, save manager and hot-exit journaling |
| `src/project`, `src/build` | `.sln`, `.slnx`, `.vcxproj`, `.csproj` and CMake (File API, presets) models, and MSBuild and CMake drivers |
| `src/lang`, `src/debug`, `src/vcs`, `src/term` | LSP clients, the DAP client, the git runner and service, and the ConPTY/PTY terminal |
| `src/ui/*` | Shell, editor view, tool windows, docking layout and dialogs |
| `src/app` | Workbench composition root, engine integrations, launcher window and the `frost` executable |
| `integrations/unity` | `com.frostide.editor` Unity package for external script editor discovery (later tier, R18) |
| `tools` | Bootstrap, verification, `frost-lint`, `featurecheck` and the git hooks |

---

## Unity editor bridge

Unity support is a later tier (R18 in the roadmap). The package that exists today opens scripts in FrostIDE:
1. Open Unity's **Package Manager**, click **+ > Add package from disk...**, and select
   `integrations/unity/com.frostide.editor/package.json` (or `unity/com.frostide.editor/package.json` in a
   packaged build).
2. In **Edit > Preferences > External Tools**, select `frost.exe` (or set the `FROSTIDE_EXECUTABLE`
   environment variable).

---

## License

Licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for third-party attributions.
