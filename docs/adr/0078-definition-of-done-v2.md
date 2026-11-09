<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0078: Definition of done v2 (real tools, evidence, independent review)

- Status: accepted
- Date: 2026-11-07
- Plan reference: PLAN.md sections 6.4 and 6.7, section 7 "Roadmap v2"; CLAUDE.md "Definition of done"; replaces
  ADR 0031; ADR 0027 decision 9 (tidy and TSan back on by default in hardening); ADR 0077

## Context
In the feature-first phase (ADR 0031), one agent built each slice. The orchestrator then read the agent's own summary
as the review. The audit of 2026-11-07 (ADR 0077) shows what that let through:
- **Fabricated results.** The Unit Tests window invented outcomes and a test asserted them. NuGet and the IL view were
  constants. Code Vision counted usages by text match.
- **Unverified commits.** 24 of the 25 commits after 1c29790 had no `Verified:` footer, and HEAD had 5 deterministic
  failures, including `tools.licensecheck`, which `-Quick` runs.
- **Untested integration claims.** Every test that needs a real FrostEngine, Unity, LuaLS or C# toolchain skipped by
  default, so green counts said nothing about those integrations.
- **A collapsed test ratio.** Commit 1c29790 added 9,372 source lines and 196 test lines.

The user ended the phase on 2026-11-07 by accepting the roadmap's recommendations (PLAN.md section 9).

## Decision
1. **Truth rule.** The product shows a result only when it comes from a real tool's output: a process stream started
   through `IProcessLauncher`, or an LSP or DAP message. When a tool is missing, the user sees "tool unavailable",
   never placeholder data.
   - FND-5 makes this structural. A frost-lint rule allows result types (test outcomes, packages, usage counts,
     IL/disassembly, diagnostics) to be built only in allowlisted parsers.
   - Every feature that shows tool results has a tool-ablation test. With the tool removed, no result appears.
2. **Real-tool acceptance.** A slice's acceptance tests drive the pinned real tools: clangd 23.1.4, cmake and ninja,
   MSBuild found with vswhere, lldb-dap 23.1.4 or DbgEng, LuaLS 3.19.1, and FrostEditor built from `engine.lock`.
   - Expected values come from oracles such as `cmake --list-presets`, `ctest -N`, `--gtest_list_tests`,
     `doctest --list-test-cases`, MSBuild evaluation, clangd references or `lua-language-server --check`. Hard-coded
     counts are not allowed.
   - Unit tests with doubles are still welcome. They do not replace the real-tool test.
3. **Verify tiers.**
   - **`-Quick`** is the per-slice gate on Windows and WSL. Once FND-6 (R1) provisions the pinned tools and tiny
     fixtures, a skipped real-tool test fails `-Quick`. Until then, every skip is listed in the slice's report.
   - **`-Corpus`** (FND-6b, R1) runs the pinned FrostEngine clone, R2a and the synthetic MSVC solution. It is required
     before a domain merge and at every milestone exit.
   - **`-Full`** runs the Release, ASan and fuzz legs. It is required at R5 (FND-14) and at every milestone exit from
     R5 on, and at the R16 release gate.
4. **Independent review for every slice.** This restores CLAUDE.md's rule and widens it from core code to all code.
   - The reviewer is a separate agent that did not write the change.
   - It re-runs the tests itself and reads the diff against the truth rule, the hard rules and the slice's acceptance
     list.
   - Blockers are fixed before merge, and the combined tree is verified before it reaches `main`.
5. **The verification footer is mandatory.** Every commit on `main` ends with the real summary line of `verify.ps1`
   and `verify.sh` (`Verified: win ..., wsl ..., lint ok`). Amended 2026-11-09 (user decision): no Co-Authored-By
   or other attribution line follows it, and the history was rewritten to remove the earlier ones. FND-2 adds a local
   `commit-msg` hook that rejects a footer whose verify-stamp does not match the committed tree hash. There is still
   no CI (D24).
   - After a passing run, `verify.ps1` and a standalone `verify.sh` record their summary line under the verified
     working tree's hash (`tools/hooks/verify_stamp.py`, in `<git common dir>/frost/verify-stamps`).
   - On `main` the hook (`tools/hooks/commit-msg`) requires a `Verified:` footer and a stamp for the tree being
     committed, and every footer item must be in that stamp.
   - Install it once per clone with `git config core.hooksPath tools/hooks`. Merge to `main` with `--no-ff` after
     verifying the merged tree, because a fast-forward runs no hook.
6. **Test discipline.**
   - Tests are not weakened or deleted without an ADR and the user's approval. One exception: the user approved
     replacing tests that assert fabricated behaviour. Each such test is replaced by a real-tool test or a "feature
     unavailable" test and listed in the harm register.
   - FND-5 adds a ratchet on the ratio of source lines to test lines per slice (`frost_lint.py ratchet`, a check
     of both verify scripts). A slice runs from the merge base with `main` to the working tree. When its code
     under `src/` and `integrations/` grows by 200 or more net lines, it needs at least 20 added test lines per
     100 of them under `tests/`; fixtures, goldens and corpora do not count.
   - Calibration: the reviewed M0-M5 slices ran 19 to 220 test lines per 100. The audited feature-first commits
     ran 0 to 16 per 100.
7. **Stall policy.** The budgets in section 4 stay the targets and become gates at R16 (REL-1).

   | UI-thread stall | Until R16 | Where |
   |---|---|---|
   | 50 ms or more | Recorded: a freeze report with the stalled stack, as today | Every build |
   | Over 250 ms | Fails the gate (interim) | The milestone journeys (FND-9a for R0, FND-9b for R3) |
   | Over the section 4 budget | Tracked against the target | Perf suites; a gate from R16 (REL-1) |
8. **Milestone exit.** A milestone exits only when all of these pass:
   - `-Quick` on both platforms and `-Corpus`.
   - The milestone's automated journeys (j1_cmake, j2_msvc, j3_frostengine_lua) driven through the test-only
     automation endpoint (FND-7a).
   - The independent review.
   - From R2 on, a UAT-1 journey the user runs and records as pass/fail in `docs/progress.md`.
   - Explicit user sign-off.
9. **Kept from ADR 0031**, because these were never relaxed:
   - Clang only, FrostLayers, `std::expected`, wide APIs and `IProcessLauncher`.
   - The clean room, and a licence check plus an ADR for each new dependency.
   - SPDX lines and comments, and no CI.
   - Killing every process tree a test or agent starts.
10. **Tidy and TSan.** `-Tidy` and `-TsanQuick` stay opt-in until FND-14 (R5) has cleared the backlog they would
    report today. At that point they return to default-on (ADR 0027 decision 9). Slices that touch sched, io or
    storage run `-TsanQuick` now.

## Consequences
- Slices take longer: an implementer, a reviewer and fix rounds, in place of one agent. Per ADR 0077 the scarce
  resource is verified throughput, not raw output.
- Features without a real tool behind them disappear from the UI until they are real. This is visible to the user and
  intended (FND-3, FND-4).
- verify.ps1 and verify.sh gain `-Corpus` (FND-6b). The hook (FND-2), the lint and ratchet (FND-5) and the
  skip-is-failure mode (FND-6) land in R0 and R1. Until each lands, its rule is applied by the reviewer by hand.

## Alternatives considered
- **Keep feature-first until U1.** Rejected by the user. It produced the fabricated layer.
- **Independent review only for core, sched, text, storage and security code** (the pre-ADR-0031 rule). Rejected:
  every fabrication was in app and UI code.
- **CI to enforce footers.** Ruled out by D24. A local hook enforces them instead.
