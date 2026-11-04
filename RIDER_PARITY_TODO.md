# FrostIDE — 1:1 JetBrains Rider Parity Backlog

This file tracks all remaining features required for complete 1:1 JetBrains Rider parity.
**Workflow Rule**: As each item or sub-feature is completed, verified, and committed, **delete it from this file**.

---

## 1. Unit Testing Tool Window & Gutter Test Runner
- **Unit Tests Tool Window (`Alt+8`)**:
  - Discover and run **.NET tests** (`dotnet test` — xUnit, NUnit, MSTest) and **C++ tests** (`ctest` / GoogleTest `--gtest_list_tests` / Catch2).
  - Hierarchical test tree (`Solution -> Project -> Suite/Fixture -> Test`) with live Pass/Fail/Skipped/Running icons and execution duration (`ms`).
  - Per-test output and stack trace pane with clickable `file:line` navigation links.
  - Toolbar actions: **Run All**, **Run Failed**, **Repeat Until Failure**, **Debug Selected Test**, and **Stop**.
- **Editor Gutter Test Icons**:
  - Play/Debug gutter icons next to `TEST()`, `TEST_F()`, `[Fact]`, `[Theory]`, `[Test]`, and `main()` / `Main()` directly in the editor gutter.

---

## 2. Editor Inlay Hints, Code Vision Lenses, & Inline Debugger Values
- **LSP Inlay Hints (`textDocument/inlayHint`)**:
  - Render inline parameter name hints (`foo(count: 42)`) and deduced type hints (`auto` -> `: const MyType&`, `var` -> `: List<string>`) from `clangd` and `csharp-ls` in `EditorView`.
- **Code Vision Lenses**:
  - Clickable inline lenses above classes/methods/structs showing **`N usages`**, **`M implementations`**, and **last Git author**.
- **Inline Debugger Variable Values & Hover DataTips**:
  - While paused at a breakpoint, render current local variable and parameter values in muted text at the end of executed source lines in `EditorView`.
  - Rich debugger hover tooltip (`DataTip`) to inspect and expand variable members on mouse hover.

---

## 3. Top Titlebar Run/Debug Configuration & Startup Project Widget
- **Titlebar Run/Debug Toolbar Widget (`ShellHeaderBar` / `MainWindow`)**:
  - **Startup Project / Target Selector**: Dropdown in the custom window header bar to switch the active startup project/target or open `Edit Configurations...`.
  - **Build Configuration & Platform Selector**: Dropdown in the custom window header bar to switch active `Configuration | Platform` (e.g. `Debug | x64`, `Release | x64`) or open `Configuration Manager...`.
  - **Action Buttons in Titlebar**:
    - **Build** (`hammer` icon)
    - **Run** (`play` icon)
    - **Debug** (`bug` icon)
    - **Stop** (`stop` icon, enabled while a build, run, or debug session is active)

---

## 4. `Attach to Process...` (`Ctrl+Alt+P`) & Compound Launch Configurations
- **Attach to Process Dialog (`Run > Attach to Process...`, `Ctrl+Alt+P`)**:
  - Live filterable list of running local processes (PID, executable name, window title, native vs. .NET runtime detection).
  - Attach `lldb-dap` (native C++) or `netcoredbg` (.NET) to the selected process PID.
- **Compound Run Configurations**:
  - Launch multiple project configurations simultaneously in one click (e.g., Server + Client).

---

## 5. Solution Explorer & File System Quality-of-Life Parity
- **5.1 Copy / Cut / Paste / Duplicate (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+D`)**:
  - Support `Copy`, `Cut`, `Paste`, and `Duplicate...` for files and folders in both `SolutionExplorer` and `FileSystemView` (via keyboard shortcuts and context menu).
  - Copy/move physical files and directories on disk, rebind open editor tabs on cut-paste, and automatically add pasted/duplicated files to the target `.vcxproj` / `.csproj` project when performed in `SolutionExplorer`.
- **5.2 Drag-and-Drop Physical File & Folder Move**:
  - Support dragging and dropping files/folders between directories in `FileSystemView` and in `SolutionExplorer` (`Folder Structure` mode), moving them on disk, updating `.vcxproj` relative paths, and rebinding open documents.
- **5.3 Solution Folders (`.sln` & `.slnx`)**:
  - Support `Add > New Solution Folder...`, `Rename Solution Folder...`, `Remove Solution Folder`, and moving projects into Solution Folders (via `Move to Solution Folder` menu and drag-and-drop), persisted to `.sln` (`2150E333-8FDC-42A3-9474-1A3956D46DE8` + `NestedProjects`) and `.slnx` (`<Folder>`).
- **5.4 Unload Project / Reload Project**:
  - Support right-clicking a project in `SolutionExplorer` to **Unload Project** and **Reload Project**, marking unloaded projects as `(unloaded)` in the solution tree and persisting the unloaded state.

---

## 6. Decompiler / Metadata-as-Source for External .NET Assemblies & C++ Symbols
- **External Symbol Decompilation / Metadata-as-Source**:
  - When invoking `Go to Declaration` (`Ctrl+B` / `F12`) on a compiled .NET type (`System.*`, `UnityEngine.*`, NuGet package types) without local source, generate/decompile a readable C# source view (`ilspycmd` / Roslyn metadata projection) in a read-only `[Decompiled]` tab.

---

## 7. Git Gutter Hunk Popup, 3-Way Conflict Resolver Dialog, & Status Bar Branch Widget
- **Interactive Gutter Change Markers**:
  - Clicking a modified/added/deleted line marker in the editor gutter opens an inline diff popup with **Rollback Hunk**, **Copy Original Text**, and **Previous/Next Change** navigation.
- **3-Way Merge Conflict Resolver Dialog (`VCS > Resolve Conflicts...`)**:
  - Dialog listing conflicted files during merge/rebase/stash-pop with **Accept Yours**, **Accept Theirs**, and **Merge...** (opening the 3-way `MergeView`).
- **Status Bar Git Branch & Ahead/Behind Widget**:
  - Clickable Git branch badge in the bottom status bar showing current branch and ahead/behind counts (`↑N ↓M`) and opening the Branches/Fetch/Pull/Push popup.

---

## 8. Docked NuGet Tool Window, IL/Assembly Viewer, & One-Click Self-Updater
- **Docked NuGet Tool Window**:
  - Dockable tool window with **Installed**, **Updates**, and **Browse** tabs, Prerelease filter, package details/README pane, Update All, and NuGet feed sources.
- **IL / Native Assembly Viewer Tool Window**:
  - Live side-by-side compiler disassembly output (`clang++ -S` for C++ or IL for C#) synced to the active editor caret.
- **One-Click Update Apply & Version Switcher in Launcher**:
  - Apply downloaded updates or switch between installed builds in `E:\FrostBuilds\beta\` directly from `LauncherWindow`.
