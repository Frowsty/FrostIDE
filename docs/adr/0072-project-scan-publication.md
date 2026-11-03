<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0072: Project scan publication and file tree

- Status: accepted functional slice (2026-11-03); remaining limits below
- Plan reference: B5, B13, B23; ADR 0042

## Context

A staged Unity scratch project produced a 2678 ms GUI-thread stall in scan publication on a loaded
Debug host. Enumeration was asynchronous, but path indexing, sorting, item allocation and Git color
updates ran on the GUI. This observation is not a controlled B5 measurement.

## Decision

Prepare the project tree, lookup index and solution-only candidates on scheduler workers. Publish
immutable snapshots only for the current project generation. The Qt model exposes node IDs on demand;
Git colors are requested when rows are drawn. Old snapshots and obsolete search results are retired on
workers. Initial solution candidates have their own generation-checked worker so they can arrive before
an enumeration completes. Active speed-search input is matched on a worker against a snapshot and
query generation. Obsolete results cannot activate or navigate old selections. Expansion restoration
and search-driven expansion proceed in bounded batches; project changes invalidate old restore paths.

## Limits and verification

Individual standard-library sorts cannot observe cancellation until the sort ends. Enumeration of
solution model file nodes in `openSolution` remains on the GUI; the direct count-returning speed-search
API scans names synchronously. The cost of one Qt expansion with a very wide sibling set is not yet
measured. No numerical budget is changed or claimed by this decision. Focused Windows tests exercise
selection, cancellation/generation, search, color locality and expansion batches. Astra approved worker
tree `742005482b029f537d5468bc5fb851e518d5c902` after corrections for stale activation, synchronous
expansion restoration and last-owner snapshot disposal. A timer tick can perform 64 search expansions
and 64 restoration attempts; the bound is on attempts, not on the cost of a single Qt expansion.

The combined main tree passed 2748/2767 Windows tests (19 skipped, one additional disabled) and
2621/2645 native Linux tests (24 skipped), plus formatting, lint, licence and feature consistency checks.
The fresh Windows stage passed startup, Direct3D 11 rendering and ConPTY input. An actual staged-window
probe opened the existing Unity scratch project and source at the requested Unicode location, preserved
source/meta bytes and closed normally. Its 15-second observation after first frame recorded 75 responding
WM_NULL probes and no freeze reports. This does not establish scan completion or numerical B5 compliance;
the deterministic tests separately cover scan publication and stale generations. Broader navigation
and solution-model responsiveness remain required product work.
