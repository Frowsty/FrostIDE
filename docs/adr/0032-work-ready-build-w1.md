<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0032: A work-ready build (W1) before the rest of U1

- Status: accepted (the user's requirement statement, 2026-11-01)
- Date: 2026-11-01
- Plan reference: PLAN.md section 7 (roadmap, "First usable build"); ADR 0029 (thin path); ADR 0031 (feature-first)

## Context
The user wants FrostIDE to replace Visual Studio for their daily work, since Rider needs a paid licence for
commercial use: "I want a usable IDE to replace my current Visual Studio even if it has quirks and bugs which we
will iron out with time." About that work: "My work is currently smaller projects utilizing an internally
developed SDK. I have a new-project powershell script which generates the new project with your desired modules,
it generates cmake files, command-list file etc and visual studio files. It also makes sure everything is linked
for all of these. So basically what I need is something that can just open the .sln/.slx files, show my source
files, let me edit them, show errors/warnings etc and let me compile".

## Decision
1. **W1, the work-ready build, comes first.** It contains:
   - M1's editor and shell;
   - opening a `.sln` or `.slnx` from the command line, File > Open and the recent list;
   - a Solution Explorer built from static readers of `.sln`, `.slnx`, `.vcxproj` and `.vcxproj.filters`, without
     MSBuild evaluation yet;
   - editing and saving;
   - Build, Rebuild and Clean of the solution or one project through the installed MSBuild (found with
     vswhere), with Configuration|Platform taken from the solution;
   - Save All before a build, and a trust prompt before the first build of a folder;
   - the build output, plus an error list of errors and warnings with click-to-jump and marks in the editor.
2. These pieces come forward from M2 (readers, trust) and M4 (build driver, error list) in minimal form. Their full
   versions stay in M2 and M4. U1's content is unchanged (ADR 0029, including the debugger and no thinning).
3. **After W1:**
   - clangd errors and warnings as you type, from compile commands (the generated CMake side's, or derived from
     the projects);
   - then navigation and completion (M3's core);
   - then the rest of U1.
4. **Acceptance:** open, browse, edit and build a small generated solution of several linked projects with the real
   MSBuild on REF-W1. The tests use a synthetic fixture in `tests/` shaped like the user's generated projects; the
   user's internal SDK is never committed.

## Consequences
- FrostIDE becomes usable for the user's work long before U1.
- The static readers miss items that imported `.props`/`.targets` files add. Path (c)'s MSBuild evaluation (M2)
  closes that gap.
- Errors appear after a build at first; live diagnostics follow right after W1.

## Alternatives considered
- Build U1 in milestone order first: the user's work needs only a fraction of it, so it would come months later.
