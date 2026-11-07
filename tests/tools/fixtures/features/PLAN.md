# Mini plan for the featurecheck tests

**Milestone ladder.** The rows are in the order the milestones run.

| # | Milestone | Units | Ends around month |
|---|---|---|---|
| M0 | Bootstrap | 3–4 | 1 |
| M1 | Editor | 8–9 | 3–4 |
| M2 | Projects | 9–10 | 5–6 |
| M3 | Intelligence | 7–8 | 7–8 |
| M4 | Build | 6–7 | 9–10 |
| M5 | Git | 8–10 | 11–13 |
| M15 | Editor completion | 6–7 | 13–15 |
| M16 | ProjectHost | 9–11 | 15–18 |
| M17 | Intelligence completion | 5–6 | 17–20 |
| M6 | Debugger | 12–14 | 20–24 |
| M7 | Analysis | 10–12 | 23–27 |
| M8 | Claude C2 | 3–4 | 23–28 |
| M9 | Refactoring | 11–13 | 26–31 |
| M10 | Debugger v2 | 9–11 | 29–35 |
| M11 | UX parity | 9–11 | 31–38 |
| M12 | Claude C3 | 5–6 | 33–39 |
| M13 | Scale | 5–6 | 34–41 |
| M14 | v1.0 | 4–5 | 35–42 |
| — | Upgrade track | 9–12 | throughout |

---

### 6.1 Test pyramid
- **Feature matrix.** `frost-featurecheck` gates the matrix (M7 ≥30 frost, ≥2 CFG, ≥45%; M9 ≥45, ≥4, ≥60%; M13 ≥60, 5, ≥75%).

## 7. Roadmap

### Roadmap v2: the R-ladder

| # | Milestone | Units | Cumulative |
|---|---|---|---|
| R0 | Stabilise | 2.5 | 2.5 |
| R1 | Daily driver | 5.5 | 8 |

#### R0: Stabilise (2.5 units)
**Epics** (Appendix C): FND-1 Green HEAD; FND-5 Lint.

#### R1: Daily driver (5.5 units)
**Epics** (Appendix C): CMK-2 Presets; UX-3 Editor essentials.

## Appendix A: Feature matrix → milestone

| Milestone | Tier | Rows |
|---|---|---|
| M1 | P0 (3) | 28 File System view; 57 gutter (+M6); 250 soft wrap* |
| M2 | P1 (2) | 3 .sln/.slnx conversion (P2 in); 20 toolchain detection, v1 slice (WSL/Docker/remote: 1.x) |

**Totals.** P0: 3 (3). P1: 2.

**Infrastructure rows** (tracked in `matrix.json` with an I prefix; P0 unless marked P1):
- **M0:** I7 Windows long paths, UTF-8 code-page manifest.
- **M5:** I14 `IHttpClient` (+M6 for Linux); I15 portable packages, symbol archival, Collect Logs.
- **P1:** I18 Power Save Mode (M7).

**Epic mapping** (the fixture's).

| Epic | Rows |
|---|---|
| FND-5 | I14, I15, I18 |
| CMK-2 | 3, 20 |
| UX-3 | 57, 250 |

**Out of epic scope** (foundation): 28, I7.

## Appendix C: Roadmap v2 epic catalogue

### R0

- **FND-1 Green HEAD** (P0, 1u; after none). Fix the failing tests.
- **FND-5 Lint** (P0, 1.5u; after FND-1). The truth rule.
- **RUN-0 Remove fake runner** (merged, 0u; after none). Merged into FND-5.

### R1

- **CMK-2 Presets** (P0, 2.5u; after FND-1). Complete presets.
- **UX-3 Editor essentials** (P1, 3u; after FND-5, CMK-2). Every action works.
