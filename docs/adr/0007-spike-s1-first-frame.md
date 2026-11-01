# ADR 0007: Spike S1, editor rendering at 5120x1440 @ 144 Hz and first frame

- Status: proposed. It needs the user's answers to the three questions below. The measurements are final:
  run 3 (2026-11-01), the quiet-window suite with five interleaved rounds, all probe validations passed.
- Date: 2026-11-01 (first drafted 2026-10-31)
- Plan reference: PLAN.md section 2 ("Editor rendering"), section 4 (B1, B5, B6), section 6.3 (session precheck),
  M0 spike S1, risk T10

## Context
PLAN.md ships the raster QPainter editor backend (QGlyphRun cache) and builds a QRhiWidget glyph-atlas backend only
if the S1 gate fails at 5120x1440 @ 144 Hz. The S1 gate is B6 (10 s fling: UI CPU per frame p99 at most 5 ms,
missed compositions at most 1 %, no content interval over 16.7 ms), plus B5 (keystroke to paint end, p50 3 ms and
p99 8 ms) and warm B1 (first frame, p50 150 ms and p99 250 ms). QRhiWidget composites the whole window, so the
backend has to be fixed before the first frame ships.

Raster Qt flushes through GDI into the DWM redirection surface and has no swap chain, so present-based tools see
nothing. S1 therefore needs backend-neutral probes, validated before their numbers count.

## Questions for the user
1. **Approve QRhi as M1's editor renderer?** That means the QRhiWidget glyph-atlas backend as the default on
   Windows, with raster kept as the golden oracle and the RDP/WARP fallback (Decisions 1 to 3). Raster fails B6's
   CPU criterion at 5120x1392 by its own cost in every round. QRhi passes it with six times the headroom, but it
   adds about 215 ms to the first frame unless M1 moves its start-up off that path.
2. **How should B6 be gated, given the desktop's own floor?** On REF-W1's normal desktop, a trivial window that
   paints once per vblank missed 5.0–9.3 % of refreshes, with a 20.8 ms maximum interval. No window can meet the
   absolute "missed at most 1 %" or "no interval over 16.7 ms" there; QRhi missed 1.5–4.9 %, under that floor.
   The options are:
   - (a) run gate runs on a quiet desktop (Wallpaper Engine and other composing software paused) and keep the
     absolute budget;
   - (b) gate against the floor measured in the same session (for example, missed at most 1 point above it);
   - (c) both: (a) for the reference gate, (b) on working desktops.

   Any change to the budget needs your approval; this ADR proposes none.
3. **B1 warm fails today, before any editor work.** frost.exe's first frame is p50 264.9 ms against 150 ms, and
   the maximum of 20 launches (which nearest-rank p99 equals here) is 360.1 ms against 250 ms. Its window is not
   visible until p50 248 ms. QRhi on the first-frame path would add about 215 ms more. Should M1 keep the
   150/250 ms budget as the target (startup profiling, ETW markers, QRhi start-up off the first frame), or should
   B1 be re-baselined?

## Decision (proposed)
1. **M1 builds the QRhiWidget glyph-atlas backend as the default editor renderer on Windows** (the PLAN.md
   contingency, risk T10). Raster fails S1 at 5120x1392 by its own cost, in every one of 5 rounds:
   - UI CPU p99 is 9.18 ms (median of 5 rounds) with full repaint, 7.38 ms with scroll-blit, and 6.29 ms even
     with fill only (no text).
   - Raster drops 13–24 % of vblank ticks itself and misses 39–51 % of refreshes, against a desktop floor of
     5–9 %.

   QRhi uses p99 0.86 ms, never drops a tick (at most 0.2 %), and misses 1.5–4.9 %. That is no more than its own
   fill-only run (1.7–5.0 %) and is under the floor.
2. **QRhi passes B6's CPU criterion, and its remaining misses come from the desktop, not from the backend.** Per
   criterion over 5 rounds at full size with text:
   - CPU passes 5 of 5;
   - the content interval stays within 16.7 ms in 3 of 5 rounds (13.9 ms), and is 20.8 ms in the other 2, the
     same maximum the trivial window had;
   - missed refreshes are within 1 % in 0 of 5 rounds.

   How the missed and interval criteria are gated is question 2.
