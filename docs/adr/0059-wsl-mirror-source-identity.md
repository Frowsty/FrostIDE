<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0059: Preserve source change ordering in WSL mirrors

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 6.4; ADR 0027 (local verification)

## Context

`tools/verify.sh` mirrors each Windows worktree onto ext4 and keeps that mirror's incremental build trees.
The former `rsync -a` copied the source mtime onto every replacement. A replacement from an older checkout
could thus have an mtime older than a previously built object even though its contents changed. Ninja then
considered that object current. On the 2026-11-02 combined verification, a changed `RhiGlyphPainter` layout
was compiled into the editor library while an older test object kept the old layout. Three Linux renderer
tests segfaulted. A hardware watchpoint showed the constructor's initialization overwriting the caller's
return address; the old caller object was preserved as `build/linux-clang/editor_rhi_test.before-debug.o`
in the affected mirror. Rebuilding the dependent tests made all three pass. Linux sccache was off.

## Decision

The mirror operation uses `rsync -a --checksum --no-times`: it compares file contents even when sizes and
times match, and a copied file gets the current time rather than an older source timestamp. Identical files
are not copied solely for a timestamp difference, retaining incremental builds. The existing exclusions and
`--delete` remain. `test_verify_scripts.py` executes the production mirror function against changed files
with older and equal timestamps, an unchanged file with a different timestamp, an obsolete file and the
excluded build directory.

This change prevents further stale objects from this cause; it does not repair objects already built from a
previously imported snapshot because the current mirror's contents may already match. Before relying on an
existing affected mirror, clean and rebuild its native build targets once, without deleting the source mirror
or dependency tree. Subsequent runs retain incremental builds. The checksum comparison reads both trees and
therefore adds I/O to each sync; measure its impact if verification time becomes a concern.
