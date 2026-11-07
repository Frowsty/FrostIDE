<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0077: Parity roadmap v2 (R-ladder) and the end of the feature-first phase

- Status: accepted
- Date: 2026-11-07
- Plan reference: PLAN.md "Plan at a glance", section 7 (roadmap, "Roadmap v2"), section 9 ("Decided 2026-11-07"),
  Appendix C (epic catalogue); supersedes the run order of ADR 0029 and ADR 0032 and ends ADR 0031; ADR 0054 (scope)
  stays; ADR 0078 (definition of done v2)

## Context
On 2026-11-07 a code-level audit of HEAD ec3d580 was made at the user's request. Eleven area analysts each had a
skeptic re-check their claims in the code, and an empirical leg built the tree and ran the suite. Findings:
- The foundation is real and well tested: the text engine and undo tree, `std::expected` errors, sched, storage, the
  git core, the ConPTY/libvterm terminal, the MSBuild compile-flag bridge, the CMake presets and File API readers, the
  LSP plumbing and crash and freeze diagnostics.
- The feature layer of 2026-11-04..06 (commit 1c29790 onwards, "complete 1:1 Rider parity") is thin and partly
  fabricated:
  - The Unit Tests window invents pass/fail and durations, and a test asserts the invented result.
  - NuGet is a constant list of 8 packages, and the IL viewer is a placeholder.
  - The decompiler writes synthetic source into the workspace.
  - Several refactorings are line-text edits that corrupt code.
  - Code Vision counts usages by text match.
- The repository's own matrix had 1 of 191 rows green, yet the README claimed "complete replacement".
- HEAD had 5 deterministic test failures. No commit after 1c29790 carried a `Verified:` footer.
- The user's logs show 145 UI stalls of at least 50 ms in 5 days. LuaLS was not bundled, so the installed build had
  no Lua intelligence.
- Estimated readiness: about 22% of CLion and 13% of Rider. Engine integration: Unity about 1.5/10, Unreal 0,
  FrostEngine about 2/10.

The user then set the priorities: "Highest priority is C++ and LUA with FrostEngine integration. The other engine
integrations and language support etc should come later", with CLion-class CMake support included in the goal.

The plan was drawn up as follows:
- Twelve domain planners each produced grounded epics with acceptance tests that run real tools.
- A completeness critic checked the epics against Rider, ReSharper C++, CLion and Rider for Unity.
- A feasibility critic attacked effort, dependencies and process risk.
- An architect sequenced the result.

The user accepted every recommendation on 2026-11-07 ("Yes to all recommendations, go ahead").

## Decision
1. **The R-ladder replaces section 7's run order.** It has 20 milestones, R0..R19, listed in PLAN.md section 7
   "Roadmap v2", and 226 epics catalogued in Appendix C. Milestone ids M0..M17 remain historical identities. Code
   markers (`TODO(Mn)`), the Appendix A matrix and featurecheck keep using them until FND-15 maps every epic to an
   M-id and an Appendix A row, and teaches featurecheck the R-ids. M6 "Preview 0.1" is no longer a gate; its content
   is split across R3, R5 and R9.
2. **Priority order:**
   1. C++, for MSVC solutions and CLion-class CMake.
   2. Lua.
   3. FrostEngine integration, aimed at Rider-for-Unity class.
   4. The later tiers: C#/.NET (R17), Unity (R18), Unreal and other languages (R19).

   Engine-aware Lua is interleaved with C++ work because the user's Lua is FrostEngine script code. Later tiers
   receive only harm removal until R16. Their position is re-decided after R16.
3. **Daily-driver points.** Only the current point's P0 epics are in flight; other work is limited to read-only spikes.
   - DD-1a, at the R1 exit (32.25u): FrostEngine C++ on its CMake presets (edit, navigate, build), plus Lua basics.
   - DD-1, at the R2 exit (47.25u): FrostEngine run, test and debug.
   - DD-2, at the R3 exit (79.75u): the user's MSVC work solution. It is checked by a journey the user runs and
     records as pass/fail only. The internal SDK is never committed.
