# ADR 0007: Spike S1, editor rendering at 5120x1440 @ 144 Hz and first frame

- Status: proposed. S1 fails for the raster backend, so PLAN.md calls for the QRhi backend. This reverses the
  planned default and was measured on a loaded machine, so it waits for the user's sign-off.
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
1. **The raster backend fails S1 at 5120x1440, so build the QRhiWidget glyph-atlas backend in M1** (the PLAN.md
   contingency, risk T10). Even a fill-only raster frame at 5120x1392 costs p99 6.7 ms of UI CPU, all of it
   per-pixel fill and the GDI flush, which no glyph cache can remove. Scroll-blit removes the paint but not the
   flush (p99 7.9 ms). The minimal QRhi view uses p99 1.0 ms.
2. **Raster stays the golden oracle and the RDP/WARP fallback.** It is selected before the first frame; it already
   meets B6's CPU criterion at 1280x720 (p99 2.5 ms).
3. **The backend-neutral probes are the S1/B6 instrument** (`tests/perf/probes`, `frost::perf_probes`, layer diag):
   - Desktop Duplication with a frame marker for content intervals, missed refreshes and frame-to-screen latency;
   - DWM timing for the refresh period and the vblank grid;
   - the `ISessionState` precheck.

   DWM's composition counters are not used (see Measurements).
4. **B6's "missed compositions" is reported against the backend's own fill-only floor on the same desktop**,
   as well as against the absolute 1 %. On REF-W1 as it is used, no backend can reach 1 %, because other software
   makes DWM skip compositions.
5. **B1 warm fails today, independent of the backend** (frost.exe first frame p50 246 ms). M1 adds startup ETW
   markers and works the budget down. QRhi adds D3D11 device and swap-chain creation to the first frame, which
   the M1 B1 test must include.

## Measurements
**Machine and builds.**
- REF-W1: Windows 11 build 26300, RTX 4070 SUPER, 5120x1440 @ 144 Hz (period 6.944 ms), 96 dpi.
- The desktop is composed in FP16 (advanced colour); DuplicateOutput1 delivers BGRA8.
- Release builds (clang-cl), Qt 6.12.0.
- Defender and Dev Drive state could not be read from the medium-integrity session (`Get-MpComputerStatus`
  unreadable, `fsutil devdrv` access denied).

**Load on the machine.**
- Two or three other agents were building in parallel, and commit headroom was about 7 GB; one build failed with
  "paging file too small".
- On the desktop: a YouTube video playing in Chrome, Wallpaper Engine (a full-screen dirty region about every
  35 ms), Spotify, Task Manager, a game client, and another process using about 30 % of the GPU's 3D engine.

Reports: `run-s1.ps1` JSON. Numbers are from the second editor series, after a harness fix (below).

### Probe validation (`s1_probe_validate`, GDI window 320x48, Release)
| Check | Result | Criterion |
|---|---|---|
| Injected paint delay 8 ms, mean on-screen shift | 8.12 ms (error +0.12) | within 1.5 ms |
| Injected paint delay 25 ms, mean on-screen shift | 24.43 ms (error -0.57) | within 1.5 ms |
| Trials seen on screen | 144 of 144 | all |
| 8 ms stall (one per 36 frames), median screen interval | 13.89 ms (7 of 10 measured) | 6.9–22.4 ms |
| 25 ms stall, median screen interval | 27.78 ms (7 of 10 measured) | 24.5–39.4 ms; every one over 16.7 ms |
| Cadence: intervals matching the frame step | 504 of 528 (95.5 %) | at least 95 % |
| WaitForVBlank interval p50 vs DWM period | 6.946 vs 6.944 ms | within 0.05 ms |
| WaitForVBlank wake-up vs DWM vblank grid | p50 0.04 ms, max 0.23 ms | |

A Debug run gave shift errors of -0.31 and -1.01 ms, 9 and 10 of 10 stalls measured, and 97.0 % cadence.

### B6 fling and B5 typing (`s1_editor`, window 5120x1392 = the work area)
The test scrolled 200,000 lines of synthetic C++ in Consolas 15 px (8 px advance, 18 px lines, about 2,900
glyphs per frame). It was a 10 s fling at 24,000 px/s decaying with τ = 3 s, one tick per vblank, so every
refresh has new content.