3. **QRhi's start-up must stay off the B1 path.** In the same executable and window, the first frame is p50
   474.6 ms with QRhi (20 launches) against 257.7 ms with raster (10 launches). In-process, the window's first
   `show()` takes about 368 ms instead of about 157 ms. Pipeline creation, including runtime HLSL compilation, is
   only 6.5 ms. So the cost is the window's RHI and Direct3D 11 start-up, not shaders. M1 measures ways to keep
   it out of the first frame (see Consequences) before the backend ships.
4. **B1 warm fails independent of the backend** (question 3). M1 adds startup ETW markers and works the budget
   down.
5. **Raster stays the golden oracle and the fallback for RDP, WARP and remote sessions.** It is selected before
   the first frame. At 1280x720 it meets B6's CPU criterion (p99 2.49 ms), but it misses 10–18 % of refreshes
   there, against 3–7 % for QRhi at the same size.
6. **The backend-neutral probes are the S1/B6 instrument** (`tests/perf/probes`, `frost::perf_probes`, layer
   diag):
   - Desktop Duplication with a frame marker for content intervals, missed refreshes and frame-to-screen latency;
   - DWM timing for the refresh period and the vblank grid;
   - the `ISessionState` precheck.

   They are validated on a GDI window and through both Qt backends, including QRhi's flip-model swap chain, and
   the UI-CPU probe is validated by an injected spin. DWM's composition counters are not used (see Findings).
7. **B6's missed and interval results are reported against an environmental floor measured in the same session**,
   as well as against the absolute budget. The floor is a trivial window painting once per vblank in a phase
   without stalls (`s1_probe_validate` phase C). A backend's own fill-only run is not a floor, because raster
   fill-only already drops ticks through its own CPU cost.

## Measurements (run 3)
**Conditions.**
- When: 2026-11-01, 01:02:33 to 01:11:10. `tests/perf/s1/run-s1.ps1` ran at bc20789 on Release builds: 47
  steps in 8:29, exit 0. Every run was valid: no session change, no lost duplication and no capture failure.
- Machine: REF-W1, AMD Ryzen 9 5900X (12 cores, 24 threads), RTX 4070 SUPER (driver 32.0.16.1742).
  - Windows 11 Home build 26300 on the High performance power plan.
  - 5120x1440 @ 144 Hz (period 6.944 ms) at 96 dpi; the desktop is composed in FP16, and DuplicateOutput1
    delivers BGRA8.
  - Qt 6.12.0, clang-cl.
- Load: no FrostIDE builds ran, and free commit was 26.8 GB. It was not an idle machine: system CPU was 37–44 %
  at the start and 33 % at the end, from the user's own processes.
  - Discord, Wallpaper Engine, FanControl, Razer software and Steam were running.
  - FrostEditor.exe processes from another session were also running; they used about 50 % CPU shortly before
    the run.
- Defender and Dev Drive state could not be read from the medium-integrity session.
- Every editor configuration ran 5 times, interleaved: backends alternate, and even rounds run in reverse order.
  Spreads are min / median / max over the 5 rounds, where the median is the third value.
- The raw data (one JSON report per step, `summary.json`, the suite log and the start conditions) is kept with
  the spike data outside the repository as `s1-run3`.

### Probe validation (all 6 passed)
`s1_probe_validate` runs a 320x48 GDI window at the start and at the end of the suite.

| Check | Start | End | Criterion |
|---|---|---|---|
| Injected 8 ms delay, error of the mean on-screen shift | +0.33 ms | +0.12 ms | within 1.5 ms |
| Injected 25 ms delay, error of the mean shift | -0.37 ms | +0.01 ms | within 1.5 ms |
| Trials seen on screen | 144 of 144 | 144 of 144 | all |
| Bounded: screen time minus flush end, every trial | 1.95–22.03 ms | 6.97–18.45 ms | -0.5 ms to 4 periods (27.78 ms) |
| 8 ms stall, median screen interval (measured of 10) | 13.89 ms (9) | 13.89 ms (9) | 7.5–22.4 ms; at least 6 |
| 25 ms stall, median screen interval (measured of 10) | 27.78 ms (7) | 27.78 ms (10) | 24.5–39.4 ms; each over 16.7 ms |
| Cadence: intervals matching the frame step | 98.7 % | 98.5 % | at least 95 % |
| WaitForVBlank interval p50 (DWM period 6.944 ms) | 6.945 ms | 6.947 ms | within 0.05 ms |
| **Environment floor** (phase C, 719 refreshes, no stalls) | **9.3 %** missed, max 20.83 ms | **5.0 %** missed, max 20.83 ms | reported |