4. **R0 comes first: stabilise and establish an honest baseline.**
   - It ends with HEAD green.
   - Fabricated features are removed or disabled under one harm register, kept in `docs/progress.md`.
   - It adds the anti-fabrication lint, makes the claims truthful, ends silent trust grants, bundles LuaLS, and
     fixes the git-clone and clangd double-spawn stalls.
   - No feature work builds on the fabricated layer.
5. **The feature-first phase (ADR 0031) ends today.** The definition of done is ADR 0078's.
6. **Estimate, in units, with no calendar dates.** About 402 units reach the C++/Lua/FrostEngine 1.0 at the R16 exit,
   and about 508 units cover everything. A unit is about one agent-assisted week. This replaces 138-168 units for
   a C++-only v1.0. Repository history shows that units overstate calendar time (M0's 3-4 units took about 1.5 days).
   The scarce resource is verified throughput. After four weeks the totals are re-baselined from green matrix rows
   per week.
7. **Resources** (user decisions, PLAN.md section 9):
   - At most 3-4 implementation agents with exclusive files or worktrees, plus one independent reviewer.
   - One heavy leg (an engine build or a full index) at a time.
   - Test corpora are pinned `git clone --no-hardlinks` copies under `E:\FrostBuilds\corpora`. They hold one
     FrostEngine SHA and preset (windows-debug), plus linux-debug on WSL ext4, and older SHAs are pruned
     automatically. A lint rule forbids the user's live `D:\dev\FrostEngine-AI-Claude` tree in tests and fixtures.
8. **FrostEngine-side work** (FEC-1, FER-1, FER-8) lands on FrostEngine `main`, verified with its own
   `Build.py --test` and `WslBuild.py --test`. FrostIDE's `engine.lock` pins only `main` SHAs. Commit a347410d, the
   orphaned Lua debug endpoint on no branch, is reference material only; FER-8 reimplements it on a shared engine
   hook.
9. **Release cadence.** Automatic beta updates are frozen until FND-11 (updater rollback, R5) passes. Until then betas
   ship as manual side-by-side zips.

## Measurements
Audit of 2026-11-07 at ec3d580 (REF-W1, win-clangcl Debug, sccache):
- Incremental build: 680 steps in 1 min 10 s.
- `ctest -LE '^stress$' -j 8 --timeout 300`: 3111 passed, 5 failed, 35 skipped and 1 disabled out of 3152, in
  5 min 15 s.
- `frost.exe --selftest`: 14/14 checks passed.
- tests/features/matrix.json: 1 green, 40 in progress and 150 planned rows out of 191.
- Commit 1c29790 added 9,372 source lines and 196 test lines.
- The user's `%LOCALAPPDATA%\FrostIDE` logs from 2026-11-02 to 11-06: 145 stall reports over 50 ms (mean 315 ms,
  maximum 16.6 s) and one minidump.

## Consequences
- The first months go to honesty, a real-tool harness and the FrostEngine and MSVC daily drivers, not to breadth.
  Visible "parity" surface shrinks in R0, because fabricated features are removed rather than polished.
- The matrix and featurecheck have to learn epic ids (FND-5, FND-15). Until then the R-ladder lives in prose and in
  Appendix C, and featurecheck keeps validating the M-ladder.
- Section 7's thin-path, W1 and feature-first paragraphs are kept as history and marked superseded.
- Follow-up work, tagged by R-milestone in Appendix C:
  - R0: FND-2 (verify-stamp hook), FND-5 (anti-fabrication lint), FND-15 (matrix mapping).
  - R1: FND-6 and FND-6b (real-tool fixtures and corpora).

## Alternatives considered
- **Keep the M-ladder and add the new scope to it.** Rejected. The M-ladder assumed a C++-only v1 and a working
  feature layer, and it would bury the user's C++ > Lua > FrostEngine order under M5-M14 breadth.
- **Polish the existing parity layer instead of removing it.** Rejected. Fabricated results are worse than missing
  features, and the audit found several of them corrupt code or write into the user's tree.
- **A hard 40-unit cap on DD-1** (feasibility critic). Rejected in its strict form: the real-tool harness and honest
  run, test and debug cannot be cut. DD-1a at 32.25 units keeps the intent.
- **Calendar dates in the roadmap** (completeness critic). Rejected until velocity is measured.
