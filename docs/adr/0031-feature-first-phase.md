<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0031: Feature-first phase until U1

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 6.7 (working agreement); ADR 0029 (thin path, U1 = the M4 exit)

## Context
The user, on 2026-11-01 at about 22:30: "I think we have to significantly lessen the point of acceptance. I
want to focus on pumping features. Make sure we get everything in, no need for the hour and minute long tests,
you review. do simplet functional test and if it passes then good we merge. Once we have a functional IDE that
can be used that's when we start going over the code. Reviewing, fixing major bugs etc. I think that should save
TONS of time to get something usable out which can replace my current IDE ASAP". Then, before a PC restart:
"the new rules of pumping out a usable product and skipping the massive rigorous testing, reviewing etc and just
basic usage testing".

## Decision
1. From now until U1 (the M4 exit, ADR 0029) or until the user ends it, one agent builds each slice: the whole
   scope (no feature cuts), simple functional tests (happy path plus obvious edge cases, each fast), `verify.ps1
   -Quick` on Windows and WSL, one commit. The orchestrator reads the agent's summary as the review and merges
   slices in batches with one `-Quick` run per batch.
2. Not done in this phase: design critics, independent review agents and re-reviews, sanitizer builds, fuzzing,
   stress and differential runs, mutation checks, perf and budget gates, screenshot goldens, `-Full`. Each slice
   lists what it skipped ("deferred"); the lists are kept in `docs/progress.md`.
3. Still required, because they are cheap or legal: Clang only; FrostLayers layering; `std::expected` errors;
   wide Windows APIs; child processes through `IProcessLauncher`; the clean room; `licensecheck` plus a short ADR
   for each new dependency; SPDX lines, header comments and doc comments; no CI; existing tests, budgets and
   warnings are never weakened or deleted. Budgets stay the targets in PLAN.md; they are not gates in this phase.
4. This overrides, for the phase only, the `-Full` rule of PLAN.md 6.7 "Branches and PRs", the "Review" row of
   6.7 and the independent-review rule in CLAUDE.md.
5. Test-run hygiene (learnt the same evening): an agent kills every process tree it starts before it returns,
   leaves no background loop or measurement lock behind, and keeps hard-error boxes off the user's desktop.

## Consequences
- A slice takes one agent run instead of five to seven; a usable build comes days sooner.
- More bugs reach `main`. The hardening phase after U1 works through the deferred lists, gives core, sched, text,
  storage and security code merged in this phase its independent review, and runs `-Full`, the sanitizers and the
  budget gates.
- Slices reviewed before the switch (storage, diff-merge, encodings) keep their recorded findings; only functional,
  data-loss, security and legal findings are fixed now, the rest is deferred.

## Alternatives considered
- Keep the tiered process (slice2.js): about 3-4 days for M1 alone; rejected by the user.
- Drop tests entirely: rejected; simple functional tests cost minutes and keep `main` usable.
