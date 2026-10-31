# ADR 0007: Spike S1, editor rendering at 5120x1440 @ 144 Hz and first frame

- Status: proposed. As measured under load (n=1 per configuration), S1 fails for the raster backend, so PLAN.md
  calls for the QRhi backend. S1 is not yet shown to pass for QRhi, and B1 for QRhi is unmeasured. Because this
  reverses the planned default, it waits for a quiet-window run of `tests/perf/s1/run-s1.ps1` (five interleaved
  rounds, with the probes validated through both backends) and the user's sign-off. That run replaces every table
  below.
- Date: 2026-10-31
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

## Decision
1. **As measured under load (n=1 per configuration), the raster backend fails S1 at 5120x1440, so build the
   QRhiWidget glyph-atlas backend in M1** (the PLAN.md contingency, risk T10). Even a fill-only raster frame at
   5120x1392 costs p99 6.7 ms of UI CPU. That is paint plus backing-store flush; the split is inferred, because the
   flush was not timed separately. Scroll-blit removes most of the paint (paint p99 1.3 ms) but costs more in total
   (p99 7.9 ms), so QWidget::scroll's move of the whole backing store is a large cost too. The minimal QRhi view
   uses p99 1.0 ms.
2. **S1 is not yet shown to pass for QRhi.** Per B6 criterion, at 5120x1392 with text:
   - UI CPU passes (p99 0.98 ms against 5 ms);
   - the content interval fails (max 20.8 ms against 16.7 ms);
   - missed refreshes are 6.7 % against 1 %, while a trivial window missed about 21 % on the same desktop.

   B1 for QRhi is unmeasured. The quiet-window run measures both, from process start for B1.
3. **Raster stays the golden oracle and the RDP/WARP fallback.** It is selected before the first frame. It met
   B6's CPU criterion at 1280x720 (p99 2.5 ms, 3 s fling).
4. **The backend-neutral probes are the S1/B6 instrument** (`tests/perf/probes`, `frost::perf_probes`, layer diag):
   - Desktop Duplication with a frame marker for content intervals, missed refreshes and frame-to-screen latency;
   - DWM timing for the refresh period and the vblank grid;
   - the `ISessionState` precheck.

   DWM's composition counters are not used (see Findings). So far the probes are validated for GDI windows only.
   The quiet-window run also validates them through each Qt backend (see Methods).
5. **B6's "missed compositions" is reported against an environmental floor measured in the same session**, as
   well as against the absolute 1 %. The floor is a trivial window painting once per vblank, in a phase without
   injected stalls (`s1_probe_validate` phase C). A backend's own fill-only run is not a floor: raster fill-only
   already drops ticks through its own CPU cost (278 of 1,454). In the two runs so far, both under load, no
   backend reached 1 % (QRhi fill-only: 1.6 % and 6.3 %).
6. **As measured under load (n=1 per configuration), B1 warm fails independent of the backend** (frost.exe first
   frame p50 246 ms against 150 ms). M1 adds startup ETW markers and works the budget down. QRhi adds D3D11 device
   and swap-chain creation and shader compilation to the first frame, which the M1 B1 test must include.

## Measurements
**Machine and builds.**
- REF-W1: AMD Ryzen 9 5900X (12 cores), RTX 4070 SUPER, Windows 11 build 26300, 5120x1440 @ 144 Hz (period
  6.944 ms), 96 dpi.
- The desktop is composed in FP16 (advanced colour); DuplicateOutput1 delivers BGRA8.
- Release builds (clang-cl), Qt 6.12.0.
- Defender and Dev Drive state could not be read from the medium-integrity session (`Get-MpComputerStatus`
  unreadable, `fsutil devdrv` access denied).

**Load on the machine.**
- Two or three other agents were building in parallel, and commit headroom was about 7 GB; one build failed with
  "paging file too small".
- On the desktop, as observed (not kept as data): a YouTube video playing in Chrome, Wallpaper Engine, Spotify,
  Task Manager, a game client, and another process using about 30 % of the GPU's 3D engine in Task Manager.

**Data.** Every table below is from runs under that load, each configuration measured once (n=1). The raw
reports are kept with the spike data outside the repository:
- `s1-run1`: probe validation in Release and in Debug, B1, the environment record and a DWM counter dump;
- `s1-run2`: the editor series after the harness fix (see Methods).

The quiet-window run (`run-s1.ps1`: 47 steps, about 9.5 min, five rounds) replaces them and gives each
configuration's min / median / max.