Stalls come every 36 frames, alternating 8 and 25 ms. The stall windows are [max(D, P) - 0.5, D + 2P + 0.5] ms
for stall D and refresh period P.

Each Qt backend is validated in a 1280x720 window. `--validate-delays` spins the delay inside `paintEvent` or
`QRhiWidget::render`. `--inject-cpu-ms 2` spins on every other tick.

| Check | Raster (GDI flush) | QRhi (flip-model swap chain) | Criterion |
|---|---|---|---|
| Injected 8 ms delay, error of the mean shift | -0.01 ms | +0.27 ms | within 1.5 ms |
| Injected 25 ms delay, error of the mean shift | +0.45 ms | +0.13 ms | within 1.5 ms |
| Trials seen on screen | 144 of 144 | 144 of 144 | all |
| Bounded: screen time minus paint end | 7.41–22.76 ms | 7.93–22.40 ms | -0.5 ms to 27.78 ms |
| UI-CPU probe: 2 ms injected spin, measured | 1.987 ms (360 intervals) | 2.005 ms (360 intervals) | within 10 % |

### B6 fling and B5 typing (`s1_editor`, 5 rounds)
The test scrolls 200,000 lines of synthetic C++ in Consolas 15 px (8 px advance, 18 px lines, 2,865 glyph quads
per frame). It is a 10 s fling at 24,000 px/s decaying with τ = 3 s, one tick per vblank, so every refresh has
new content. The window is 5120x1392, the work area. Each cell gives min / median / max.

| Run | UI CPU/frame p99 | Paint p99 | Missed refreshes | Max interval | Ticks dropped by UI | B6 passes (CPU, missed, interval) |
|---|---|---|---|---|---|---|
| raster, full repaint | 8.92 / **9.18** / 10.52 ms | 6.00 / 6.19 / 7.03 ms | 43.6 / 49.3 / 50.8 % | 27.8 / 34.7 / 34.7 ms | 17.8 / 23.0 / 24.0 % | 0, 0, 0 of 5 |
| raster, scroll-blit | 6.40 / **7.38** / 7.71 ms | 1.01 / 1.09 / 1.36 ms | 38.9 / 41.1 / 41.8 % | 27.8 / 27.8 / 34.7 ms | 15.3 / 15.5 / 22.1 % | 0, 0, 0 of 5 |
| raster, fill only | 5.69 / **6.29** / 6.41 ms | 2.92 / 3.23 / 3.30 ms | 38.5 / 41.9 / 46.4 % | 27.8 / 27.8 / 34.7 ms | 12.9 / 16.8 / 19.6 % | 0, 0, 0 of 5 |
| QRhi atlas, text | 0.74 / **0.86** / 0.89 ms | 0.49 / 0.58 / 0.67 ms | 1.5 / 4.0 / 4.9 % | 13.9 / 13.9 / 20.8 ms | 0.0 / 0.0 / 0.1 % | 5, 0, 3 of 5 |
| QRhi, fill only | 0.66 / 0.74 / 0.83 ms | 0.40 / 0.47 / 0.50 ms | 1.7 / 4.2 / 5.0 % | 13.9 / 20.8 / 20.8 ms | 0.0 / 0.0 / 0.2 % | 5, 0, 2 of 5 |
| raster, text, 1280x720 [^short] | 2.23 / 2.49 / 2.61 ms | 1.84 / 1.94 / 2.02 ms | 10.2 / 14.4 / 18.3 % | 13.9 / 20.8 / 20.8 ms | 0.0 % | 5, 0, 2 of 5 |
| QRhi, text, 1280x720 [^short] | 0.72 / 0.76 / 0.86 ms | 0.42 / 0.45 / 0.55 ms | 2.8 / 5.3 / 7.1 % | 13.9 / 13.9 / 20.8 ms | 0.0 % | 5, 0, 3 of 5 |
| Trivial GDI window (environment floor) | – | – | 5.0 and 9.3 % | 20.8 ms | – | – |

