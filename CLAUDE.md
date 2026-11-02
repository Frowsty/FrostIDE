# FrostIDE agent manual

FrostIDE is a cross-platform IDE written in C++23, targeting independently implemented Rider feature and
workflow parity, full MSVC solution support, CMake, Lua, Unity and FrostEngine (ADR 0054). These are goals,
not claims of completed support. `PLAN.md` is the source of truth for scope, architecture, budgets and milestones; read the
relevant section before starting a slice. `docs/progress.md` is the hand-off log: read it first, update it last.

## Hard rules
- **No CI/CD of any kind.** No GitHub Actions, hosted or self-hosted runners, or cloud machines. Everything is
  built and verified locally on this Windows 11 machine and in WSL (Ubuntu 24.04). Never add `.github/workflows`.
- **Supported platforms:** Windows 11 x64 and Linux x64. macOS is unsupported until a test leg exists; keep
  platform seams portable but never claim macOS works.
- **Compilers for our code:** Clang only (clang-cl + MSVC STL 14.44 on Windows, clang + libstdc++ 14 on Linux).
  cl.exe is a test oracle and a target for user projects, never a compiler for FrostIDE itself.
- **Language:** C++23 compiled in C++26 mode. Allowed C++26 features: pack indexing, `_` placeholders,
  `= delete("reason")`, user `static_assert` messages, attributes on structured bindings. Banned: reflection,
  contracts syntax, expansion statements, `#embed`, std::execution, C++26 library headers, named modules.
  Missing library features go through `frost/compat` (PLAN.md section 2.1).
- **Layering:** only `src/ui` and `src/app` may use Qt. `FrostLayers.cmake` fails configure on violations.
- **Errors:** fallible APIs return `[[nodiscard]] std::expected<T, frost::Error>`; no exceptions across modules.
- **Windows APIs:** wide (`...W`) APIs only; child processes only through `IProcessLauncher` with absolute paths.
- **Clean room:** never consult or copy GPL or proprietary IDE sources (Qt Creator, KDDockWidgets, Zed's GPL
  crates, JetBrains code, Visual Studio). Borrowed algorithms cite their MIT/Apache/BSD source in a comment.
- **Licences:** every new dependency needs an SPDX allowlist check (`tools/licensecheck.py`) and an ADR.

## Documentation convention (PLAN.md section 6.6)
- Code is documented with ordinary comments. C++/C#: `//` for notes up to three lines, `/* */` for longer
  explanations. CMake, PowerShell, bash, Python, TOML and YAML: `#` lines.
- Every source file starts with an `SPDX-License-Identifier: Apache-2.0` line and a header comment saying what
  the file is for. Files without comment syntax (JSON, fixtures, goldens, binaries) are covered by `REUSE.toml`.
- Public declarations document behaviour, ownership/lifetime, thread safety, preconditions, complexity and
  errors; budgeted code cites its budget (`// Budget: B5.`). TODOs read `// TODO(M3): ...`.
- Separate Markdown is limited to this file, PLAN.md, README/LICENSE/NOTICE/SECURITY/CONTRIBUTING, ADRs,
  protocol notes and the progress log. Markdown never replaces code comments.

## Environment
- Toolchain lives in `E:\sdk` (pinned in `sdk/sdk.lock.json`, installed by `tools/bootstrap.ps1`). In PowerShell,
  dot-source `tools\env.ps1` to put it on PATH. In WSL, `source ~/sdk/env.sh` (installed by
  `tools/bootstrap-wsl.sh`; apt packages via `wsl -d Ubuntu-24.04 -u root -- bash tools/bootstrap-wsl.sh --system`).
- From Git Bash, prefix `wsl.exe` calls with `MSYS_NO_PATHCONV=1` so `/mnt/...` paths are not rewritten.
- Linux builds run on ext4 (`~/frost-src`, synced by `tools/verify.sh`), never directly on `/mnt/e`.

## Definition of done
1. `tools\verify.ps1 -Quick` passes on Windows: build, unit and integration tests, frost-lint, licence check.
2. `tools/verify.sh` passes in WSL (it rsyncs the tree to ext4, builds and runs the tests).
3. New behaviour has tests; nothing is weakened (tests, budgets, warnings) without an ADR and user approval.
4. The commit message ends with a verification footer, e.g. `Verified: win 412/412, wsl 398/398, lint ok`,
   followed by the co-author line.

**Feature-first phase (ADR 0031, user decision 2026-11-01), until U1:** items 1-4 apply with simple functional
tests only. Design critics, independent reviews, sanitizers, fuzzing, stress runs, perf gates, goldens and `-Full`
are deferred to the hardening phase; each slice lists what it deferred. Kill every process tree you start.

**Review update (user decision 2026-11-02, ADR 0054):** use GPT-6.1 Terra implementation agents and GPT-6 Astra
verification. Independent review is required again for these changes; the remaining feature-first deferrals
stand. Give agents exclusive files or separate worktrees, address review blockers, then verify the combined
tree before merging. Missing functionality cannot be relabelled as deferred hardening.

## Working agreement (PLAN.md section 6.7)
- Standing permission (D23): commit, push and merge feature branches after verification and independent review.
- One branch per vertical slice (`m0/sched`, `m1/piece-tree`, ...). Merge to `main` only with green verification.
- Code touching core, sched, text, storage or security gets an independent review before merge (in the
  feature-first phase, ADR 0031, that review moves to the hardening phase after U1).
- Update `docs/progress.md` (done, next, blockers, questions) at the end of every session.
- Stop and ask the user about legal questions, costs, scope changes or weakening any test or budget.