### Probe validation (`s1_probe_validate`, GDI window 320x48, Release; run 1, under load)
| Check | Result | Criterion |
|---|---|---|
| Injected paint delay 8 ms, mean on-screen shift | 8.12 ms (error +0.12) | within 1.5 ms |
| Injected paint delay 25 ms, mean on-screen shift | 24.43 ms (error -0.57) | within 1.5 ms |
| Trials seen on screen | 144 of 144 | all |
| Bounded: screen time minus flush end, every trial | 6.95–21.23 ms | -0.5 ms to 4 refresh periods (27.78 ms) |
| 8 ms stall, median screen interval | 13.89 ms (7 of 10 measured) | 7.5–22.4 ms; at least 60 % measured |
| 25 ms stall, median screen interval | 27.78 ms (7 of 10 measured) | 24.5–39.4 ms; every one over 16.7 ms |
| Cadence: intervals matching the frame step | 504 of 528 (95.5 %) | at least 95 % |
| WaitForVBlank interval p50 vs DWM period | 6.946 vs 6.944 ms | within 0.05 ms |
| WaitForVBlank wake-up vs DWM vblank grid | p50 0.04 ms, max 0.23 ms | |

The stalls come every 36 frames over 720 frames, alternating 8 and 25 ms (10 of each). The stall windows are
[max(D, P) - 0.5, D + 2P + 0.5] ms for stall D and period P. A Debug run gave shift errors of -0.31 and
-1.01 ms, 9 and 10 of 10 stalls measured, and 97.0 % cadence (`validate-debug.json`).

These checks validate the probes for GDI windows only. The quiet-window run adds two validations per Qt backend:
- `s1_editor --validate-delays` runs the same delay trials with the delay spun inside `paintEvent` or
  `QRhiWidget::render`, so QRhi's flip-model swap chain is covered;
- `s1_editor --inject-cpu-ms 2` checks that the UI-CPU probe reports a known GUI-thread spin within 10 %.

### B6 fling and B5 typing (`s1_editor`, window 5120x1392 = the work area; run 2, under load, n=1)
The test scrolled 200,000 lines of synthetic C++ in Consolas 15 px (8 px advance, 18 px lines, about 2,900
glyphs per frame). It was a 10 s fling at 24,000 px/s decaying with τ = 3 s, one tick per vblank, so every
refresh has new content.

| Run | UI CPU/frame p50 / p99 | Paint p50 / p99 | Missed refreshes | Max interval | Vblank to screen p50 / p99 | Ticks dropped by UI |
|---|---|---|---|---|---|---|
| raster, full repaint | 7.50 / **9.44** ms | 4.72 / 6.46 ms | 53.2 % | 34.7 ms | 20.8 / 34.7 ms | 440 of 1,453 fling ticks |
| raster, scroll-blit | 5.85 / **7.85** ms | 0.22 / 1.29 ms | 43.4 % | 34.7 ms | 20.8 / 27.8 ms | 295 of 1,455 |
| raster, fill only | 5.20 / **6.66** ms | 2.47 / 3.48 ms | 41.1 % | 27.8 ms | 20.8 / 27.8 ms | 278 of 1,454 |
| QRhi atlas, text | 0.66 / **0.98** ms | 0.34 / 0.65 ms | 6.7 % | **20.8 ms** | 27.7 / 41.6 ms | 23 of 1,454 |
| QRhi, fill only | 0.60 / 0.86 ms | 0.25 / 0.53 ms | 6.3 % | 13.9 ms | 20.8 / 34.6 ms | 14 of 1,454 |
| raster, text, 1280x720 [^short] | 1.85 / 2.49 ms | 1.29 / 1.92 ms | 14.1 % | 20.8 ms | 13.8 / 13.9 ms | 13 of 446 |
| QRhi, text, 1280x720 [^short] | 0.60 / 0.81 ms | 0.26 / 0.49 ms | 4.6 % | 13.9 ms | 20.8 / 20.8 ms | 13 of 445 |
| Environment floor: GDI window 320x48 (run 1) | – | – | about 21 % [^floor] | – | – | – |

[^short]: A 3 s fling with no typing (`--size 1280x720 --seconds 3 --keys 0`, about 433 frames); the other rows
are 10 s flings with 50 keys.

[^floor]: 161 of 758 refreshes. Phase B of the validation missed 201 of 758 (26.5 %, max interval 41.7 ms), but
40 of those are the refreshes its 20 injected stalls cost by design (one per 8 ms stall, three per 25 ms stall).
The earlier "15.5–26.5 %" row included the stalls. `s1_probe_validate` now measures the floor in a separate
stall-free phase.

Vblank to screen runs from the vblank tick that started a frame to the display vblank that showed it. At
1280x720, QRhi shows each frame one refresh later than raster (20.8 against 13.8 ms), so its swap-chain path
queues about one more frame than the GDI flush. At full size with text it adds another refresh (27.7 ms). That
latency is a cost of switching backends, which the quiet-window run measures again.