[^short]: A 3 s fling with no typing (`--size 1280x720 --seconds 3 --keys 0`, 433 frames); the other rows are
10 s flings (1,441 ticks) with 50 keys.

- UI CPU p50 is 7.00 ms (median) for raster with full repaint, against 0.59 ms for QRhi with text. Text costs
  QRhi about 0.1 ms: its text and fill-only rows cannot be told apart in missed refreshes or intervals.
- Raster's fill-only frame alone exceeds 5 ms at this size. That cost is paint plus backing-store flush; the
  split is inferred, because the flush is not timed separately. Scroll-blit removes most of the paint (p99
  1.09 ms) but not the flush or QWidget::scroll's move of the backing store.
- Vblank to screen is the time from the vblank tick that started a frame to the display vblank that showed it.
  - Raster's median was 20.8 ms at full size and 13.8 ms at 1280x720 in every round.
  - QRhi's median settled per run at one of two levels: 20.8 or 27.7 ms at full size (27.7 ms in 3 of 5 text
    rounds and 4 of 5 fill rounds), and 13.8 or 27.7 ms at 1280x720 (27.7 ms in 1 of 5).
  - So QRhi's swap chain sometimes queues one or two extra frames. Typing does not show it: input to screen
    p50 was 12.0 ms on QRhi against 12.8 ms on raster.

**B5.** The test sends 50 keys, 23 ms apart and not aligned to vblank. Each inserts a character and repaints the
line, or the whole frame on QRhi. B5 passed in 5 of 5 rounds on every backend.

| Backend | Input to paint end p50 (median) | Input to paint end p99 | Input to screen p50 / p99 (medians) |
|---|---|---|---|
| raster | 0.17 ms | 0.29 / 0.36 / 0.67 ms | 12.8 / 21.3 ms |
| raster, scroll-blit | 0.18 ms | 0.27 / 0.28 / 0.31 ms | 14.5 / 21.7 ms |
| QRhi | 0.33 ms | 0.42 / 0.52 / 0.67 ms | 12.0 / 20.0 ms |

The spike has no layout and no soft wrap, so this only shows that the backends add nothing significant.

### B1 first frame (`s1_first_frame`, empty workspace, 1280x800 window)
Each warm block of 10 launches follows one priming launch. Each cell gives min / p50 / p90 / max; with 20
samples the nearest-rank p99 equals the maximum.

| Target | Launches | First frame on screen | Settled (fade-in done) p50 | Window visible p50 |
|---|---|---|---|---|
| frost.exe, warm | 20 (2 blocks) | 251.8 / **264.9** / 272.3 / 360.1 ms | 314.9 ms | 248.2 ms |
| `s1_editor --first-frame`, raster, warm | 10 | 248.3 / **257.7** / 286.7 / 293.0 ms | 310.7 ms | 235.8 ms |
| `s1_editor --first-frame`, QRhi, warm | 20 (2 blocks) | 415.2 / **474.6** / 488.5 / 736.7 ms | 512.1 ms | 467.3 ms |
| frost.exe, cold proxy | 5 | 346.6 / **351.2** / 362.0 / 362.0 ms | 403.6 ms | 341.5 ms |

All 55 measured windows were made topmost before they were shown. Every warm frost.exe launch but one took
281.1 ms or less.

In-process timings of `s1_editor --first-frame`, p50 over every launch including the priming ones:

| From → to | Raster (11 launches) | QRhi (22 launches) |
|---|---|---|
| Process creation → `main` | 15.1 ms | 15.1 ms |
| `main` → QApplication constructed | 25.5 ms | 25.8 ms |
| `main` → view created | 41.5 ms | 43.0 ms |
| `main` → first `show()` returned | 199.0 ms | 411.4 ms |
| `main` → first paint ended | 204.7 ms | 405.0 ms |
| Pipeline creation, including HLSL compilation | – | 6.5 ms |

