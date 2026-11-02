<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0057: CMake in the workbench

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 (CMake), section 7 (W1); ADR 0044, ADR 0046

## Context

The CMake service and its File API model, profile controller and tool window existed independently of the
application. A folder containing `CMakeLists.txt` needs the same workbench paths for trust, saving, builds,
diagnostics and source navigation as an MSBuild solution, while retaining CMake's own profiles and targets.

## Decision

1. Opening a CMake folder creates a CMake service, profile tool window and target tree. A folder containing one
   solution still follows the existing solution selection rule. File activation uses the workbench's normal
   open path; target context actions can build a target. The profile selection is persisted separately.
2. A CMake build backend connects the existing Build actions, Save All, trust prompt, output and Problems view
   to the service. Configuring and building remain behind the project's trust gate. Watcher batches are passed
   to both the navigation workbench and CMake so CMake inputs can trigger reconfiguration.
3. clangd uses the active profile's build-directory compilation database and restarts when the CMake model
   changes. CMake generators that do not produce `compile_commands.json` cannot supply it through this path.
4. Run builds the selected executable target, or the first executable when none is selected, and starts its
   File API artifact in the Run console. Input, output and Stop use `RunSession`. A per-request completion
   belongs to that build, and carries the model captured at completion. A profile generation change rejects a
   delayed completion; it cannot launch an artifact from another profile. Shutdown cancels the CMake job,
   invalidates pending Run completions and reruns, and stops and joins a running session.
5. Queued events from a backend that is no longer active are dropped on the UI thread. Closing a project while
   its CMake build or program is active is refused until it stops, preserving its tabs and project state.

## Verification and limits

In the isolated integration tree, the Windows Debug targeted build passed, followed by 13/13 focused tests on
a hidden desktop. The real CMake app tests cover trust, configuration, the target tree, compilation database,
diagnostic navigation, build and Run, console input, profile changes and persistence, a delayed older build
completion, a profile change before Run completion, shutdown with a queued completion, and shutdown of a
running program. A separate service test checks the per-request completion and model. One earlier run stalled
in the failed-build phase and was interrupted before its cause could be captured; later runs passed, but that
stall remains unexplained. A deterministic shutdown test holding a build in progress remains useful. Native
Linux application verification is pending; the Windows result alone does not establish cross-platform success.
