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