QRhi renders its first frame inside `show()`; raster paints just after `show()` returns. Both finish before the
harness sees the window. Raster's first `show()` takes about 157 ms of `s1_editor`'s 258 ms. frost.exe's own breakdown is not measured here; M1 profiles it.

### Caveats
- **Load.** The machine was busy with the user's own software (see Conditions), and Wallpaper Engine kept DWM
  composing. The CPU probe counts only the GUI thread's own cycles, so preemption does not inflate it, but cache
  and clock contention can.
- **UI CPU covers the GUI thread only.** Raster's GDI flush runs on that thread (in the kernel) and is counted.
  The Direct3D runtime's and the GPU driver's worker threads are not, so QRhi's 0.86 ms excludes them. The
  injected-spin check validates what is counted, not what is left out.
- **B1 endpoint.** The first frame here is the on-screen start of DWM's fade-in, which is later than budgets.json's
  B1 endpoint (the ETW FirstPresent event). The numbers are therefore conservative, but B1 fails with any endpoint:
  the window becomes visible at p50 248 ms.
- **Cold proxy.** It copies frost.exe and its DLLs with COPY_FILE_NO_BUFFERING into a new directory, so
  FrostIDE's binaries are not cached and have no prefetch trace. System DLLs stay cached. Defender's first scan
  of the new files is mixed in. A true cold start is measured at M1 under budgets.json's cold conditions.
- **B5** is a synthetic QKeyEvent sent to the view, so it skips the Windows input queue.
- **Spike views, not the M1 editor:** ASCII only, one font, no wrapping and no horizontal scroll.

### Earlier runs (under load; supporting only)
Runs 1 and 2 (2026-10-31) measured each configuration once, while two or three agent builds and a video ran.
They agree with run 3 in every direction:
- raster's p99 UI CPU was 9.44, 7.85 and 6.66 ms (full repaint, scroll-blit, fill only) against 0.98 ms for
  QRhi;
- raster missed 41–53 % of refreshes and QRhi 6.3–6.7 %;
- frost.exe's warm first frame was p50 245.5 ms.

Run 3 supersedes their numbers.

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
  - When several presents fall before one vblank, only the last one counts.
  - A missed refresh is any refresh with no new content: an interval of k periods counts k - 1.
- **Verdicts need samples.** A gate with too few samples is reported as "not measured", never as a pass.
  - B6 CPU needs at least 100 tick-to-tick intervals.
  - B6 missed and interval need two decoded updates.
  - B5 needs every key painted.
  - A run with fewer decoded updates than a quarter of its fling frames is invalid. No run in run 3 had an
    undecoded capture.
- **UI CPU per frame** is QueryThreadCycleTime on the GUI thread from one vblank tick to the next, calibrated
  against QPC. It includes work that Qt defers to the event loop: QRhiWidget renders and presents on the window's
  next update, not inside `repaint()`.
- **B1.**
  - Process start is the QPC reading just before CreateProcessW. The child runs in its executable's directory, in
    a kill-on-close job.
  - The window is placed with STARTF_USEPOSITION over a solid topmost backdrop. Five pixels inside the client area
    are read from every duplicated frame.
  - The first frame is the first duplicated frame in which three of them leave the backdrop colour; "settled" is
    when they reach the idle colour.
  - `s1_editor --first-frame` opens a frost.exe-sized window with either backend from one executable, so the
    difference between the two backends is QRhi's own cost.
- **Fixed during the spike.** The first editor series measured about one refresh period (6.3–7.0 ms) of "CPU"
  wherever the real work was shorter. The cause was the harness: `processEvents(WaitForMoreEvents, maxTime)` drops
  WaitForMoreEvents and spins. The harness now blocks in the one-argument overload; runs 2 and 3 use it.

### Findings about the platform
- **DWM counters are synthetic on build 26300.** In DwmGetCompositionTimingInfo with a null window, cRefresh and
  cFrame grow by exactly one per call, and cFramesMissed, cFramesDropped and cFramesLate stay 0. Only the refresh
  period and qpcVBlank are real. A kept dump (`dwm-counters.json` in `s1-run1`) shows this: six calls 100 ms
  apart returned cRefresh 2 to 7, while qpcVBlank advanced by 10 to 21 periods between calls. Missed compositions
  therefore come from the duplication markers.