**B5.** The test sent 50 keys, 23 ms apart and not aligned to vblank. Each inserted a character and repainted
the line, or the frame on QRhi.

| Backend (run 2, under load, n=1) | Input to paint end, p50 / p99 | Input to screen, p50 / p99 |
|---|---|---|
| raster | 0.15 / 0.23 ms | 12.2 / 21.2 ms |
| raster, scroll-blit | 0.19 / 0.31 ms | 13.0 / 21.3 ms |
| QRhi | 0.36 / 0.50 ms | 11.8 / 15.4 ms |

B5 passed on both backends in this run. The spike has no layout and no soft wrap, so this only shows that the
backends add nothing significant.

### B1 first frame (`s1_first_frame`, frost.exe Release, empty workspace, 1280x800 window; run 1, under load)
| Variant | First frame on screen, p50 / p99 | Settled (fade-in done) p50 | Window visible (EnumWindows) p50 |
|---|---|---|---|
| warm (8 launches after 1 priming launch) | **245.5 / 344.4 ms** (p99 = max of 8; min 236) | 301 ms | 219 ms |
| cold proxy (5 fresh, uncached copies) | **330.5 / 535.0 ms** (p99 = max of 5) | 386 ms | 311 ms |

Every one of the 13 measured windows was made topmost before it was shown (`raisedBeforeShow`).
- The warm p99 is a single outlier: every other warm launch took 256.6 ms or less.
- budgets.json's full run takes 20 samples; the quiet-window run takes 20 warm launches per target.
- The endpoint here is the on-screen start of DWM's fade-in. That is later than budgets.json's B1 endpoint (the
  ETW FirstPresent event), so these numbers are conservative against the budget.

### Methods
- **Session precheck.** Before and after every run, the precheck verifies that the input desktop is unlocked
  (OpenInputDesktop and WTS lock state), that the session is the active console session, and that a DXGI output
  is attached with the console display on (GUID_CONSOLE_DISPLAY_STATE). A failed check, a lost duplication or a
  capture failure marks the run invalid. Every run here was valid.
- **Screen time.**
  - A screen update is a duplicated frame whose dirty region touches the window and that shows a new 16-bit marker
    (22 cells of 4 px drawn at the window's top-left corner).
  - Its time is the first vblank on DWM's grid at or after DXGI's LastPresentTime. Raw present times jitter across
    the refresh period, so their differences do not count refreshes.
  - When several presents fall before one vblank, only the last one counts.
  - A missed refresh is any refresh with no new content: an interval of k periods counts k - 1.
- **Verdicts need samples.** A gate with too few samples is reported as "not measured" (null), never as a pass.
  - B6 CPU needs at least 100 tick-to-tick intervals.
  - B6 missed and interval need at least two decoded updates.
  - B5 needs every key painted.
  - A run with fewer decoded updates than a quarter of its fling frames is invalid.
- **UI CPU per frame.** QueryThreadCycleTime on the GUI thread from one vblank tick to the next. It is calibrated
  against QPC, and it includes work that Qt defers to the event loop: QRhiWidget renders and presents on the
  window's next update, not inside `repaint()`. It counts the GUI thread only. Raster's GDI flush runs on that
  thread (in the kernel) and is counted. The Direct3D runtime's and the GPU driver's worker threads are not
  counted, so the raster-to-QRhi comparison may favour QRhi. The quiet-window run checks the probe with a known
  spin on both backends.
- **B5** is a synthetic QKeyEvent sent to the view. It skips the Windows input queue, whose cost was not measured.
- **B1.**
  - Process start is the QPC reading just before CreateProcessW. The window is placed with STARTF_USEPOSITION over
    a solid topmost backdrop. The child runs in its executable's directory, in a kill-on-close job.
  - Five pixels inside the client area are read from every duplicated frame. The first frame is the first one in
    which three of them leave the backdrop colour (the start of DWM's fade-in); "settled" is when they reach the
    idle colour.
  - The cold proxy copies frost.exe and its DLLs with COPY_FILE_NO_BUFFERING into a new directory, so FrostIDE's
    binaries are not cached and have no prefetch trace. System DLLs stay cached. The new files also get
    Defender's first scan, which is mixed into the cold number (Defender's state was unreadable). Cold launch 5
    saw its window at 512 ms against about 311 ms for the others.
  - For QRhi's B1, `s1_editor --first-frame` opens a frost.exe-sized window with each backend and is measured by
    the same harness. Both backends run from one executable, so the difference between them is QRhi's cost.
- **Fixed during the spike.** The first editor series measured about one refresh period (6.3–7.0 ms) of "CPU"
  wherever the real work was shorter. The cause was the harness: `processEvents(WaitForMoreEvents, maxTime)` drops
  WaitForMoreEvents and spins. The harness now blocks in the one-argument overload, and the numbers above were
  re-measured.

### Findings about the platform
- **DWM counters are synthetic on build 26300.** In DwmGetCompositionTimingInfo with a null window, cRefresh and
  cFrame grow by exactly one per call, and cFramesMissed, cFramesDropped and cFramesLate stay 0. Only the refresh
  period and qpcVBlank are real. In the kept dump (`dwm-counters.json`), six calls 100 ms apart returned cRefresh
  2 to 7 while qpcVBlank advanced by 10 to 21 periods between calls. Missed compositions therefore come from the
  duplication markers.
- **DWM ETW is deferred.** A real-time Microsoft-Windows-Dwm-Core session needs elevation or Performance Log Users
  membership, and agent sessions run at medium integrity. The duplication markers measure the same quantity, and
  their validation is above.
- **GDI windows lose frames when DWM is busy.** In the two validation runs, the 320x48 GDI window painting once
  per vblank got 73.5 % (Release) and 84.5 % (Debug) of refreshes on screen, counting the refreshes the stalls
  cost. Without them, the Release run got about 79 %. A diagnostic capture that was not kept showed full-output
  dirty frames from the wallpaper, which would make DWM skip compositions. QRhi's fill-only run lost 1.6 % and
  6.3 % in the two editor series.
- **A window shown without activation by a background process opens below the foreground window**, so it is not
  on screen. B1 automation needs either a window it may raise (as here) or a quiet desktop.

## Consequences
- M1 builds `IEditorRenderer` with the QRhiWidget atlas backend as the default on Windows, if the quiet-window run
  confirms Decision 1 and the user signs off. Raster remains the oracle (golden images), the fallback for RDP, WARP
  and remote sessions, and the backend for small windows if needed. Because the backend is fixed before the first
  frame, the backend choice is made at startup (M1).
- QRhi still has to meet B6's interval and missed criteria. Its extra refresh of latency (see the vblank-to-screen
  column) is part of the decision.
