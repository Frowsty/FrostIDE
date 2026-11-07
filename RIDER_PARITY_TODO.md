# FrostIDE: Rider parity backlog (superseded)

This file used to say that every Rider parity item was done. That was wrong. An audit of the code on
2026-11-07 ([ADR 0077](docs/adr/0077-parity-roadmap-v2.md)) showed what the claim covered:
- **Fabricated results.** Some features showed results that no tool produced.
- **Unproved rows.** Only 1 of the 191 feature-matrix rows had an acceptance test.

**Where the backlog lives now:**
- **Plan.** [PLAN.md](PLAN.md), section 7 "Roadmap v2", sets the order: C++ (MSVC and CMake), then Lua, then
  FrostEngine, then the later tiers.
- **Epics.** PLAN.md Appendix C lists every epic, with acceptance tests that run real tools.
- **Evidence.** [`tests/features/matrix.json`](tests/features/matrix.json) records each row's status and the
  epics that deliver it. `tools/featurecheck.py` checks it.
- **Harm removal.** The harm register in [docs/progress.md](docs/progress.md) tracks each fabricated feature
  until it is removed or guarded (FND-3).

Parity is measured only by green matrix rows, each proved by an acceptance test against a real tool
([ADR 0078](docs/adr/0078-definition-of-done-v2.md)).
