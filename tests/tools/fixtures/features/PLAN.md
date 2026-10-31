# Mini plan for the featurecheck tests

### 6.1 Test pyramid
- **Feature matrix.** `frost-featurecheck` gates the matrix (M7 ≥30 frost, ≥2 CFG, ≥45%; M9 ≥45, ≥4, ≥60%; M13 ≥60, 5, ≥75%).

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
