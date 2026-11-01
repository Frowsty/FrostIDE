# ADR 0007: Spike S1, editor rendering at 5120x1440 @ 144 Hz and first frame

- Status: proposed. It needs the user's answers to the four questions below. The measurements are run 3 (the
  main suite, five interleaved rounds) and run 4 (a follow-up with interleaved trivial windows and one round
  covering the whole output), both on 2026-11-01. All probe validations passed in both runs. The desktop was the
  user's normal, busy desktop, not a quiet one.
- Date: 2026-11-01 (first drafted 2026-10-31)
- Plan reference: PLAN.md section 2 ("Editor rendering"), section 4 (B1, B2, B5, B6, B15), section 6.3 (session
  precheck), M0 spike S1, risk T10

## Context
PLAN.md ships the raster QPainter editor backend (QGlyphRun cache) and builds a QRhiWidget glyph-atlas backend only
if the S1 gate fails at 5120x1440 @ 144 Hz. The S1 gate is B6 (10 s fling: UI CPU per frame p99 at most 5 ms,
missed compositions at most 1 %, no content interval over 16.7 ms), plus B5 (keystroke to paint end, p50 3 ms and
p99 8 ms) and warm B1 (first frame, p50 150 ms and p99 250 ms). QRhiWidget composites the whole window, so the
backend has to be fixed before the first frame ships.

Raster Qt flushes through GDI into the DWM redirection surface and has no swap chain, so present-based tools see
nothing. S1 therefore needs backend-neutral probes, validated before their numbers count. B6's "missed
compositions" are measured here as refreshes in which the window showed no new content (Methods).

## Questions for the user
1. **Approve QRhi as M1's editor renderer on Windows?** That means the QRhiWidget glyph-atlas backend as the
   default on Windows, with raster kept as the golden oracle and the RDP/WARP fallback (Decisions 1 to 3). Raster
   fails B6's CPU criterion at 5120x1392 by its own cost in every round. QRhi passes it in all 10 rounds of runs 3
   and 4, using about a sixth of the budget (p99 0.86 ms against 5 ms). But it adds 216.9 ms to the first frame
   (the p50 difference), and starting it later moves that cost onto B2 and B15 instead (Consequences).
2. **How should B6's missed and interval criteria be gated?** What is known:
   - QRhi with text at the work area (5120x1392) missed 1.5–9.2 % of refreshes over 10 rounds, and its maximum
     content interval was within 16.7 ms in 6 of them. Its own CPU cost does not explain this: p99 under 1 ms,
     and it dropped at most 7 of 1,441 ticks in a run.
   - With the window covering the whole output (5120x1440, over the taskbar), one round: QRhi missed 1.25 % with
     text and 0.56 % with fill only, with a 13.9 ms maximum interval in both. Whether DWM flipped the swap chain
     directly there (independent flip) was not observed.
   - Trivial 320x48 windows interleaved on the same desktop missed more than the QRhi editor in the same rounds:
     medians 7.8 % (Qt raster) and 9.6 % (QRhi), against 5.5 % for QRhi with text and 4.4 % with fill only. So
     a trivial window is not a floor under a large window's misses, and where QRhi's work-area misses come from
     is not established.
   - The desktop was busy (Wallpaper Engine, Discord, 33–61 % system CPU). No quiet-desktop control was run.

   The options are:
   - (a) measure B6 on a quiet desktop (Wallpaper Engine and other composing software paused) and keep the
     absolute budget. Whether QRhi then passes at the work area is unmeasured.
   - (b) measure B6 with the window covering the whole output (B6's corpus is "5120x1440") and keep the absolute
     budget. That came closest to passing, but a maximised editor uses the work area.
   - (c) decide at M1, after a quiet-desktop control and PresentMon's present mode for QRhi (PLAN's cross-check,
     skipped in S1) show where the misses come from.

   This ADR proposes (c) and no budget change; any budget change needs your approval. The earlier draft's option
   of gating against a same-session trivial-window floor is withdrawn, because run 4 shows that such a window
   misses more than the editor.
3. **B1 warm fails today, before any editor work.** frost.exe's first frame is p50 264.9 ms against 150 ms, and
   the maximum of 20 launches (which nearest-rank p99 equals here) is 360.1 ms against 250 ms. Its window is
   first seen at p50 248.2 ms (polled). QRhi on the first-frame path would add 216.9 ms more. Should M1 keep the
   150/250 ms budget as the target (startup profiling, ETW markers, QRhi start-up off the first frame), or should
   B1 be re-baselined?
4. **When can a true cold B1 run happen?** The cold numbers here come from a proxy whose bias has an unknown
   direction (Caveats). budgets.json's cold conditions (after a reboot, or after a standby-list purge with
   RAMMap -Et) are not met. Either needs you: a reboot, with the first-frame steps run first after logon, or an
   elevated RAMMap -Et before each launch. Which do you prefer, and when?