- The whole window is RHI-composited, so every widget's backing store is uploaded as a texture. The M1 B6 test
  must include tool windows and docks, not just the editor.
- B1 must include D3D11 device creation, the swap chain and shader compilation. `s1_editor --first-frame` records
  the pipeline-creation time, which includes runtime HLSL compilation. Ship precompiled shaders (qsb needs Qt
  Shader Tools, which requires a licence check and an ADR) or compile HLSL on a worker thread during startup (M1).
- B1 warm, as measured under load (n=1 per configuration), misses at about 246 ms before any editor work exists:
  about 219 ms passes before the window is visible. M1 adds ETW startup markers (FirstPresent) and profiles DLL
  loading, the platform plugin and the font database.
- In two runs under load, no backend reached B6's absolute "missed at most 1 %". `tools/perf.ps1` should report
  each run against the environmental floor of the same session (a trivial window, no stalls) and run the gate on
  a quiet desktop. Changing the budget itself needs the user's approval; this ADR does not propose that.
- M1's B6 test keeps the per-backend probe validation (`--validate-delays`, `--inject-cpu-ms`), so the swap-chain
  path stays validated.
- QRhi is semi-public API in Qt's private Gui package: `find_package(Qt6 COMPONENTS GuiPrivate)` and
  `Qt6::GuiPrivate`. It is the same Qt6Gui library under the same licence. `tools/licensecheck.py` already mapped
  `Qt6::GuiPrivate` targets to Gui; this spike makes it map private `find_package` components the same way.
  Two new tests cover it, one of them checking that an unknown private module still needs an ADR. Private APIs
  may change between Qt minors, so M1 wraps them in one module.
- Port `tests/perf/s1/child_process.cpp` to `frost::io::IProcessLauncher` when m0/io is merged. ProcessSpec then
  needs the first window's placement and show command (TODO(M1) in the file).

## Alternatives considered
- **Ship raster and accept the miss.** At 5120x1392 a raster frame needed about 5 ms of CPU before any text was
  drawn (fill-only p50 5.2 ms). As measured under load (n=1 per configuration), that fails B6's CPU criterion; the
  quiet-window run confirms or refutes it.
- **Raster with partial flushes** (scroll-blit plus a dirty-region flush). A scroll moves every pixel, so the flush
  is still the whole window; the measurement shows p99 7.9 ms.
- **PresentMon** for on-screen timing. It cannot see raster GDI flushes, and it would add a dependency. The same
  validated probes measure both backends instead.
- **DWM ETW instead of Desktop Duplication.** It needs elevation, so it is deferred; see Findings.
- **A true cold start** (reboot, or a standby-list purge with RAMMap -Et). Neither is possible from an agent
  session. The proxy covers FrostIDE's own binaries only. The real cold B1 is measured at M1 under budgets.json's
  cold conditions.
