<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0043: The QRhi editor renderer (glyph atlas behind IEditorRenderer)

- Status: accepted
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("Editor rendering"), risk T10; ADR 0007 decisions 1, 3, 5 and 8; ADR 0011 A1
  (QRhi approved for Windows); ADR 0037 (the renderer seam). Built in the feature-first phase (ADR 0031).

## Context
ADR 0011 A1 made QRhi M1's editor renderer on Windows: raster misses B6's CPU criterion at the user's 5120x1392
work area, QRhi used about a sixth of it in S1. Raster stays the golden oracle and the fallback for RDP and WARP;
Linux keeps raster. ADR 0037 left the `IEditorRenderer` seam and the backend-neutral `EditorDisplayList` for it.

## Decision
1. **Backend.** `RhiGlyphPainter` turns a display list into instanced quads: glyphs from `EditorGlyphAtlas`, solid
   quads (atlas texel block) for fills, selections, current line, carets, lines and the gutter, discs for markers.
   Runs are shaped with the raster backend's QTextLayout settings; glyphs are rasterised by QPainter at the device
   pixel ratio with four horizontal subpixel steps, so cells match raster's pixels. One render pass, three scissored
   draws (fills; text area; gutter), whole frame each paint (no dirty regions: S1 measured under 1 ms per frame).
   HLSL is compiled at pipeline creation by the D3D11 backend (S1's approach, no qsb). `RhiEditorViewport`
   (QRhiWidget, Direct3D 11) replaces the scroll area's viewport; `RhiEditorRenderer` feeds it each frame's list.
   QRhi's headers come from Qt6::GuiPrivate (QRhi's semi-public API, pinned by the exact Qt version like S1): part
   of Qt Gui under the same licence, so no new dependency (licensecheck maps it to Gui) and no licence ADR.
2. **Choice.** Setting `editor.renderer` (`auto`, `raster`, `rhi`; Application, advanced). `chooseEditorBackend`:
   `raster` is raster; `rhi` is QRhi wherever a backend exists; `auto` is QRhi on Qt's `windows` platform unless the
   session is remote (`SM_REMOTESESSION`) or the adapter is software (WARP: vendor 0x1414/device 0x8c, or a CPU
   device, known once the QRhi exists); Linux and offscreen platforms are raster. Every choice and fallback is
   logged through frost::log with its reason, and `EditorView::rendererState()` exposes it.
3. **Start-up (ADR 0007 decision 3).** Every view's first frame is raster; the QRhi viewport is attached after it
   (queued), so QRhi's start-up stays off B1 and off the first editor frame. This is ADR 0007's first option ("show
   the workspace with raster, create the RHI surface when the first editor opens"); which window-level cost lands on
   B2/B15 is measured in the hardening phase.
4. **Fallback.** QRhiWidget's `renderFailed`, a painter initialisation error or a lost device swap the viewport back
   to raster (queued, never inside Qt's paint) and the view does not retry until `editor.renderer` changes. The
   editor model (document, selections, undo, scroll) is untouched; only the viewport widget changes.
5. **Changes.** Font, size, zoom and ligature changes reset the atlas (`setFont`); a new device pixel ratio (DPI
   change, monitor move) resets it on the next frame; resizes recreate only the colour buffer. A theme change needs
   no rebuild, because the atlas holds coverage and colours are per quad (a deviation from the slice text, which
   listed theme changes with atlas rebuilds).

## Consequences
- Tests (`ui_editor_tests`): the painter renders into an offscreen texture on a D3D11 WARP device at 100 % and
  150 % and is compared per row with the raster image; QRhi's Null backend (Linux) checks the recorded frame and
  skips the pixels. The rule, the atlas, the live setting switch and forced, offscreen and WARP fallbacks are tested.
- The first QRhi viewport in a shown raster window makes Qt switch that window to RHI composition, which is untested
  on screen here (no on-screen runs in this slice) and may flicker once.
- Deferred to the hardening phase: B5/B6/B1/B2/B15 measurements on screen (PresentMon present mode, quiet-desktop
  control, swap-chain frame latency), screenshot goldens of the QRhi backend, sanitizers, an atlas-eviction policy
  (today a full 4096-texel atlas is cleared and refilled), precompiled shaders, re-evaluating `auto` on session
  changes (RDP connect while running), and a Linux QRhi leg (ADR 0007 decision 8).

## Alternatives considered
- **QRhi for the whole window from start-up**: puts about 210 ms on B1 (ADR 0007 decision 3).
- **Warming Direct3D 11 on a worker thread**: may hide the cost from B2/B15 too, but needs the B1/B2/B15 harness to
  show it helps; left for the hardening phase.
- **Dirty-region rendering**: QRhiWidget redraws its texture anyway, and a full frame cost under 1 ms in S1.
- **Rasterising glyphs with QRawFont::alphaMapForGlyph**: its image offset relative to the origin is not exposed,
  so cells would not line up with raster's glyphs.
