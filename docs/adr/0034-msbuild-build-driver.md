<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0034: The W1 MSBuild build driver

- Status: accepted (feature-first phase, ADR 0031); decisions 3 (language) and 4 (what ends after MSBuild
  exits) superseded by ADR 0035
- Date: 2026-11-01
- Plan reference: PLAN.md section 3.2 (src/build), section 3.5 (trust, launching); ADR 0032 (W1); ADR 0011 (E2)

## Context
W1 needs Build, Rebuild and Clean of a user's `.sln`/`.slnx` through the installed MSBuild, with output and an
error list, before M4's full build system (MSBuildLogger, Build window). frost::build is new, so its first
choices are recorded here.

## Decision
1. **Discovery** runs `vswhere.exe` from the Program Files (x86) known folder with `-sort -prerelease -products *
   -requires Microsoft.Component.MSBuild -format json -utf8`. `-sort` replaces `-latest`, which returns one
   instance only: the list keeps every instance, newest first, the order `-latest` picks from, and the default is
   the first one whose `MSBuild\Current\Bin\amd64\MSBuild.exe` exists. `instanceId` overrides it.
2. **No JSON dependency.** A ~250-line private reader in `src/build/src/json.cpp` parses vswhere's output.
3. **UTF-8 output comes from `DOTNET_CLI_UI_LANGUAGE`**, set to the user's Windows UI language (or a chosen one).
   MSBuild 17.14 switches its console to UTF-8 only under that override; without it, output arrives in the console's
   OEM code page (measured below). Invalid bytes that still arrive become U+FFFD.
4. **The process tree ends with the build.** MSBuild runs in frost::io's Job Object with `-nodeReuse:false` and
   `MSBUILDDISABLENODEREUSE=1`. When MSBuild exits, the session terminates whatever is left in the job (a PDB
   server or another straggler that inherited the output pipe can hold it open long after the build).
   `_MSPDBSRV_ENDPOINT_` is unique per build, so that PDB server is the build's own and ending it cannot break a
   concurrent Visual Studio build. Cancellation terminates the job.
5. **Trust is an explicit input.** `ErrorCode::Untrusted` joins frost::core (appended after `Internal`, so earlier
   values keep their numbers); `startMsBuild` and `BuildSession::start` refuse without `trusted`.
6. **Diagnostics** come from a hand-written parser of MSBuild's canonical format (`origin: [subcategory] category
   [code]: text [project]`), not from a logger, until FrostIDE.MSBuildLogger (M4). The console logger runs with
   `NoSummary;ForceNoAlign;DisableConsoleColor` and `-v:minimal`; duplicates are dropped.
7. **Environment:** `NoDefaultCurrentDirectoryInExePath` is removed for builds (ADR 0011, E2) and
   `MSBUILDTERMINALLOGGER=off` keeps the format stable.

## Measurements
REF-W1, VS 2022 Build Tools 17.14.37710.0, 2026-11-01, the tiny fixture (`tests/build/fixtures/tiny_sln`) in a
directory named `exp é x`: without the override the `é` arrived as byte 0x82 (CP437), with
`DOTNET_CLI_UI_LANGUAGE=en-US` as C3 A9. The six real-MSBuild tests (discovery, five builds) take 6 s together;
vswhere takes about 50 ms.

## Consequences
- Builds show MSBuild's messages in the user's UI language, in UTF-8. Localized category words are not parsed
  yet (I12, M4).
- Every build starts a fresh PDB server, which costs a little time on large solutions. TODO(M4): measure it.

## Alternatives considered
- `-latest`: hides the other instances the user may want to pick.
- nlohmann/json or another JSON library: a dependency (and an ADR and licence check) for one small input.
- A console with code page 65001 for MSBuild: frost::io starts children without one from the GUI.
- Leaving stragglers alone after MSBuild exits: an inherited pipe would keep the build from finishing until the
  straggler exits, and processes would outlive the build.