## Decision (proposed)
1. **M1 builds the QRhiWidget glyph-atlas backend as the default editor renderer on Windows** (the PLAN.md
   contingency, risk T10). Raster fails S1 at 5120x1392 by its own cost, in every one of 5 rounds:
   - UI CPU p99 is 9.18 ms (median of 5 rounds) with full repaint, 7.38 ms with scroll-blit, and 6.29 ms even
     with fill only (no text). At the whole output, fill only, it was 7.18 ms (run 4, one round).
   - Raster drops 13–24 % of vblank ticks itself and misses 39–51 % of refreshes. Those misses are upper bounds,
     because its captures merged several presents 145–350 times per run (Caveats). If each merge hid one update,
     raster missed 16–37 %, still well above QRhi.

   QRhi with text uses p99 0.86 ms (median, run 3). It dropped at most 1 of 1,441 ticks per run in run 3 (3, or
   0.2 %, with fill only), and 7 in one round of run 4. It missed 1.5–4.9 % of refreshes in run 3 and 4.6–9.2 %
   in run 4.
2. **QRhi passes B6's CPU criterion; its missed and interval criteria are open.** Per criterion, over the 10
   rounds at the work area with text (runs 3 and 4):
   - CPU passes 10 of 10;
   - the content interval stays within 16.7 ms in 6 of 10 (13.9 ms), and is 20.8 ms in the other 4;
   - missed refreshes are within 1 % in 0 of 10.

   The misses do not come from QRhi's own CPU cost. That they come from the desktop is not shown: trivial windows
   on the same desktop missed more, and at the whole output QRhi missed 0.56–1.25 % (one round). How the missed
   and interval criteria are gated is question 2.
3. **QRhi's start-up must stay off the B1 path, and moving it must not just shift it onto B2 or B15.** In the same
   executable and window, the first frame is p50 474.6 ms with QRhi (20 launches) against 257.7 ms with raster
   (10 launches), 216.9 ms more. In-process, the window's first `show()` takes p50 367.4 ms instead of 157.5 ms,
   about 210 ms more. Pipeline creation for the spike's two trivial shaders, including runtime HLSL compilation,
   takes 6.5 ms. So the cost is the window's RHI and Direct3D 11 start-up, not these shaders. M1 measures ways to
   keep the cost out of the first frame before the backend ships (see Consequences).
4. **B1 warm fails independent of the backend** (question 3). M1 adds startup ETW markers and works the budget
   down.
5. **Raster stays the golden oracle and the fallback for RDP, WARP and remote sessions.** It is selected before
   the first frame. At 1280x720 it meets B6's CPU criterion (p99 2.49 ms) without dropping ticks, but it misses
   10–18 % of refreshes there, against 3–7 % for QRhi at the same size.
6. **The backend-neutral probes are the S1/B6 instrument** (`tests/perf/probes`, `frost::perf_probes`, layer
   diag):
   - Desktop Duplication with a frame marker for content intervals, missed refreshes and frame-to-screen latency;
   - DWM timing for the refresh period and a vblank anchor for the refresh grid;
   - the `ISessionState` precheck.

   They are validated on a GDI window and through both Qt backends. The validation includes QRhi's flip-model
   swap chain, both at 1280x720 and covering the whole output. The UI-CPU probe is validated by an injected spin.
   DWM's composition counters are not used (see Findings).
7. **B6's missed and interval results are reported with context, not against a floor.** The context is:
   - trivial windows interleaved with the editor runs: `s1_probe_validate` phase C (GDI), and `s1_editor` at
     320x48 with fill only, on both backends;
   - the number of merged captures in each run.

   The trivial windows show how disturbed the desktop was. They are not a lower bound: in run 4 they missed more
   than the QRhi editor did. So they never adjust a verdict. A backend's own fill-only run is not a floor either,
   because raster's fill-only frame already drops ticks through its own CPU cost.
8. **Linux keeps PLAN's default, raster, for now.** S1 measured nothing on Linux: the probes are Windows-only,
   and WSL has no display they could measure. The Linux renderer is chosen after an S1 leg on a Linux desktop
   measures both backends (M1).

## Measurements
**Conditions.**
- Run 3: 2026-11-01, 01:02:33 to 01:11:10.
  - `tests/perf/s1/run-s1.ps1` ran at bc20789 on Release builds: 47 steps in 8:29, exit 0. Every run was valid:
    no session change, no lost duplication and no capture failure.
  - Load before the suite: free commit 26.8 GB, system CPU 44 % (`s1-run3-conditions.txt`). `environment.json`
    read 37 % at the start and 33 % at the end.
  - The measurement lock was taken late. night.py's measure-begin ran at 01:04:06, about 1.5 min into the
    suite, and recorded 26.1 GB free commit, 42.8 % system CPU and no FrostIDE builds (`lock-record.txt`).
    `validate-start`, the four backend validations and about the first four editor runs of round 1 ran before
    it.
  - The other night agents' scripts started from 01:08. Their builds started from 01:11, as the suite ended.
- Run 4: 2026-11-01, 01:43:08 to 01:47:41.
  - It ran `run-s1.ps1 -Suite floors` on Release builds of the code this ADR's commit records (bc20789's suite
    plus `--full-output` and the floors suite): 26 steps in 4:24, exit 0, every run valid.
  - The measurement lock was held throughout. measure-begin at 01:43:03 recorded 26.1 GB free commit, 35.5 %
    system CPU and no FrostIDE builds; measure-end was at 01:47:41 (`lock-begin.txt`, `lock-end.txt`).
    `environment.json` read 61 % at the start (an instantaneous sample) and 34 % at the end.
