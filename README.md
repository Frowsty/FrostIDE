# FrostIDE

FrostIDE is a native C++23 and Qt 6 IDE for Windows and Linux built to be a complete replacement for JetBrains Rider—without the JVM overhead or background memory bloat.

The goal is straightforward: support every workflow you rely on in **JetBrains Rider** for `.sln`, `.slnx`, `.vcxproj`, and `.csproj` development, layer in the first-class **CMake** project tooling you normally only get in **CLion**, and ship with bespoke integration for **FrostEngine** (alongside Unity).

---

## Why FrostIDE?

If you work across C++, C#, and game engines, you usually end up juggling Rider for `.sln` / `.csproj` / Unreal / Unity work and CLion or Visual Studio when touching raw CMake or native toolchains. FrostIDE puts all of those workflows into one fast, native binary:

- **Full Rider replacement for MSBuild & .NET solutions**: Open `.sln` and `.slnx` solutions directly. Manage Solution Folders, `.vcxproj` and `.vcxproj.filters` virtual filters, physical directory views, project references, NuGet packages, per-configuration properties (`Debug|x64`, `Release|x64`, custom platforms), and startup run/debug profiles from the header bar.
- **CLion-style CMake project support**: Open any folder containing a `CMakeLists.txt` or `CMakePresets.json` without generating a Visual Studio solution first. FrostIDE drives `cmake-file-api`, manages CMake profiles and build directories, streams compile commands straight to `clangd`, and rewrites `add_executable` / `add_library` / `target_sources` entries in `CMakeLists.txt` when you add, rename, move, or delete source files.
- **Bespoke FrostEngine integration**: Native support for `.frostproj` workspaces, engine IPC, runtime target control, and embedded Lua scripting (`lua-language-server`), plus a drop-in Unity Editor package (`com.frostide.editor`) with Play / Pause / Step controls wired straight into the main toolbar.

---

## Core Feature Set

### Solution Explorer & Project Management
- **`.sln` & `.slnx` read/write support**: Add existing projects, remove projects, unload/reload projects, and organize solutions with nested Solution Folders.
- **Three C++ hierarchy modes**: Switch on the fly between **Filters & Folders**, **Filters Only** (`.vcxproj.filters`), and **Folder Structure** (physical disk layout).
- **File & filter operations**: Cut, Copy, Paste, and Duplicate (`Ctrl+X`, `Ctrl+C`, `Ctrl+V`, `Ctrl+D`), drag-and-drop files between virtual filters or physical directories, batch-add existing files and folders, and auto-rebind open editor tabs and project XML includes on rename or move.
- **Project Properties & Configuration Manager**: Edit C++ compiler/linker flags, preprocessor definitions, include paths, C# target frameworks, project dependencies, and solution-wide configuration/platform matrices.

### Editor, Navigation & Refactoring
- **Piece-tree text buffer & Tree-sitter highlighting**: Instant startup on large files, incremental syntax highlighting, code folding, multi-caret editing, rectangular selection, and split editor groups.
- **Language servers**: Out-of-the-box integration with `clangd` (C/C++), Roslyn / `csharp-ls` (C#), and `lua-language-server` (Lua).
- **Rider navigation muscle memory**:
  - **Search Everywhere** (`Shift, Shift`) with All / Classes / Files / Symbols / Actions tabs
  - **Go to File** (`Ctrl+Shift+N`), **Go to Symbol** (`Ctrl+Alt+Shift+N`), **Find Action** (`Ctrl+Shift+A`)
  - **Find Usages** (`Alt+F7`), **Structure Tool Window** (`Alt+7`), **Hierarchy View**
  - **Refactorings**: Rename (`F2` / `Shift+F6`), Extract Method/Variable, Change Signature, Safe Delete, and `Alt+Enter` quick-fixes / context actions
  - **Local History**: Built-in SQLite-backed local history for every saved file, independent of Git

### Native & Managed Debugging
- **Dual DAP backends**: Debug native C/C++ binaries via `lldb-dap` and .NET assemblies via `netcoredbg` through a unified Debug tool window (`Alt+5`).
- **Inspection & control**: Conditional breakpoints, hit counts, logpoints, call stack frame switching, local/watch variable trees, expression evaluation, memory hex view, and register inspection.

### Version Control (Git)
- **Header & status bar branch widget**: Switch, create, merge, rebase, rename, or delete branches and run Fetch / Pull / Push / Stash directly from the window header bar.
- **Commit & Git Log tool windows**: Stage/unstage files or individual hunks, amend commits, browse the commit graph (`Alt+9`), inspect side-by-side diffs, run file blame/annotations, and resolve 3-way merge conflicts.

---

## Building from Source

FrostIDE builds with `clang-cl` on Windows and `clang++` on Linux using CMake presets and Ninja.

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

Full local verification scripts are available in `tools\verify.ps1` (Windows) and `tools/verify.sh` (Linux).

---

## Repository Layout

| Directory | Description |
| :--- | :--- |
| `src/core`, `src/io`, `src/sched` | Error handling, atomic/safe file I/O, filesystem watcher, and work-stealing thread pool |
| `src/text`, `src/syntax`, `src/document` | Piece-tree text buffer, Tree-sitter syntax engine, save manager, and hot-exit journaling |
| `src/project`, `src/build` | `.sln`, `.slnx`, `.vcxproj`, `.csproj`, and CMake (`cmake-file-api` / presets) models and MSBuild/CMake drivers |
| `src/lang`, `src/debug`, `src/vcs`, `src/term` | LSP clients (`clangd`, C#, Lua), DAP debugger client, Git runner/service, and ConPTY/PTY terminal |
| `src/ui/*` | Custom frameless Rider shell, editor view, tool windows, docking layout, and dialogs |
| `src/app` | Workbench composition root, FrostEngine & Unity integrations, launcher window, and `frost` executable |
| `integrations/unity` | `com.frostide.editor` Unity package for external script editor discovery and Play-mode IPC |

---

## Unity Editor Bridge

To use FrostIDE with Unity:
1. Open Unity's **Package Manager**, click **+ > Add package from disk...**, and select `integrations/unity/com.frostide.editor/package.json` (or `unity/com.frostide.editor/package.json` in a packaged build).
2. In **Edit > Preferences > External Tools**, select `frost.exe` (or set the `FROSTIDE_EXECUTABLE` environment variable).

---

## License

Licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for third-party attributions.
