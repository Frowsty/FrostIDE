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

## 4. `Attach to Process...` (`Ctrl+Alt+P`) & Compound Launch Configurations
- **Attach to Process Dialog (`Run > Attach to Process...`, `Ctrl+Alt+P`)**:
  - Live filterable list of running local processes (PID, executable name, window title, native vs. .NET runtime detection).
  - Attach `lldb-dap` (native C++) or `netcoredbg` (.NET) to the selected process PID.
- **Compound Run Configurations**:
  - Launch multiple project configurations simultaneously in one click (e.g., Server + Client).

---

## 6. Decompiler / Metadata-as-Source for External .NET Assemblies & C++ Symbols
- **External Symbol Decompilation / Metadata-as-Source**:
  - When invoking `Go to Declaration` (`Ctrl+B` / `F12`) on a compiled .NET type (`System.*`, `UnityEngine.*`, NuGet package types) without local source, generate/decompile a readable C# source view (`ilspycmd` / Roslyn metadata projection) in a read-only `[Decompiled]` tab.

---

## 8. Docked NuGet Tool Window, IL/Assembly Viewer, & One-Click Self-Updater
- **Docked NuGet Tool Window**:
  - Dockable tool window with **Installed**, **Updates**, and **Browse** tabs, Prerelease filter, package details/README pane, Update All, and NuGet feed sources.
- **IL / Native Assembly Viewer Tool Window**:
  - Live side-by-side compiler disassembly output (`clang++ -S` for C++ or IL for C#) synced to the active editor caret.
- **One-Click Update Apply & Version Switcher in Launcher**:
  - Apply downloaded updates or switch between installed builds in `E:\FrostBuilds\beta\` directly from `LauncherWindow`.