- The machine was not idle in either run. The user's own software was running: Discord, Wallpaper Engine,
  FanControl, Razer software and Steam, plus FrostEditor.exe processes from another session in run 3.
- Machine: REF-W1, AMD Ryzen 9 5900X (12 cores, 24 threads), RTX 4070 SUPER (driver 32.0.16.1742).
  - Windows 11 Home build 26300 on the High performance power plan.
  - 5120x1440 @ 144 Hz (period 6.944 ms) at 96 dpi; Qt 6.12.0, clang-cl.
  - The desktop is probably advanced-colour (HDR or automatic colour management): the B1 backdrop drawn as
    #206020 is captured as #40ab40. DuplicateOutput1 delivers BGRA8 (format 87), the only format the probe
    requests, so the composition format itself is not recorded.
- Defender and Dev Drive state could not be read from the medium-integrity session.
- Every editor configuration ran 5 times per run, interleaved: backends alternate, and even rounds run in reverse
  order. Spreads are min / median / max over 5 rounds, where the median is the third value. Five rounds give
  rough spreads, not tight intervals.
- The raw data is kept outside the repository in `E:\FrostBuilds\perf\m0\s1\`:
  - `run3\` and `run4-floors\` hold one JSON report per step, `summary.json`, `environment.json`, the start
    conditions and lock records, and run 3's suite log;
  - `run1\` and `run2\` hold the earlier runs;
  - `b1-fade-diagnosis\` holds the independent verifier's six diagnostic first-frame launches (FROST_S1_DIAGNOSE
    logs and `fade.py`) and its recomputation script;
  - `analysis\` holds the scripts that produced this ADR's tables.

### Probe validation (all passed in runs 3 and 4)
`s1_probe_validate` runs a 320x48 GDI window at the start and at the end of each suite.

| Check (run 3) | Start | End | Criterion |
|---|---|---|---|
| Injected 8 ms delay, error of the mean on-screen shift | +0.33 ms | +0.12 ms | within 1.5 ms |
| Injected 25 ms delay, error of the mean shift | -0.37 ms | +0.01 ms | within 1.5 ms |
| Trials seen on screen | 144 of 144 | 144 of 144 | all |
| Bounded: screen time minus flush end, every trial | 1.95–22.03 ms | 6.97–18.45 ms | -0.5 ms to 4 periods (27.78 ms) |
| 8 ms stall, median screen interval (measured of 10) | 13.89 ms (9) | 13.89 ms (9) | 7.5–22.4 ms; at least 6 |
| 25 ms stall, median screen interval (measured of 10) | 27.78 ms (7) | 27.78 ms (10) | 24.5–39.4 ms; each over 16.7 ms |
| Cadence: intervals matching the frame step | 98.7 % | 98.5 % | at least 95 % |
| WaitForVBlank interval p50 (DWM period 6.944 ms) | 6.945 ms | 6.947 ms | within 0.05 ms |
| WaitForVBlank wake-up after the grid vblank, p50 / max | 0.09 / 0.41 ms | 0.09 / 0.30 ms | reported |
| Trivial window, phase C (719 refreshes, no stalls) | 9.3 % missed, max 20.83 ms | 5.0 % missed, max 20.83 ms | reported |

Stalls come every 36 frames, alternating 8 and 25 ms. The stall windows are [max(D, P) - 0.5, D + 2P + 0.5] ms
for stall D and refresh period P. The grid vblank is DWM's qpcVBlank plus whole refresh periods. WaitForVBlank
woke within 0.55 ms after it in both runs, which shows that the grid matches the real vblanks, although qpcVBlank
itself is refreshed irregularly (Findings).

Run 4 repeated the GDI validation, and it passed at both ends:
- the 8 ms delay error was -0.20 / -0.02 ms and the 25 ms error -0.30 / +0.58 ms;
- 144 of 144 trials were seen, bounded at 6.88–20.47 / 6.91–19.27 ms;
- the stalls were measured 9, 9 / 7, 9 of 10, and the cadence was 98.8 / 99.5 %;
- the wake-up was p50 0.13 / 0.05 ms, max 0.55 / 0.32 ms;
- the phase C windows missed 7.2 / 7.5 %, max 20.83 ms.

In run 3, each Qt backend is validated in a 1280x720 window. `--validate-delays` spins the delay inside
`paintEvent` or `QRhiWidget::render`. `--inject-cpu-ms 2` spins on every other tick.

| Check | Raster (GDI flush) | QRhi (flip-model swap chain) | QRhi, whole output (run 4) | Criterion |
|---|---|---|---|---|
| Injected 8 ms delay, error of the mean shift | -0.01 ms | +0.27 ms | -0.02 ms | within 1.5 ms |
| Injected 25 ms delay, error of the mean shift | +0.45 ms | +0.13 ms | +0.30 ms | within 1.5 ms |
| Trials seen on screen | 144 of 144 | 144 of 144 | 144 of 144 | all |
| Bounded: screen time minus paint end | 7.41–22.76 ms | 7.93–22.40 ms | 7.94–21.18 ms | -0.5 ms to 27.78 ms |
| UI-CPU probe: 2 ms injected spin, measured | 1.987 ms (360 intervals) | 2.005 ms (360 intervals) | – | within 10 % |

### B6 fling and B5 typing (`s1_editor`, run 3, 5 rounds)
The test scrolls 200,000 lines of synthetic C++ in Consolas 15 px (8 px advance, 18 px lines, 2,865 glyph quads
per frame). It is a 10 s fling at 24,000 px/s decaying with τ = 3 s, one tick per vblank, so every refresh has
new content. The window is 5120x1392, the work area. Each cell gives min / median / max.

| Run | UI CPU/frame p99 | Paint p99 | Missed refreshes | Max interval | Ticks dropped by UI | Merged captures | B6 passes (CPU, missed, interval) |
|---|---|---|---|---|---|---|---|
| raster, full repaint | 8.92 / **9.18** / 10.52 ms | 6.00 / 6.19 / 7.03 ms | 43.6 / 49.3 / 50.8 % | 27.8 / 34.7 / 34.7 ms | 17.8 / 23.0 / 24.0 % | 145 / 195 / 201 | 0, 0, 0 of 5 |
| raster, scroll-blit | 6.40 / **7.38** / 7.71 ms | 1.01 / 1.09 / 1.36 ms | 38.9 / 41.1 / 41.8 % | 27.8 / 27.8 / 34.7 ms | 15.3 / 15.5 / 22.1 % | 227 / 289 / 310 | 0, 0, 0 of 5 |
| raster, fill only | 5.69 / **6.29** / 6.41 ms | 2.92 / 3.23 / 3.30 ms | 38.5 / 41.9 / 46.4 % | 27.8 / 27.8 / 34.7 ms | 12.9 / 16.8 / 19.6 % | 269 / 291 / 350 | 0, 0, 0 of 5 |
| QRhi atlas, text | 0.74 / **0.86** / 0.89 ms | 0.49 / 0.58 / 0.67 ms | 1.5 / 4.0 / 4.9 % | 13.9 / 13.9 / 20.8 ms | 0 / 0 / 1 ticks | 0 / 0 / 1 | 5, 0, 3 of 5 |
| QRhi, fill only | 0.66 / 0.74 / 0.83 ms | 0.40 / 0.47 / 0.50 ms | 1.7 / 4.2 / 5.0 % | 13.9 / 20.8 / 20.8 ms | 0 / 0 / 3 ticks | 0 | 5, 0, 2 of 5 |
| raster, text, 1280x720 [^short] | 2.23 / 2.49 / 2.61 ms | 1.84 / 1.94 / 2.02 ms | 10.2 / 14.4 / 18.3 % | 13.9 / 20.8 / 20.8 ms | 0 | 0 | 5, 0, 2 of 5 |
| QRhi, text, 1280x720 [^short] | 0.72 / 0.76 / 0.86 ms | 0.42 / 0.45 / 0.55 ms | 2.8 / 5.3 / 7.1 % | 13.9 / 13.9 / 20.8 ms | 0 | 0 / 0 / 1 | 5, 0, 3 of 5 |
| Trivial GDI window (phase C, start and end) | – | – | 5.0 and 9.3 % | 20.8 ms | – | 0 | – |

[^short]: A 3 s fling with no typing (`--size 1280x720 --seconds 3 --keys 0`, 433 frames); the other rows are
10 s flings (1,441 ticks) with 50 keys.

- UI CPU p50 is 7.00 ms (median) for raster with full repaint, against 0.59 ms for QRhi with text. Text costs
  QRhi about 0.1 ms. In run 3 its text and fill-only rows cannot be told apart in missed refreshes or intervals.
- Raster's fill-only frame alone exceeds 5 ms at this size. That cost is paint plus backing-store flush.
  `vblankToFlushMs` gives the wall time from the tick to the flush, but the GUI thread's cycles are not split
  between paint and flush, so how the cost divides between them is inferred. Scroll-blit removes most of the
  paint (p99 1.09 ms) but not the flush or QWidget::scroll's move of the backing store.
- Vblank to screen is the time from the vblank tick that started a frame to the display vblank that showed it.
  - Raster's median was 20.8 ms at full size and 13.8 ms at 1280x720 in every round.
  - QRhi's median settled per run at one of two levels: 20.8 or 27.7 ms at full size (27.7 ms in 3 of 5 text
    rounds and 4 of 5 fill rounds), and 13.8 or 27.7 ms at 1280x720 (27.7 ms in 1 of 5).
  - So QRhi's present path sometimes queues one or two extra frames. Run 4 shows that this belongs to the
    present path, not the content or the desktop. A trivial 320x48 QRhi window switched between 13.8, 20.8
    and 27.7 ms too, while an interleaved trivial raster window stayed at 13.8 ms in 5 of 5 rounds. Typing does
    not show it: input to screen p50 was 12.0 ms on QRhi against 12.8 ms on raster.

**B5.** The test sends 50 keys, 23 ms apart and not aligned to vblank. Each inserts a character and repaints the
line, or the whole frame on QRhi. B5 passed in 5 of 5 rounds on every backend in run 3. In run 4 it passed in every
run that typed: the QRhi runs at the work area and the three whole-output runs.

| Backend | Input to paint end p50 (median) | Input to paint end p99 | Input to screen p50 / p99 (medians) |
|---|---|---|---|
| raster | 0.17 ms | 0.29 / 0.36 / 0.67 ms | 12.8 / 21.3 ms |
| raster, scroll-blit | 0.18 ms | 0.27 / 0.28 / 0.31 ms | 14.5 / 21.7 ms |
| QRhi | 0.33 ms | 0.42 / 0.52 / 0.67 ms | 12.0 / 20.0 ms |

The spike has no layout and no soft wrap, so this only shows that the backends add nothing significant.

### Trivial windows and the whole output (`run-s1.ps1 -Suite floors`, run 4)
Each of 5 rounds ran four windows, in reverse order in even rounds:
- a trivial QRhi window (320x48 at the work area's top-left, fill only, 5 s fling, no typing);
- QRhi with text at the work area;
- the same trivial window on raster (a Qt raster window flushed through GDI);
- QRhi with fill only at the work area.

After round 3, one round covered the whole output (`--full-output`, 5120x1440 over the taskbar) with QRhi text,
raster fill only and QRhi fill only.

| Run | UI CPU/frame p99 | Missed refreshes | Max interval | Vblank to screen p50, per round | Ticks dropped | Merged | B6 passes (CPU, missed, interval) |
|---|---|---|---|---|---|---|---|
| QRhi, text, work area | 0.79 / 0.86 / 0.93 ms | 4.6 / 5.5 / 9.2 % | 13.9 / 13.9 / 20.8 ms | 20.8, 27.7, 20.8, 27.7, 20.8 ms | 0, 7, 0, 1, 0 | 0 / 0 / 1 | 5, 0, 3 of 5 |
| QRhi, fill only, work area | 0.72 / 0.75 / 0.78 ms | 3.8 / 4.4 / 5.3 % | 13.9 ms in all | 20.8, then 27.7 in 4 | 0, 1, 0, 0, 2 | 0 | 5, 0, 5 of 5 |
| Trivial QRhi window, 320x48 | 0.73 / 0.77 / 0.80 ms | 8.2 / 9.6 / 10.9 % | 13.9 ms in all | 13.8, 20.8, 27.7, 13.8, 13.8 ms | 0 | 0 | 5, 0, 5 of 5 |
| Trivial raster window, 320x48 | 0.31 / 0.31 / 0.32 ms | 6.9 / 7.8 / 9.3 % | 13.9 / 20.8 / 20.8 ms | 13.8 ms in all | 0 | 0 | 5, 0, 2 of 5 |
| Trivial GDI window (phase C, start and end) | – | 7.2 and 7.5 % | 20.8 ms | – | – | 0 | – |
| QRhi, text, whole output (1 round) | 0.85 ms | 1.25 % | 13.9 ms | 13.8 ms | 0 | 0 | pass, fail, pass |
| QRhi, fill only, whole output (1 round) | 0.77 ms | 0.56 % | 13.9 ms | 20.8 ms | 0 | 0 | pass, pass, pass |
| raster, fill only, whole output (1 round) | 7.18 ms | 38.8 % | 27.8 ms | 20.8 ms | 340 (23.6 %) | 172 | fail, fail, fail |

- At the work area, QRhi repeated run 3: CPU passed 10 of 10 rounds over both runs, and missed refreshes were
  never within 1 %. Run 4's text rounds missed a little more than run 3's (median 5.5 against 4.0 %).
- By median, the trivial windows missed more than the QRhi editor in the same rounds (7.8 and 9.6 % against 5.5
  and 4.4 %). A trivial window therefore shows how disturbed the desktop is, but it is not a floor under a large
  window's misses (Decision 7).
- Covering the whole output cut QRhi's misses to 0.56–1.25 % and kept every interval at 13.9 ms. Raster did not
  change (38.8 % against 38.5–46.4 % at the work area in run 3). One explanation fits but was not tested: DWM may
  flip a borderless flip-model swap chain that covers the output directly, or put a large one on an overlay
  plane, instead of composing it. PresentMon's present mode would show which.

### B1 first frame (`s1_first_frame`, run 3, empty workspace, 1280x800 window)
Each warm block of 10 launches follows one priming launch. Each cell gives min / p50 / p90 / max; with 20
samples the nearest-rank p99 equals the maximum. "First frame" is the detector's endpoint, two or three refreshes
after the first visible change (Caveats).

| Target | Launches | First frame on screen | Settled (fade-in done) p50 | Window first seen p50 (polled) |
|---|---|---|---|---|
| frost.exe, warm | 20 (2 blocks) | 251.8 / **264.9** / 272.3 / 360.1 ms | 314.9 ms | 248.2 ms |
| `s1_editor --first-frame`, raster, warm | 10 (1 block, at the end) | 248.3 / **257.7** / 286.7 / 293.0 ms | 310.7 ms | 235.8 ms |
| `s1_editor --first-frame`, QRhi, warm | 20 (2 blocks) | 415.2 / **474.6** / 488.5 / 736.7 ms | 512.1 ms | 467.3 ms |
| frost.exe, cold proxy | 5 | 346.6 / **351.2** / 362.0 / 362.0 ms | 403.6 ms | 341.5 ms |

All 55 measured windows were made topmost before they were shown. Every warm frost.exe launch but one took
281.1 ms or less. frost.exe's and QRhi's two blocks each were interleaved between the editor rounds (after rounds
1 and 3). Raster's single block ran after round 5, next to the cold proxy, so it was not interleaved.

In-process timings of `s1_editor --first-frame`, p50 over every launch including the priming ones:

| From → to | Raster (11 launches) | QRhi (22 launches) |
|---|---|---|
| Process creation → `main` | 15.1 ms | 15.1 ms |
| `main` → QApplication constructed | 25.5 ms | 25.8 ms |
| `main` → view created | 41.5 ms | 43.0 ms |
| `main` → first `show()` returned | 199.0 ms | 411.4 ms |
| `main` → first paint ended | 204.7 ms | 405.0 ms |
| First `show()` alone (view created → returned) | 157.5 ms | 367.4 ms |
| Pipeline creation, including HLSL compilation | – | 6.5 ms |

QRhi renders its first frame inside `show()`; raster paints just after `show()` returns. Both finish before the
harness sees the window. Raster's first `show()` takes about 157 ms of `s1_editor`'s 258 ms. frost.exe's own
breakdown is not measured here; M1 profiles it.

### Caveats
- **Load.** The machine was busy with the user's own software (see Conditions), and Wallpaper Engine kept DWM
  composing. The CPU probe counts only the GUI thread's own cycles, so preemption does not inflate it, but cache
  and clock contention can.
- **Sample sizes.** Each configuration has 5 rounds per run. There are four phase C trivial-window samples (two
  per run) and ten `s1_editor` trivial-window runs (run 4). The whole-output configurations ran once each.
  Raster's first-frame block (10 launches) ran once, at the end of the suite, not interleaved like the others.
- **UI CPU covers the GUI thread only.** Raster's GDI flush runs on that thread (in the kernel) and is counted.
  The Direct3D runtime's and the GPU driver's worker threads are not, so QRhi's 0.86 ms excludes them. The
  injected-spin check validates what is counted, not what is left out.
- **Merged captures.** Desktop Duplication sometimes returns one frame for several desktop presents
  (AccumulatedFrames > 1). An update inside such a frame is invisible, so its refresh counts as missed and its
  interval is too long. Raster at full size had 145–350 merged captures per run, so its missed refreshes and
  maximum intervals are upper bounds. If each merge hid exactly one update, raster missed 16–37 %, but a merge
  can hide more, and the reports keep only the count. Every QRhi, 1280x720 and trivial-window run had 0 or 1,
  and the phase C windows had 0, so those numbers are not affected. Raster's verdict rests on CPU, which merged
  captures do not touch.
- **What "missed compositions" means here.** B6 says missed compositions. S1 measures refreshes in which the
  window's marker did not change. That counts frames the application never presented as well as presents DWM did
  not show. PLAN's PresentMon cross-check for the QRhi backend was skipped, so present-side counts for QRhi
  are not available (no dependency was added).
- **Window size.** The editor ran at 5120x1392, the work area a maximised window gets, not at B6's 5120x1440. That
  does not matter for the CPU criterion. It does matter for missed refreshes and intervals: covering the whole
  output changed QRhi's misses from 3.8–9.2 % to 0.56–1.25 % (one round). Whether that was independent flip, an
  overlay plane or something else was not observed.
- **B1 endpoint.** The detector fires on about the third composed frame of DWM's fade-in, not the first. In six
  diagnostic launches by the independent verifier (3 of frost.exe, 3 of the raster `s1_editor`), it fired
  12.1–18.5 ms after the first present whose samples differed from the backdrop at all, with two composed frames
  in between (`b1-fade-diagnosis`). So the first frame here is about two or three refreshes late. It is also
  later than budgets.json's endpoint, the ETW FirstPresent event, by the time from present to composition. The
  numbers are therefore conservative, and B1 fails with either endpoint: frost.exe's first change is at about
  250 ms.
- **Window first seen** is polled: EnumWindows between Sleep(1) calls, every 1 to 16 ms depending on the timer
  resolution. It is context only.
- **Cold proxy.** Before each launch it copies frost.exe and its DLLs with COPY_FILE_NO_BUFFERING into a new
  directory, so the copies have no prefetch trace and the copy does not cache them. Whether they are then read
  from disk at launch is not verified, because Defender may scan the new files when they are closed, which reads
  them into the cache. System DLLs stay cached. Defender's first scan is mixed into the launch as well. So the
  bias has an unknown direction. budgets.json's cold conditions (reboot or a RAMMap -Et standby purge) are not met
  (question 4).
- **B5** is a synthetic QKeyEvent sent to the view, so it skips the Windows input queue.
- **Spike views, not the M1 editor:** ASCII only, one font, no wrapping and no horizontal scroll.

### Earlier runs (under load; supporting only)
Runs 1 and 2 (2026-10-31) measured each configuration once, while two or three agent builds and a video ran.
Run 3 supersedes their numbers.
- **Run 1** used the harness before the spin fix (Methods). Every CPU figure in it is close to one refresh period
  and is not a measurement of the backends. Raster's p99 was 9.21, 7.39 and 6.71 ms (full repaint, scroll-blit,
  fill only). QRhi's was 6.85 ms with text and 6.95 ms with fill only, so QRhi's fill-only run is above raster's.
  - Raster missed 39.7–52.0 % of refreshes at full size, and QRhi 1.6–3.4 %.
  - An earlier version of `s1_first_frame` (frost.exe only, 8 valid warm launches) gave a warm first frame of
    p50 245.5 ms and a cold-proxy p50 of 330.5 ms.
- **Run 2** used the fixed harness and agrees with run 3 in direction:
  - raster's p99 UI CPU was 9.44, 7.85 and 6.66 ms (full repaint, scroll-blit, fill only), against 0.98 ms for
    QRhi with text and 0.86 ms with fill only;
  - raster missed 41.1–53.2 % of refreshes at full size and QRhi 6.3–6.7 %;
  - at 1280x720, raster used 2.49 ms and missed 14.1 %, and QRhi 0.81 ms and 4.6 %.

  Run 2 had no first-frame steps.

### Methods
- **Session precheck.** Before and after every run, the precheck verifies that the input desktop is unlocked
  (OpenInputDesktop and WTS lock state), that the session is the active console session, and that a DXGI output
  is attached with the console display on (GUID_CONSOLE_DISPLAY_STATE). A failed check, a lost duplication or a
  capture failure marks the run invalid.
- **Screen time.**
  - A screen update is a duplicated frame whose dirty region touches the window and that shows a new 16-bit marker
    (22 cells of 4 px drawn at the window's top-left corner).
  - Its time is the first vblank on DWM's grid at or after DXGI's LastPresentTime. Raw present times jitter across
    the refresh period, so their differences do not count refreshes.
  - When several presents fall before one vblank, only the last one counts. Captures that merged several presents
    are counted per run (`mergedCaptures`).
  - A missed refresh is any refresh with no new content: an interval of k periods counts k - 1.
- **Verdicts need samples.** A gate with too few samples is reported as "not measured", never as a pass.
  - B6 CPU needs at least 100 tick-to-tick intervals.
  - B6 missed and interval need two decoded updates.
  - B5 needs every key painted.
  - A run with fewer decoded updates than a quarter of its fling frames is invalid. No run in runs 3 and 4 had an
    undecoded capture.
- **UI CPU per frame** is QueryThreadCycleTime on the GUI thread from one vblank tick to the next, calibrated
  against QPC. It includes work that Qt defers to the event loop: QRhiWidget renders and presents on the window's
  next update, not inside `repaint()`.
- **Trivial windows and the whole output (run 4).** `run-s1.ps1 -Suite floors` interleaves trivial
  `s1_editor` windows (`--content fill --size 320x48 --seconds 5 --keys 0`, both backends) with QRhi runs at the
  work area. It adds one round with `--full-output`, which places the window over `QScreen::geometry()` instead of
  `availableGeometry()`, and a QRhi delay validation in that window.
- **B1.**
  - Process start is the QPC reading just before CreateProcessW. The child runs in its executable's directory, in
    a kill-on-close job. `tests/perf/s1/child_process.cpp` calls CreateProcessW directly, not through
    `frost::io::IProcessLauncher`, which is not merged yet (TODO(M1) in the file; Consequences).
  - The window is placed with STARTF_USEPOSITION over a solid topmost backdrop. Five pixels inside the client area
    are read from every duplicated frame.
  - The first frame is the first duplicated frame in which three of them are more than 24 levels from the
    backdrop colour, which during the fade-in is about its third composed frame (Caveats). "Settled" is when four
    of them are within 6 levels of the idle colour.
  - `s1_editor --first-frame` opens a frost.exe-sized window with either backend from one executable, so the
    difference between the two backends is QRhi's own cost.
- **Fixed during the spike.** The first editor series measured about one refresh period (6.3–7.0 ms) of "CPU"
  wherever the real work was shorter. The cause was the harness: `processEvents(WaitForMoreEvents, maxTime)` drops
  WaitForMoreEvents and spins. The harness now blocks in the one-argument overload; runs 2, 3 and 4 use it.

### Findings about the platform
- **DWM counters are synthetic on build 26300.** In DwmGetCompositionTimingInfo with a null window, cRefresh and
  cFrame grow by exactly one per call, and cFramesMissed, cFramesDropped and cFramesLate stay 0. The refresh
  period is real. qpcVBlank is a real time on the refresh grid, but it is refreshed irregularly.
  - A kept dump (`run1\dwm-counters.json`) shows this. Six calls 100 ms (14.4 periods) apart returned cRefresh 2
    to 7, while qpcVBlank moved by 10, 21, 16, 0 and 21 whole periods. Calls 4 and 5 returned the same value, and
    one value lay 1.8 ms ahead of the call.
  - The probes therefore use qpcVBlank only as an anchor for the grid (qpcVBlank plus whole periods). The
    wake-phase check above confirms the grid. Missed compositions come from the duplication markers.
- **DWM ETW is deferred.** A real-time Microsoft-Windows-Dwm-Core session needs elevation or Performance Log Users
  membership, and agent sessions run at medium integrity. The duplication markers measure the same quantity, and
  their validation is above.
- **Trivial windows are not a floor.** On REF-W1's busy desktop, trivial 320x48 windows missed 5.0–10.9 % of
  refreshes (GDI and Qt raster 5.0–9.3 %, QRhi 8.2–10.9 %). That is mostly more than the QRhi editor at the work
  area (1.5–9.2 %; in run 4 medians 7.8 and 9.6 % against 5.5 and 4.4 %), and far more than at the whole output
  (0.56–1.25 %). Which windows lose refreshes depends on their size and present path, not only on the desktop:
  - at 1280x720, raster lost more than QRhi without dropping ticks (10–18 % against 3–7 %);
  - at 320x48, QRhi lost slightly more than raster.
- **A window shown without activation by a background process opens below the foreground window**, so it is not
  on screen. B1 automation needs either a window it may raise (as here) or a quiet desktop.

## Consequences
- If the user approves (question 1), M1 builds `IEditorRenderer` with the QRhiWidget atlas backend as the default
  on Windows. Raster remains the oracle (golden images) and the fallback for RDP, WARP and remote sessions. The
  backend is chosen at startup. Linux keeps raster until a Linux S1 leg exists (Decision 8).
- **QRhi start-up must stay out of B1, and moving it later lands it on B2 and B15.** It costs 216.9 ms on screen
  at p50, and about 210 ms of that is inside the first `show()`. M1 measures these options against B1, B2 and
  B15 before the backend ships:
  - Show the empty workspace with raster, and create the RHI surface when the first editor opens. Whether
    converting a shown window to RHI composition is seamless is unmeasured. This only moves the cost:
    - B2 (warm reopen with an editor tab restored, process start to the tab's first highlight, p99 300 ms, gated
      at M2) would pay it during start-up. frost.exe's first frame alone is p50 264.9 ms, so B2 would land
      around 475 ms before any editor work.
    - B15 (open request to F1's first screen, p50 150 ms, M1) would pay it whenever F1 is the first editor
      opened.
  - Give the editor its own native RHI surface, so the rest of the window stays raster. The cost moves to the
    first editor in the same way.
  - Warm Direct3D 11 and the driver on a worker thread while the application starts. This is the only option that
    could hide the cost from all three budgets. Whether the 210 ms can run in parallel is unmeasured.

  The M1 B1, B2 and B15 tests must include whichever path ships.
- Runtime HLSL compilation of the spike's two trivial shaders costs 6.5 ms, which gives no reason to precompile.
  M1's shader set is larger and is measured then. Precompiling would not need Qt Shader Tools (qsb): DXBC from
  the Windows SDK's fxc can be wrapped in a QShader.
- QRhi's present path sometimes holds a frame one or two refreshes longer, even for a trivial window (see the
  vblank-to-screen results). M1 checks the swap chain's frame latency before the latency budgets are set.
- M1's B6 test records the present mode for the QRhi backend with PresentMon, as PLAN.md's cross-check says,
  which S1 skipped. It also runs a quiet-desktop control. Both are needed to answer question 2: where QRhi's
  misses at the work area come from, and why covering the whole output removed most of them.
- The whole window is RHI-composited, so every widget's backing store is uploaded as a texture. The M1 B6 test
  must include tool windows and docks, not just the editor.
- B1 warm misses by about 115 ms at p50 before any editor work exists. M1 adds ETW startup markers (FirstPresent)
  and profiles the first `show()`, DLL loading, the platform plugin and the font database (question 3). A true
  cold run waits for question 4.
- `tools/perf.ps1` reports B6 with its context: interleaved trivial windows and merged-capture counts. These never
  adjust a verdict (Decision 7). How missed and interval are gated is question 2, and a budget change needs the
  user's approval.
- M1's B6 test keeps the per-backend probe validations (`--validate-delays`, `--inject-cpu-ms`), including the
  whole-output delay validation, so the swap-chain path stays validated.
- QRhi is semi-public API in Qt's private Gui package: `find_package(Qt6 COMPONENTS GuiPrivate)` and
  `Qt6::GuiPrivate`. It is the same Qt6Gui library under the same licence. `tools/licensecheck.py` already mapped
  `Qt6::GuiPrivate` targets to Gui; this spike makes it map private `find_package` components the same way.
  Two new tests cover it, one of them checking that an unknown private module still needs an ADR. Private APIs
  may change between Qt minors, so M1 wraps them in one module.
- `tests/perf/s1/child_process.cpp` starts children with CreateProcessW directly. That departs from CLAUDE.md's
  `IProcessLauncher` rule because m0/io is not merged yet. Port it to `frost::io::IProcessLauncher` when m0/io
  lands; ProcessSpec then needs the first window's placement and show command (TODO(M1) in the file).

## Alternatives considered
- **Ship raster and accept the miss.** At 5120x1392 even a fill-only raster frame exceeded B6's CPU budget in all
  5 rounds (p99 5.69–6.41 ms), before any text is drawn, and at the whole output it was 7.18 ms. Raster cannot
  pass B6's CPU criterion at this size, whatever the desktop.
- **Raster with partial flushes** (scroll-blit plus a dirty-region flush). A scroll moves every pixel, so the flush
  is still the whole window; scroll-blit measured p99 6.40–7.71 ms.
- **PresentMon as the on-screen timing instrument.** It cannot see raster GDI flushes, and it would add a
  dependency. The same validated probes measure both backends instead. PresentMon stays PLAN's cross-check for
  QRhi, which M1 runs (Consequences).
- **DWM ETW instead of Desktop Duplication.** It needs elevation, so it is deferred; see Findings.
- **Gating B6 against a same-session trivial-window floor.** Withdrawn: in run 4 trivial windows missed more than
  the QRhi editor, so they bound nothing.
- **A true cold start** (reboot, or a standby-list purge with RAMMap -Et). Neither is possible from an agent
  session; question 4 asks the user. The proxy is not a substitute.