- **DWM ETW is deferred.** A real-time Microsoft-Windows-Dwm-Core session needs elevation or Performance Log Users
  membership, and agent sessions run at medium integrity. The duplication markers measure the same quantity, and
  their validation is above.
- **The desktop has a floor of missed refreshes.** On REF-W1's normal desktop, a trivial window missed 5.0–9.3 %
  of refreshes. A raster window at 1280x720 missed 10–18 %, and QRhi at the same size 3–7 %. GDI-flushed windows
  lose more than the flip-model swap chain.
- **A window shown without activation by a background process opens below the foreground window**, so it is not
  on screen. B1 automation needs either a window it may raise (as here) or a quiet desktop.

## Consequences
- If the user approves (question 1), M1 builds `IEditorRenderer` with the QRhiWidget atlas backend as the default
  on Windows. Raster remains the oracle (golden images) and the fallback for RDP, WARP and remote sessions. The
  backend is chosen at startup.
- **QRhi start-up must stay out of B1.** It costs about 215 ms on screen, 211 ms of it inside the first
  `show()`. M1 measures these options against B1 before the backend ships:
  - show the empty workspace with raster, and create the RHI surface when the first editor opens (whether
    converting a shown window to RHI composition is seamless is unmeasured);
  - give the editor its own native RHI surface, so the rest of the window stays raster;
  - warm Direct3D 11 and the driver on a worker thread while the application starts.

  The M1 B1 test must include whichever path ships.
- Runtime HLSL compilation costs 6.5 ms, so precompiled shaders are not needed for B1. They would need Qt
  Shader Tools (qsb), with a licence check and an ADR.
- QRhi's swap chain sometimes holds a frame one or two refreshes longer during a fling (see the vblank-to-screen
  results). M1 checks the swap chain's frame latency before the latency budgets are set.
- The whole window is RHI-composited, so every widget's backing store is uploaded as a texture. The M1 B6 test
  must include tool windows and docks, not just the editor.
- B1 warm misses by about 115 ms at p50 before any editor work exists. M1 adds ETW startup markers (FirstPresent)
  and profiles the first `show()`, DLL loading, the platform plugin and the font database (question 3).
- `tools/perf.ps1` reports B6 against the same-session environmental floor as well as the absolute budget. How the
  gate uses the two is question 2; a budget change needs the user's approval.
- M1's B6 test keeps the per-backend probe validations (`--validate-delays`, `--inject-cpu-ms`), so the
  swap-chain path stays validated.
- QRhi is semi-public API in Qt's private Gui package: `find_package(Qt6 COMPONENTS GuiPrivate)` and
  `Qt6::GuiPrivate`. It is the same Qt6Gui library under the same licence. `tools/licensecheck.py` already mapped
  `Qt6::GuiPrivate` targets to Gui; this spike makes it map private `find_package` components the same way.
  Two new tests cover it, one of them checking that an unknown private module still needs an ADR. Private APIs
  may change between Qt minors, so M1 wraps them in one module.
- Port `tests/perf/s1/child_process.cpp` to `frost::io::IProcessLauncher` when m0/io is merged. ProcessSpec then
  needs the first window's placement and show command (TODO(M1) in the file).

## Alternatives considered
- **Ship raster and accept the miss.** At 5120x1392 even a fill-only raster frame exceeded B6's CPU budget in all
  5 rounds (p99 5.69–6.41 ms), before any text is drawn. Raster cannot pass B6 at this size, whatever the desktop.
- **Raster with partial flushes** (scroll-blit plus a dirty-region flush). A scroll moves every pixel, so the flush
  is still the whole window; scroll-blit measured p99 6.40–7.71 ms.
- **PresentMon** for on-screen timing. It cannot see raster GDI flushes, and it would add a dependency. The same
  validated probes measure both backends instead.
- **DWM ETW instead of Desktop Duplication.** It needs elevation, so it is deferred; see Findings.
- **A true cold start** (reboot, or a standby-list purge with RAMMap -Et). Neither is possible from an agent
  session. The proxy covers FrostIDE's own binaries only.