| Run | UI CPU/frame p50 / p99 | Paint p50 / p99 | Missed refreshes | Max interval | Ticks dropped by UI |
|---|---|---|---|---|---|
| raster, full repaint | 7.50 / **9.44** ms | 4.72 / 6.46 ms | 53.2 % | 34.7 ms | 440 of 1,635 |
| raster, scroll-blit | 5.85 / **7.85** ms | 0.22 / 1.29 ms | 43.4 % | 34.7 ms | 295 |
| raster, fill only | 5.20 / **6.66** ms | 2.47 / 3.48 ms | 41.1 % | 27.8 ms | 278 |
| QRhi atlas, text | 0.66 / **0.98** ms | 0.34 / 0.65 ms | 6.7 % | 20.8 ms | 23 |
| QRhi, fill only | 0.60 / 0.86 ms | 0.25 / 0.53 ms | 6.3 % | 13.9 ms | 14 |
| raster, text, 1280x720 | 1.85 / 2.49 ms | 1.29 / 1.92 ms | 14.1 % | 20.8 ms | 13 |
| QRhi, text, 1280x720 | 0.60 / 0.81 ms | 0.26 / 0.49 ms | 4.6 % | 13.9 ms | 13 |
| GDI validation window (floor) | – | – | 15.5–26.5 % | 27.8–41.7 ms | – |

**B5.** The test sent 50 keys, 23 ms apart and not aligned to vblank. Each inserted a character and repainted
the line, or the frame on QRhi.

| Backend | Input to paint end, p50 / p99 | Input to screen, p50 / p99 |
|---|---|---|
| raster | 0.15 / 0.23 ms | 12.2 / 21.2 ms |
| raster, scroll-blit | 0.19 / 0.31 ms | 13.0 / 21.3 ms |
| QRhi | 0.36 / 0.50 ms | 11.8 / 15.4 ms |

B5 passes on both backends. The spike has no layout and no soft wrap, so this only shows that the backends add
nothing significant.

### B1 first frame (`s1_first_frame`, frost.exe Release, empty workspace, 1280x800 window)
| Variant | First frame on screen, p50 / p99 | Settled (fade-in done) p50 | Window visible (EnumWindows) p50 |
|---|---|---|---|
| warm (8 launches after 1 priming launch) | **245.5 / 344.4 ms** (min 236) | 301 ms | 219 ms |
| cold proxy (5 fresh, uncached copies) | **330.5 / 535.0 ms** | 386 ms | 311 ms |

Every one of the 13 measured windows was made topmost before it was shown (`raisedBeforeShow`).

### Methods
- **Session precheck.** Before and after every run, the precheck verifies that the input desktop is unlocked
  (OpenInputDesktop and WTS lock state), that the session is the active console session, and that a DXGI output
  is attached with the console display on (GUID_CONSOLE_DISPLAY_STATE). A failed check, or a lost duplication,
  marks the run invalid. Every run here was valid.
- **Screen time.**
  - A screen update is a duplicated frame whose dirty region touches the window and that shows a new 16-bit marker
    (22 cells of 4 px drawn at the window's top-left corner).
  - Its time is the first vblank on DWM's grid at or after DXGI's LastPresentTime. Raw present times jitter across
    the refresh period, so their differences do not count refreshes.
  - When several presents fall before one vblank, only the last one counts.
  - A missed refresh is any refresh with no new content: an interval of k periods counts k - 1.
- **UI CPU per frame.** QueryThreadCycleTime on the GUI thread from one vblank tick to the next. It is calibrated
  against QPC, and it includes work that Qt defers to the event loop: QRhiWidget renders and presents on the
  window's next update, not inside `repaint()`.
- **B5** is a synthetic QKeyEvent sent to the view. It skips the Windows input queue, which costs about 0.1 ms.
- **B1.**
  - Process start is the QPC reading just before CreateProcessW. The window is placed with STARTF_USEPOSITION over
    a solid topmost backdrop.
  - Five pixels inside the client area are read from every duplicated frame. The first frame is the first one in
    which three of them leave the backdrop colour (the start of DWM's fade-in); "settled" is when they reach the
    idle colour.
  - The cold proxy copies frost.exe and its DLLs with COPY_FILE_NO_BUFFERING into a new directory, so FrostIDE's
    binaries are not cached and have no prefetch trace. System DLLs stay cached.
- **Fixed during the spike.** The first editor series measured about 6.6 ms "CPU" for every backend and size. The
  cause was the harness: `processEvents(WaitForMoreEvents, maxTime)` drops WaitForMoreEvents and spins. The harness
  now blocks in the one-argument overload, and the numbers above were re-measured.

### Findings about the platform
- **DWM counters are synthetic on build 26300.** In DwmGetCompositionTimingInfo with a null window, cRefresh and
  cFrame grow by exactly one per call, and cFramesMissed, cFramesDropped and cFramesLate stay 0. Only the refresh
  period and qpcVBlank are real. Missed compositions therefore come from the duplication markers.
- **DWM ETW is deferred.** A real-time Microsoft-Windows-Dwm-Core session needs elevation or Performance Log Users
  membership, and agent sessions run at medium integrity. The duplication markers measure the same quantity, and
  their validation is above.
- **GDI windows lose frames when DWM is busy.** A trivial 320x48 GDI window that paints once per vblank gets only
  74–85 % of refreshes on screen. Full-output dirty frames from the wallpaper make DWM skip the next composition.
  The flip-model swap chain behind QRhiWidget loses about 6 % in the same conditions.
- **A window shown without activation by a background process opens below the foreground window**, so it is not
  on screen. B1 automation needs either a window it may raise (as here) or a quiet desktop.

## Consequences
- M1 builds `IEditorRenderer` with the QRhiWidget atlas backend as the default on Windows. Raster remains the
  oracle (golden images), the fallback for RDP, WARP and remote sessions, and the backend for small windows if
  needed. Because the backend is fixed before the first frame, the backend choice is made at startup (M1).
- The whole window is RHI-composited, so every widget's backing store is uploaded as a texture. The M1 B6 test
  must include tool windows and docks, not just the editor.
- B1 must include D3D11 device creation, the swap chain and shader compilation. Ship precompiled shaders (qsb
  needs Qt Shader Tools, which requires a licence check and an ADR) or compile HLSL on a worker thread during
  startup (M1).
- B1 warm fails at about 246 ms before any editor work exists: about 219 ms passes before the window is visible.
  M1 adds ETW startup markers (FirstPresent) and profiles DLL loading, the platform plugin and the font database.
- B6's absolute "missed at most 1 %" cannot pass on REF-W1 as it is used, for any backend. `tools/perf.ps1` should
  report each run against a fill-only baseline taken in the same session, and run the gate on a quiet desktop.
  Changing the budget itself needs the user's approval; this ADR does not propose that.
- QRhi is semi-public API in Qt's private Gui package: `find_package(Qt6 COMPONENTS GuiPrivate)` and
  `Qt6::GuiPrivate`. It is the same Qt6Gui library under the same licence. `tools/licensecheck.py` already mapped
  `Qt6::GuiPrivate` targets to Gui; this spike makes it map private `find_package` components the same way.
  Two new tests cover it, one of them checking that an unknown private module still needs an ADR. Private APIs
  may change between Qt minors, so M1 wraps them in one module.
- Port `tests/perf/s1/child_process.cpp` to `frost::io::IProcessLauncher` when m0/io is merged (TODO(M1) in the
  file).

## Alternatives considered
- **Ship raster and accept the miss.** At 5120x1392 every raster frame needs about 5 ms of CPU before any text is
  drawn. That fails B6 by its own cost, not the environment's.
- **Raster with partial flushes** (scroll-blit plus a dirty-region flush). A scroll moves every pixel, so the flush
  is still the whole window; the measurement shows p99 7.9 ms.
- **PresentMon** for on-screen timing. It cannot see raster GDI flushes, and it would add a dependency. The same
  validated probes measure both backends instead.
- **DWM ETW instead of Desktop Duplication.** It needs elevation, so it is deferred; see Findings.
- **A true cold start** (reboot, or a standby-list purge with RAMMap -Et). Neither is possible from an agent
  session. The proxy covers FrostIDE's own binaries only. The real cold B1 is measured at M1 under budgets.json's
  cold conditions.
