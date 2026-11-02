# FrostIDE: Plan to v1.0

**Scope update (2026-11-02, user decision; ADR 0054).** The product goal now includes independently
implemented Rider feature and workflow parity, CMake, first-class Lua, Unity integration and FrostEngine
integration. The earlier C/C++-only positioning, Unity exclusion and deferred C# scope below are the
historical baseline, superseded where they conflict with this decision. W1 and U1 remain delivery
checkpoints, not the whole goal. Appendix B records the added acceptance tracks; the original milestone
estimates and Appendix A counts do not estimate or cover that additional work. Original source, icons,
themes and branding remain required. Implementation uses GPT-6.1 Terra agents with GPT-6 Astra review.

## Plan at a glance

**Status (2026-10-31).** The user answered the before-M0 and before-M1 decisions (§9) and authorised development; M0 is in progress. No CI/CD of any kind is used (D24): every build and test runs locally on REF-W1 and in WSL on it. **2026-11-01, evening:** M0 is signed off and M1 is under way. The user chose a thin path to a first usable build (D29, ADR 0029): M1–M4 shrink to the cores that real C++ work needs, the first usable build U1 is the M4 exit (around month 10–12), and the deferred halves move to M15–M17, which run before Preview 0.1. The user answered ADR 0029's questions the same evening ("I agree with everything except 4 and 5. I don't care for those savings"), so ADR 0029 is accepted: U1 also has a minimal debugger, moved from M6 to M4, and U1 is not thinned further. The same evening the user put W1, a work-ready build for their MSVC solutions, first inside M1–M4 (ADR 0032), and started a feature-first phase that runs until U1, with a hardening phase after it (ADR 0031; §7, "W1 first" and "Feature-first phase").

**Product.** A cross-platform C/C++ IDE for Windows 11 x64 and Linux x64 (macOS is unsupported until a test leg exists, D24; the code keeps its platform seams so it can be added later), with Rider's layout and behaviour (Islands-style shell, tool-window stripes, Search Everywhere, Alt+Enter) under its own name, icons and themes. Full-fidelity MSVC solutions (.sln, .slnx, .vcxproj) on Windows, CLion-grade CMake, native Claude Code integration. Written in C++23 (C++26 mode, allowlisted) by Claude Code in tested vertical slices, with user sign-off at every milestone.

**Key decisions**
1. **GUI.** Qt Widgets, LGPLv3, dynamically linked, tracking the latest Qt minor (6.12 now, 6.13+ later). Qt 6.12 is the last Qt for Windows 10, so FrostIDE is Windows 11 only (D14, decided), at the lowest build that both Qt's current minor and our VM test leg support: 22631 today, re-checked at each Qt minor (§1.3). Raster editor painting; a QRhi backend only if the backend-neutral S1 gate fails at 5120x1440 @ 144 Hz.
2. **Compiler.** Clang only for our own code (clang-cl with the MSVC STL, clang with libstdc++; Apple Clang if macOS is ever added). cl.exe is a first-class target for user projects and a test oracle.
3. **LLVM cadence.** Start on 23.1.x; adopt each major within ~6 weeks of its .1 release behind the corpus-differential gate; a local `llvm-next` build leg run on demand (no CI).
4. **Semantics.** Stock clangd at M3. From M7, frost-clangd: unpatched clangd plus its extension points.
5. **Analysis.** Five latency tiers, a registry generated from the pinned LLVM's tables (baseline count produced in M0; count and coverage targets gated at M7, M9 and M13), SARIF ingestion; openly below ReSharper C++ parity in v1.
6. **MSBuild.** Native instant tree and fingerprinted cache; design-time builds in an out-of-process C# ProjectHost that never starts on a warm reopen of an unchanged tree; MSBuild CLI fallback, which is also the evaluator of the first usable build until ProjectHost lands in M16 (ADR 0029); `cl.exe /c` as independent oracle.
7. **CMake.** Stateful File API, presets v2–v13, a CMakeLists language service and the CMake debugger.
8. **Debugging.** Our own DAP client: lldb-dap (from U1, M4, built from the pinned LLVM sources on both operating systems), GDB ≥16 (M6) and our own DbgEng adapter (M10). Never vsdbg.
9. **"Sub-second".** First frame ≤150 ms warm; interactive (L1) ≤700 ms p50 / ≤1 s p99 on a warm reopen. L0–L3 have warm-reopen budgets; a first-ever open promises L0 and the trees (B23); L4 (full index) is tracked, not gated.
10. **Processes.** One UI process with a Qt-free core; engines out of process under a supervisor and resource governor.
11. **Product infrastructure from M1.** Settings scopes, safe save and hot exit, local crash reporting with bounded retention, a trust model that also covers project-shared settings, packages from M5, an updater from M6, no telemetry and no network before L1.
12. **Claude Code.** C1 IDE bridge (M5), C2 MCP server and hooks (M8), C3 native chat (M12, refusing subscription sign-in in code until Anthropic approves; terminal mode unaffected). ACP after 1.0.
13. **Working agreement.** One PR per slice, merged after green local verification on Windows and WSL (no CI), independent review of core code, a demo pack and explicit user sign-off at each milestone exit.
14. **Thin path (D29).** A first usable build (U1) at the M4 exit: open, browse, edit and build a CMake project or an MSVC solution, with highlighting, data safety, the IntelliJ keymap, clangd completion, diagnostics and navigation, clickable build errors, Run, a minimal debugger (line breakpoints, stepping, stack, threads, variables, watches; MSVC-built programs through their PDBs) and a terminal (where Claude Code runs until the C1 bridge), fast and safe (§7, "First usable build"). The polish that U1 does not need follows in M15–M17, before Preview 0.1 (ADR 0029).

**Milestone ladder.** 138–168 units (129–156 for the milestones plus the upgrade track's 9–12) plus 10% contingency is about 152–185 weeks, roughly 2.9–3.6 years when run sequentially. Levers that shorten it are in §7. Month 1 is November 2026. The rows are in the order the milestones run. Milestone IDs are identities, not positions: code markers (`TODO(Mn)`), ADRs and the matrix use them, so the deferred halves of M1–M4 got the new IDs M15–M17, which run between M5 and M6 (ADR 0029).

| # | Milestone | Units | Ends around month |
|---|---|---|---|
| M0 | Bootstrap (signed off 2026-11-01) | 3–4 | 1 |
| M1 | Editor, shell and data-safety core | 8–9 | 3–4 |
| M2 | Project models and trust | 9–10 | 5–6 |
| M3 | C++ intelligence core | 7–8 | 7–8 |
| M4 | Build, Run, minimal debugger, Terminal (**first usable build U1**) | 9–12 | 10–12 |
| M5 | Git, Claude C1, packages (**dogfood v0.1**) | 8–10 | 12–14 |
| M15 | Editor and shell completion | 6–7 | 14–16 |
| M16 | ProjectHost, solution writes, C#, CMake language | 9–11 | 16–19 |
| M17 | C++ intelligence completion | 5–6 | 17–21 |
| M6 | Debugger completion, test runner, installers, updater (**Preview 0.1**) | 9–11 | 20–24 |
| M7 | Analysis tiers | 10–12 | 23–27 |
| M8 | Claude C2 | 3–4 | 23–28 |
| M9 | Refactoring, code generation, templates | 11–13 | 26–32 |
| M10 | Debugger v2 and full test runner | 9–11 | 29–35 |
| M11 | UX parity, importers, accessibility | 9–11 | 31–38 |
| M12 | Claude C3 native chat | 5–6 | 33–40 |
| M13 | Scale hardening | 5–6 | 34–41 |
| M14 | v1.0 | 4–5 | 35–43 |
| — | Dependency-upgrade track (5–7 LLVM majors, 5–7 Qt minors, quarterly vcpkg) | 9–12 | throughout |

---

## 1. Product definition and scope

### 1.1 Requirements as interpreted
| Requirement (user's words) | v1.0 delivery | Redefined? |
|---|---|---|
| "Rider clone… GUI of Rider is perfect" | Rider's layout, tool windows, popups and an IntelliJ-compatible keymap; own name, themes ("Glacier") and icons, subject to name clearance (L8); no JetBrains logos, icons, names or theme files (JetBrains Mono under the OFL). | Yes: no JetBrains assets |
| "feature complete" | Every P0 and P1 row of the 257-row matrix (the 249 researched rows plus 8 new ones), minus the v1 slices in §1.4. Appendix A maps every row to a milestone. | Yes: measurable |
| ".sln and .slnx" | Full fidelity on Windows through design-time builds against the user's VS. Labelled approximate mode on Linux. | Partly |
| "CLion's support for CMake" | File API, presets v2–v13, profiles, CTest, a CMakeLists language service (M16), the CMake debugger (M6) and a cache viewer (M16). CLion's WSL and remote toolchains move to 1.x. | Partly (D5) |
| "cross platform" | Windows 11 x64 (build 22631+ today; the floor is the lowest build a test leg covers, §1.3), Linux x64 (glibc ≥2.39). A platform counts as supported only if a local test leg (REF-W1, its WSL, or a VM on it) covers it; there is no CI (D24). macOS is unsupported until a test leg exists; platform seams stay so it can be added later. arm64 on Windows and Linux in 1.x. | Yes: no Windows 10 (D14), no macOS in v1 (D24) |
| "sub second launch times" | The budgets in §4. Sub-second means L1 on a warm reopen. A first-ever open promises L0 plus the project trees available in restricted mode (B23, with its preconditions). | Yes |
| "best in class static code analysis" | Clang-based tiers T0–T4. The registry exposes every configurable Clang warning and primary clang-tidy check of the pinned LLVM, ≥100 CSA checkers and ≥60 frost checks. The total is generated from the pinned LLVM's own tables, never borrowed: LLVM 23.1.4 has 406 primary tidy checks, and the configurable warning-group and CSA counts are measured in M0 (no total is claimed before then). Coverage target: ≥75% of ReSharper C++'s ~296 own inspections. ReSharper C++ lists 2,237 inspections, so v1 is below parity. | Yes: counted, below parity |
| "C++23 with C++26 features" | Own code: C++23 baseline plus a C++26 allowlist and a compat layer. The two C# helpers (ProjectHost, MSBuildLogger) are a disclosed exception (D3). User code: the row 118 slice (§1.4). | One exception |
| "always test… make sure everything works" | Verify gates on every change, independent oracles, executable exit criteria, user sign-off per milestone (§6.7). | No |
| "maintainable and documented" | Ordinary code comments in every language in the repository (§6.6), enforced by frost-lint and reviewed at each milestone exit. | No |
| "Claude Code integration" | C1–C3 (§5); C4 (ACP) after 1.0. | No |
| "then we can take it from there" | Working agreement (§6.7): a demo pack and explicit sign-off before each next milestone. | No |

### 1.2 Positioning, C#, v1.0 contents
**Positioning.** Visual Studio is Windows-only; CLion has no MSBuild model; Rider opens C++ .sln only on Windows and C++ .slnx on all three operating systems (marked experimental). FrostIDE gives full .sln/.slnx/.vcxproj fidelity on Windows through the user's own MSBuild, a labelled approximate mode on Linux, CLion-level CMake and Rider-grade UX on both supported operating systems (macOS later, if a test leg is provided).

**C# (historical baseline, expanded by ADR 0054 and Appendix B).**
- **v1.0, solution citizenship:** C# projects in the Solution view; build, run via launchSettings.json and navigation to build errors (all M16, ADR 0029); tree-sitter highlighting (M1).
- **Original v1.1 proposal, no longer a scope limit:** roslyn-language-server, netcoredbg, ICSharpCode.Decompiler, MTP. The former exclusions of ReSharper-grade C#, Razor, XAML, MAUI and Unity do not limit the expanded Rider-parity goal. Engine and dependency choices require their own compatibility and licence decisions before implementation; this list is not evidence of delivered C# support.

**v1.0 contents.** Editor and shell; all project models; C++ intelligence; analysis tiers T0–T4; at least 8 multi-file refactorings; formatter; build and run; debugger; a test runner for GoogleTest, Catch2, doctest, Boost.Test and CTest; Git; terminal; local history; Claude C1–C3; settings import; updater; accessibility gate; signed installers.

### 1.3 Non-goals and platforms
**Non-goals.** MSVC Edit and Continue; vsdbg, cppvsdbg, cpptools; JetBrains assets, names and plugin compatibility; an Unreal project model (post-1.0; UE-generated NMake solutions open as ordinary .sln); remote development and WSL, Docker or remote toolchains (1.x); MSVC IFC modules, kernel debugging, full Objective-C, ML completion; database tools, HTTP client, embedded; translations (code is i18n-ready), telemetry, app stores, Windows 10, macOS in v1 (D24), CI/CD services of any kind (D24).

**Platforms.** A platform is supported only if a test leg covers it. All test legs are local: REF-W1, WSL Ubuntu 24.04 on it, and VMs or containers on it (D24).

| OS | Supported | Test legs |
|---|---|---|
| Windows | 11 x64, at the lowest build that both Qt's current minor and our VM leg support: 22631 (23H2) today, re-checked at each Qt minor. Windows 11 23H2 servicing ends for all editions on 2026-11-10 (Home/Pro already ended in Nov 2025), so if no 23H2 evaluation image can be obtained, the floor becomes the build of the oldest obtainable image (24H2, 26100, or 25H2). Release notes state the build actually tested. | REF-W1 (Windows 11 Home 26300) via `tools/verify.ps1`; a VirtualBox VM at the floor build on REF-W1, stored under E:\vm (~64 GB, never on C:), for install smoke tests at M6 and M14 |
| Linux | x64 with glibc ≥2.39 and libstdc++ ≥14 (the Ubuntu 24.04 floor). X11 and XWayland; native Wayland is best effort. | WSL Ubuntu 24.04 on REF-W1 (E:\WSL\Ubuntu-24.04) via `tools/verify.sh`: the floor leg itself; optional local containers (podman in WSL) for a headless weston smoke and the AppImage on Fedora, Debian, Arch and openSUSE, once set up |
| macOS | **Unsupported until a test leg exists** (D24: no CI, no cloud Mac). Platform seams (NSTextInputClient, NSAccessibility, FSEvents, kqueue, menus) stay in the design so macOS can be added if the user provides a Mac; until then no macOS behaviour is claimed or tested. | none |

### 1.4 Tiering
| Tier | Meaning | Rows |
|---|---|---|
| P0 | By Preview 0.1 (M6), except the five rows listed below | 86 |
| P1 | Required for v1.0 | 85 |
| P2 | 1.x | 47 |
| P3 | Later | 39 |

Counts include the moves below and new rows 250–257 (soft wrap, whitespace/indent guides and editor zoom are P0; rulers, clipboard history, drag and drop, fold persistence and the native chat panel are P1). Infrastructure rows I1–I20 (Appendix A) are tracked alongside.

**Scope moves.** P1 → P0: the Claude bridge (232) and trusted project (25, now restricted mode). P2 → P1: .sln↔.slnx conversion (3), CMake cache viewer (13), Move (141), CMake debugger (193), quality-gate hooks (234). WSL, Docker and remote toolchain detection (part of 20) and row 223 stay in 1.x (D5). Row 4 means design-time build plus native tree.

**P0 rows finishing after Preview 0.1** (any other P0 slip needs user approval). The thin path (ADR 0029) moved parts of these rows to M15–M17, which run before M6, so no further P0 row slips past Preview 0.1:
- Row 31, Explorer context actions: Set as Startup Project, Open in Explorer and Copy Path in M2; Build/Rebuild/Clean and Open in Terminal in M4; Compare With in M5; Unload/Reload project and Properties in M16; Run Unit Tests in M6; Show History in M11 (it needs the Local History UI and file history, rows 218 and 209).
- Row 99, inspection framework: severities and suppression in M17; profiles and scopes in M7.
- Row 118, C++23/26 language: C++23 in M3; the modules flag and reflection downgrading in M7.
- Row 227, keymaps: IntelliJ (Windows/Linux; the macOS variant is defined but untested, D24) in M1; Visual Studio, VS 2022, ReSharper and VS Code keymaps in M11.
- Row 242, status bar widgets: line:col, line separator, encoding, read-only lock, indent and processes in M1; the error indicator in M3; column mode in M15; the navigation bar in M17; the analysis monitor in M7 with solution-wide analysis (row 111).

**v1 slices of large rows.**
- **Row 20, toolchain detection:** MSVC (VS 2022/2026), clang-cl, MinGW-w64, Clang, GCC and custom compilers in v1; WSL, Docker and remote-host toolchains in 1.x (D5).
- **Row 118.** C++23 plus the C++26 subset the pinned Clang supports at release. C++20 modules behind clangd's `--experimental-modules-support`. C23/C2y features as Clang supports them (#embed is listed for Clang 24). Reflection (P2996), contracts and constexpr exceptions are a documented gap: errors in constructs Clang cannot parse are downgraded to a "not supported by the current engine" hint.
- **Row 102, inspection catalogue:** ≥60 frost checks plus the full Clang, tidy and CSA mapping.
- **Row 103, data flow:** CSA and tidy, plus 5 custom CFG checks.
- **Row 159, run configurations:** remote GDB/LLDB and core-dump types follow rows 190 and 188 (P2).
- **Row 176, data renderers:** natvis via DbgEng plus LLDB STL formatters; natvis on LLDB in 1.1.
- **Row 233, MCP server:** a stdio proxy for Claude Code; HTTP transports and other agents in 1.1.

---

## 2. Key technology decisions

| Decision | Choice | Why | Rejected |
|---|---|---|---|
| GUI toolkit | Qt Widgets, LGPLv3, linked dynamically, tracking the latest minor (§2.3). Per-platform runtime module allowlist: Core, Gui, Widgets and the platform plugin; Linux adds DBus, XcbQpa, WaylandClient and the IBus input-context plugin. The licence check keys each module on its LGPL/GPL status from the SBOM, so GPL-only modules (Charts, Canvas Painter) fail local verification. Svg only in the icon baker, Test only in tests. No QML, no Qt Network (`IHttpClient`, §2.2). Never delay-loaded (/DELAYLOAD breaks the staticMetaObject data import). | Native IME (TSF, NSTextInputClient, text-input-v3), accessibility (UIA, NSAccessibility, AT-SPI), DPI and menus; deterministic offscreen tests. Tracking minors keeps open-source patches timely; later LTS patch releases are commercial-only. | 6.12 LTS to keep Windows 10. A custom D3D11/Metal toolkit (rebuilds IME, accessibility, DPI). Electron. QML. ImGui. |
| Editor rendering | Custom `EditorView : QAbstractScrollArea` over a **layered display map** (folds → soft wraps → inlays → block decorations), each layer mapping rows in O(log n) through sum-tree summaries, so sticky lines, inlays and wrapping never force a rewrite. A backend-neutral display list behind `IEditorRenderer`/`ITextShaper`. The raster QPainter backend (QGlyphRun cache) ships first and stays the golden oracle and RDP fallback; a QRhiWidget glyph-atlas backend is built only if S1 fails. **S1 gate** = B6 plus B5 and warm B1. On-screen timing comes from DWM frame timing (DwmGetCompositionTimingInfo, DWM ETW events) and a Desktop Duplication capture probe; PresentMon only compares the QRhi backend. | Raster Qt flushes through GDI with no swap chain, so present-based tools see nothing; these probes work for either backend. QRhiWidget composites the whole window, so the backend is fixed before the first frame. | GPU by default. QPlainTextEdit. Qt Creator's editor (GPL). |
| Text buffer | Persistent, ref-counted B+tree of pieces; clean-room design from the published descriptions of VS Code's piece tree (MIT) and Zed's sum_tree (Apache-2.0). Summaries: bytes, newlines, UTF-16 units, chars, longest line. O(1) snapshots, anchors, a transactional undo tree excluding IME preedit, UTF-8 storage. Originals ≥128 MB are mapped, smaller ones copied. Grapheme-cluster caret movement (UAX #29, utf8proc). | Lock-free readers on snapshots. Mapped files block git checkout and builds on Windows. | Gap buffer. Zed's rope crate (GPL). |
| Syntax | tree-sitter 0.27.1 (ABI 15); pinned grammars C, C++ 0.23.4, C# 0.23.5, CMake, XML, JSON, Markdown; own queries and corpora. Off above 50 MB and always above 4 GiB (uint32 offsets); a SIMD line index for huge files. | Under 2 ms incremental; ready before clangd. | TextMate grammars. |
| Theming | `FrostStyle` (QProxyStyle over Fusion) compiles the `.theme.json` schema (colors, `ui` with `*` wildcards, `icons.ColorPalette`) to a blob; imports `.icls`. **Glacier Dark/Light** with Islands-equivalent geometry (10 px radius, 4 px gaps; compact 8/3 px), inactive alpha 0.44, tested MainWindow.background contrast ≥1.20:1. Also Glacier Classic, High Contrast, Sync with OS. | Imports existing themes; loads in microseconds. | Qt stylesheets. JetBrains theme files. |
| Icons and fonts | Own SVG icons (16/20/14 px grid, `_dark` variants) baked into atlases at scales 1, 1.25, 1.5, 1.75, 2, 2.5 and 3; other scales downsample the next larger atlas. Lucide (ISC) as stopgap. JetBrains Mono 2.304 and Inter 4.1 (OFL; Inter verified in M1). | No SVG parsing at startup; crisp at fractional DPI (Qt 6 defaults to PassThrough). | intellij-community icons. |
| Compilers and STL (own code) | **Clang only (D2).** **Windows:** clang-cl and lld-link from the pinned LLVM (23.1.x now) on E:, against MSVC STL 14.44 (VS 2022 Build Tools 17.14 on REF-W1), pinned in `sdk/sdk.lock.json`; the pair moves to 14.52 after its November GA (needs a VS 2026 Build Tools install, which requires elevation by the user). **Linux:** clang with libstdc++ 14 (the floor, in WSL Ubuntu 24.04); libstdc++ 15 is optional; GCC 16.2 as second compiler is optional and deferred until a local gcc:16.2 container is set up in WSL. **macOS:** none in v1 (D24). `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` is never defined. | One sanitizer set, `-Wthread-safety`, the C++26 allowlist; the LLVM engines need this toolchain anyway. STL ≥14.52 rejects Clang <22 (STL1000), so compiler and STL move as a pair. S4 proves the link. | cl.exe for own code: no documented C++26 core features (no allowlist), no `-Wthread-safety`, no UBSan/TSan, experimental libFuzzer only; it would force strict C++23, a second warning and sanitizer set and a redesign of the verification legs. |
| Warnings | Per-compiler sets in `cmake/warnings`. **clang-cl:** `/W4 /WX` plus `-Wconversion -Wshadow -Wthread-safety -Wglobal-constructors -Wexit-time-destructors`; never `-Wall` (clang-cl reads it as `-Weverything`). **clang:** `-Wall -Wextra -Wpedantic` plus the same explicit flags and `-Werror`. **GCC 16:** `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` only. | Each compiler gets only flags it understands. | One shared flag line. |
| C++23/26 policy | C++23 baseline compiled as `-std=c++2c` or `/std:c++latest`, extensions off; allowlist plus `frost/compat` (§2.1); Debug and verification builds enable `_MSVC_STL_HARDENING=1`, `_LIBCPP_HARDENING_MODE`, `_GLIBCXX_ASSERTIONS`. | The C++26 DIS ballot was disapproved on 2026-10-23; library floors differ by platform. | Reflection and contracts (GCC 16 only). std::execution (not in MSVC STL). Modules. |
| Build and dependencies | CMake 4.4.5, Ninja Multi-Config 1.13.2, presets v10, PCH, unity builds, sccache. vcpkg baseline 2026.10.16 with overlay triplets: x64-windows-clangcl (/MD, matches Qt), x64-windows-clangcl-fuzz (/MT, Qt-free), x64-linux-clang (arm64-osx only if macOS is added). Binary cache: `files` at E:\vcpkg-cache on Windows and `~/sdk/vcpkg-cache` in WSL. Qt through `tools/qt-fetch.py` into E:\sdk (Windows) and `~/sdk` (WSL), straight from download.qt.io, because aqtinstall 3.3.0 cannot locate Qt 6.12.0 for Windows (S3); 1–2 h source-build fallback. LLVM: official release binaries where S5 shows they suffice (on Windows they lack lldb and lldb-dap); self-built otherwise. `sdk/sdk.lock.json` pins these with SHA-256, plus the MSVC toolset 14.44.35207 and Windows SDK 10.0.26100.0. `tools/bootstrap.ps1` and `tools/bootstrap-wsl.sh` install from the lock file. ProjectHost uses NuGet Central Package Management with `packages.lock.json`; Python tooling uses hashed requirements. | Reproducible on both supported operating systems; fast dev loop (B28). vcpkg's qtbase is still 6.11.2. | Conan. Qt from vcpkg. Bazel. |
| Allocator | CRT allocator globally. Explicit mimalloc 3.6.x heaps inside hot structures. | A global override is unsafe while Qt is a /MD DLL. | Global override. |
| Semantic engine | Stock clangd from the pinned LLVM at M3. **frost-clangd** at M7: stock `clangdMain` + `clangDaemonTweaks`, extended only through FeatureModuleRegistry (`frost/*` methods), the Tweak registry and a `frost-*` ClangTidyModule. No patches, so each LLVM major is a rebuild plus a corpus differential. Settings: UTF-8 positions; index in FrostIDE's cache; clang-cl driver mode via `compilationDatabaseChanges`; FastCheckFilter Loose plus a curated list; `-j` from the ResourceGovernor (default cores/2, low priority); curated suppressions; in restricted mode `--enable-config=false` and no `--query-driver`. | The best open C++ front end, including MSVC compatibility, with no fork. EDG went open source on 2026-09-30; re-evaluated in 2027. | Forking clangd. ccls. |
| Static analysis | **T0:** tree-sitter, under 2 ms. **T1:** clangd diagnostics, include-cleaner, fast tidy and frost checks. **T2:** frost-analyzer, min(4, cores/4) LibTooling processes (governor-scaled) after 2 s idle or on save, for checks clangd disables (misc-const-correctness, use-after-move, unchecked-optional-access, CSA); each with its own PCH cache, ~2 GB cap and timeout. **T3:** Inspect Code: full tidy, CSA CTU (CodeChecker 6.29), IWYU 0.27, optional cppcheck 2.22 and clazy 1.17, MSVC `/analyze`, all as SARIF 2.1.0; headless `frost inspect` for scripted, non-interactive runs. **T4:** index-derived checks (unused, can be final, internal linkage). One registry with stable IDs, Rider-style categories, `frost_cpp_<id>_highlighting` keys and quick-fixes, generated at build time from LLVM's diagnostic tables, the tidy check list and the CSA checker registry. | Cheap checks are instant; expensive ones never block typing. Counts are generated, never borrowed (ReSharper's 974 Clang diagnostics come from its own bundled Clang). | Everything inside clangd. |
| Refactoring | Single-TU: clangd's 20 tweaks plus frost tweaks. Multi-file: index refs → parallel reparse → `tooling::Replacements` → preview → atomic undo. | Real ASTs. | Textual refactoring. |
| Formatter | clang-format binary at M3; libFormat in a delay-loaded `frost_format` DLL at M7. `.clang-format` is the source of truth (EditorConfig mapped onto it); all 211 options in the UI. | Users already have the files. | Our own formatter. |
| MSBuild / .vcxproj | Behind `IMsBuildEvaluator`:<br>**(a) Native.** A `.vcxproj`/`.filters` reader builds the instant tree, including NMake projects (`ConfigurationType=Makefile`: NMakeIncludeSearchPath, NMakePreprocessorDefinitions, NMakeForcedIncludes, AdditionalOptions, build command lines). ProjectModelCache stores one fingerprinted record per (project, config, platform, toolchain). Warm opens of an unchanged tree validate natively and start zero .NET processes.<br>**(b) `FrostIDE.ProjectHost`.** About 3k lines of C# over StreamJsonRpc: net472 via MSBuildLocator, net10.0 for SDK-style projects. 2–4 workers, active configuration first, a shared EvaluationContext. Runs `ComputeReferenceCLInput;GetProjectDirectories;GetClCommandLines` with the msbuild-extractor-sample global properties. Source of truth. `FrostIDE.MSBuildLogger` (C#) streams build events. Both land in M16 (ADR 0029).<br>**(c) `MSBuild.exe -getTargetResult` JSON.** Spike, consistency check, pure-C++ fallback, and the evaluator of the first usable build until ProjectHost lands (M2, with the wrapper target of ADR 0005).<br>**(d) Approximate mode.** Labelled; off Windows or without VS. | Only MSBuild evaluates property sheets and conditions exactly. The cache gives native-speed warm opens. net472 needs no extra runtime. (b) and (c) share one engine, so `cl.exe /c` compiles the extracted command lines as the independent check (§6.1). | A native evaluator as source of truth. CLI-only (D3). |
| .sln / .slnx | Native readers: a `.sln` tokenizer, `.slnx` via pugixml following Slnx.xsd, and `.slnf`. Writes and conversion go through Microsoft.VisualStudio.SolutionPersistence 1.0.52 (MIT) in ProjectHost. | Reads are on the critical path. Writes must match what VS produces. | A native writer (only if D3 is CLI-only). |
| CMake | Stateful File API query `client-frostide`: codemodel v2, cache v2, cmakeFiles v1.1, toolchains v1.1, configureLog v1. Native presets parser for v2–v13. `ctest --show-only=json-v1`. `isGenerated` sources show a "Build to generate" hint. **Language service (M16):** tree-sitter-cmake plus the File API model plus docs baked from `cmake --help-*-list`. **Debugger (M6):** `cmake --debugger --debugger-pipe` (CMake ≥3.27) through the DAP client. Requires CMake ≥3.21. | Exact targets and flags per configuration; CLion-grade CMake editing and debugging. | Parsing CMakeLists for the model. |
| Toolchain detection | VS Setup COM API, vswhere fallback. Cached vcvarsall environment diffs keyed by instance, arch, SDK and vcvars_ver. Also clang, GCC and MinGW. Tools resolve to absolute paths (§3.5). WSL, Docker and remote in 1.x (D5). | Correct environments, no per-launch cost. | vcvarsall on every build. |
| Debuggers | **Windows:** lldb-dap from U1 (M4), debugging MSVC-built programs through their PDBs. S5 found that the official LLVM 23.1.4 Windows package has no lldb or lldb-dap, so M4 builds lldb-dap from the pinned LLVM sources into E:\sdk, pinned in `sdk/sdk.lock.json` (the SDK leg, §6.3; ADR 0029, decision 12); **frost-dbgeng-dap** at M10 on `Microsoft.Debugging.Platform.DbgEng` 20260319.1511.0 (natvis, minidumps, symbol servers, hardware breakpoints), delivered by the ComponentDownloader until legal review. **Linux:** lldb-dap from U1 (M4), built from the same sources in WSL into `~/sdk`, because the official package's lldb-dap links a libpython that Ubuntu 24.04 lacks (S5); `gdb -i=dap` (GDB ≥16) out of process from M6; Ubuntu 24.04 ships GDB 15, so ≥16 is a documented prerequisite there, tested with a GDB ≥16 installed into WSL (`~/sdk`); ptrace_scope detection with a fix hint (M6, with attach). **macOS:** none in v1 (D24); the design (lldb-dap with the system debugserver) stays for later. | DbgEng is redistributable with full PDB fidelity. | vsdbg and cpptools (licence forbids it, enforced since 1.24.5; local verification scans artifacts). |
| DAP/LSP/MCP clients | Own C++23 clients. `frost-codegen` generates types from DAP 1.71, the LSP 3.18 metaModel and the MCP schema, and emits each schema `description` as a code comment. JSON via yyjson. | Typed, versioned and documented. cppdap was archived on 2026-09-28. | cppdap. |
| Process model and IPC | One UI process with Qt-free core services. Engines run in a Job Object or process group under a supervisor that restarts them with backoff and replays state. Content-Length JSON-RPC and NDJSON; tests also run every service over a serialising in-memory transport. `FrostLayers.cmake` fails configure on layering violations; only `src/ui` and `src/app` link Qt. | Crash isolation; no IPC on the keystroke path. Re-evaluated at M13. | Shared-memory IPC in v1. |
| Scheduler | `frost::sched`: N−1 Chase-Lev workers in four lanes (Interactive > UserVisible > Background > Idle) with aging; background capped at cores−4. `Task<T>` coroutines; `std::stop_token` plus generation counters (latest request wins). MainThreadDispatcher delivers one Qt event per frame. asio on IOCP, epoll, kqueue. SimExecutor for virtual-time tests. Helpers run at BELOW_NORMAL + EcoQoS or SCHED_IDLE. | Priorities alone cut Zed's first search result from 16.81 s to 32 ms. | std::execution. TBB. |
| Storage and caching | SQLite 3.54.1 (WAL) for caches, local history, session state and the hot-exit journal; settings are JSON (§3.6). **FSEG** segments: immutable, generation-named, XXH3 checksums, an atomically renamed manifest; a mapped file is never replaced; the reader is fuzzed. XXH3-128 fingerprints. Caches in `%LOCALAPPDATA%\FrostIDE\caches`, XDG or `~/Library/Caches`, with a 20 GB default budget and LRU eviction after 30 days unopened. One path normaliser for cache keys (per-volume case folding, `\\?\`, UNC, ReFS). | Mappable and crash-safe. VS's SQLite cache opened LLVM 4x faster. | LMDB. |
| File watching | ReadDirectoryChangesW on IOCP (Ex variant only on NTFS; it fails on ReFS), rescan on overflow, optional USN journal; inotify with limit detection and polling fallback; FSEvents with a persisted event ID. gitignore-aware parallel scan. | Scales to 500k files. | QFileSystemWatcher. |
| VCS | git CLI: `status --porcelain=v2 -z`, a persistent `cat-file --batch`, fsmonitor advice. Own histogram diff and 3-way merge (cores in M1). Remote operations use `GIT_TERMINAL_PROMPT=0`, timeouts and `frost-askpass` (GIT_ASKPASS/SSH_ASKPASS over an ACL'd pipe) for credentials, passphrases and host keys, honouring credential.helper/GCM. **Commit signing:** detects `commit.gpgsign`, `tag.gpgSign` and `gpg.format` (openpgp, ssh, x509) before committing. SSH signing (`ssh-keygen -Y sign`) gets its passphrase prompt through frost-askpass (`SSH_ASKPASS` with `SSH_ASKPASS_REQUIRE=force`) or from a running ssh-agent. OpenPGP signing needs a running gpg-agent with a GUI pinentry; a missing agent or a TTY-only pinentry is detected up front and shown with a fix hint. Signing runs under a 60 s timeout, and a timeout reports a clear error instead of hanging. LFS detection; hook output in a console. | The user's git behaviour, hooks included, once trusted (§3.5). | libgit2. |
| Terminal | Bundled ConPTY 1.25.260930003 (MIT) on dedicated reader/writer threads that answer the cursor request; forkpty on Unix. libvterm 0.3.3 behind `IVtEngine`, patched for DECRQM ?2026, XTVERSION, kitty keyboard, OSC 8, OSC 9;4, and win32-input-mode (DECSET ?9001) encoding for ConPTY. IME preedit in TerminalView. `TERM_PROGRAM=FrostIDE`. | The Claude Code TUI needs synchronized output and Shift+Enter; on Windows modifiers reach claude.exe only via win32-input-mode (S8 verifies). | In-box ConPTY. A separate pty-host. |
| Search | RE2 default, PCRE2 10.49 JIT on demand; prioritized streaming; SIMD fuzzy matcher; trigram index above 50k files (M13). | Linear-time default. | std::regex. |
| Extensibility | v1: declarative JSON manifests (themes, keymaps, templates, external tools); after 1.0, WASM components on Wasmtime 50.x in a plugin host. | UI process stays safe. | In-process native plugins. |
| Claude integration | C1–C3 in v1, C4 after 1.0 (§5). Reverse-engineered surfaces (lock-file fields, openDiff results, notifications) are versioned and pinned by golden transcripts per CLI minor. | The user keeps their own install. | Bundling the CLI. |
| Self-diagnostics | spdlog; Tracy in dev builds; Perfetto export; ETW provider "FrostIDE" (FirstPresent, FirstHighlight, Interactive, IndexMapped, InputToPaint, FlushEnd); a 50 ms stall watchdog feeding freeze reports. | Budgets need markers. | |
| Licences | FrostIDE: Apache-2.0 (D7, decided). SPDX allowlist for linked code, matched as exact expressions: MIT, BSD-2-Clause, BSD-3-Clause, `BSD-3-Clause WITH PCRE2-exception` (PCRE2 10.49), Apache-2.0, `Apache-2.0 WITH LLVM-exception`, BSL-1.0, Zlib, ISC, curl, blessing (SQLite), OFL-1.1 (fonts). LGPL-3.0-only only for dynamically linked Qt. **Bundled separate processes** (never linked) form their own class: `appimageupdatetool` on Linux (AppImageUpdate is reported as MIT; zsync2, which it uses, is Artistic-2.0; both verified in M0). GPL tools only as separate, user-installed processes. Proprietary redistributables (DbgEng) are a separate class: downloaded on first use, never bundled before legal review. `frost-licensecheck` produces the SBOM. When it is built in M0, every vcpkg port's declared licence expression is checked against the list (utf8proc's Unicode data tables, for example); any expression not on the list needs an ADR before the dependency lands. | Patent grant; compatible with every dependency. | MIT. GPLv3. |

### 2.1 Allowlist, compat layer and startup hygiene
**C++26 features allowed in our code.** Pack indexing; `_` placeholders; `= delete("reason")`; user `static_assert` messages; attributes on structured bindings; structured-binding packs once the allowlist probe passes on every supported compiler.

**Banned in v1.** Reflection; contracts syntax; expansion statements; `#embed`; std::execution; direct C++26 library headers; named modules. `toolchain_probe` must compile the allowlist and reject a banned construct.

**`frost/compat` polyfills**, keyed on `__cpp_lib_*`, each with its removal condition: flat_map/flat_set (STL 14.44, libstdc++ 14); mdspan via Kokkos (libstdc++ <16.1); the libc++/Apple gaps (zip, generator, move_only_function, chunk/slide, stacktrace via cpptrace, inplace_vector via beman, enumerate) matter only if macOS is added, though each polyfill still switches on `__cpp_lib_*` so any library lacking a feature gets it. Also `function_ref`, saturation arithmetic, `FROST_ASSUME`, and `FROST_PRE`/`FROST_POST`/`FROST_ASSERT` with ignore, observe, enforce and quick_enforce semantics.

**Dependencies.** From vcpkg: gtest 1.18, benchmark 1.10, yyjson, xxhash 0.8.4, sqlite3 3.54.1, re2, pcre2, asio, llhttp, pugixml, spdlog, cpptrace, mimalloc, utf8proc, crashpad, libsodium, libcurl (Linux), tracy (version chosen in M0: 0.14.1 from the vcpkg baseline; use under the dev-builds-only listener exception of ADR 0009). Vendored: tree-sitter, libvterm.

**Startup hygiene.**
- `-Wglobal-constructors` and `-Wexit-time-destructors` are errors on Clang legs.
- `frost.exe` manifest: longPathAware, activeCodePage UTF-8, PerMonitorV2. frost-lint bans narrow (`…A`) Win32 path APIs.
- A DLL-import allowlist on `frost.exe`; frost_format, DbgHelp and WinHTTP are delay-loaded.
- Icon atlases and the theme blob are baked at build time; tool windows are created lazily.
- No network I/O before L1 (tested by local verification); `claude` is never spawned on the cold-start path (the first-run wizard runs it only after L1, §3.4). Fast exit; PGO later.

### 2.2 Product infrastructure
| Item | Design | Milestone |
|---|---|---|
| Settings | Typed registry, scopes, `.frost/`, migrations, dialog, importers (§3.6). | M1 (registry, scopes, JSON scope files); dialog M15; importers M11 |
| Data safety | `SaveManager`: atomic writes (temp file, then ReplaceFileW or rename) preserving BOM, EOL, encoding, permissions, ACLs, hardlinks and symlink targets; retry and UI for locked or read-only files; disk-full handling. Configurable autosave on frame deactivation and idle. A SQLite dirty-buffer journal for hot exit and crash recovery. External changes: silent reload when clean, a 3-way merge prompt when dirty. | M1 |
| Crash and freeze reporting | Crashpad (Apache-2.0) out-of-process handler in frost and every engine, writing to `<cache>/crashes`. The stall watchdog writes freeze reports (UI-thread stack samples); engine crashes record the TU and command line. Help > Collect Logs and Diagnostics zips logs, dumps and freeze reports with a redaction preview. `tools/package` archives symbols per tagged build (symstore on Windows, debuglink on Linux) under E:\FrostBuilds. Upload is opt-in (D26). Local retention follows the Privacy row. | Handler M1; Collect Logs, symbols M5 |
| HTTPS | `IHttpClient`: WinHTTP (auto-proxy/PAC), libcurl with OpenSSL 3 on Linux (an NSURLSession shim only if macOS is added); a fake double; fuzzed parser. | M5 Windows; M6 Linux |
| Packaging | Portable zip or tarball (Qt deployment, app-local VC runtime matching the pinned STL, M4's lldb-dap build, ProjectHost per D27 from M16, archived split symbols), smoke-tested with `--selftest` from a fresh extraction under a clean profile on REF-W1 and in WSL (and in the floor-build VM from M6). Unsigned per-user WiX MSI (no UAC) and AppImage (static type-2 runtime) in M6; signed, every helper included, in M14 (D25). Packages stay local or in the private repository for personal use (D8, D22). Uninstall removes lock files and associations (caches on request); upgrade-in-place tested. Size: B27. | M5, M6, M14 |
| Updates | Ed25519-signed manifest (libsodium) with SHA-256 payload hashes; nightly, EAP and stable channels; SemVer plus build number; generated changelog. Own updater applying the per-user MSI after exit (Windows), `appimageupdatetool` (AppImageUpdate/zsync2) run as a separate, unlinked process (Linux). Checks only after L1 and with consent; rollback; downgrade protection. One `ComponentDownloader` (signature check, resume, proxy, offline import) for DbgEng and other optional components. | M6 |
| Privacy and network | No telemetry in v1. No network I/O before L1. Every outbound endpoint (updates, symbol servers, downloads, crash upload) is recorded in an ADR and listed in Settings with first-use consent; symbol servers stay off until enabled per project; an offline switch. Verification test: a scripted session records connections (ETW Kernel-Network, `strace -f -e trace=connect`) and asserts none without consent. **Retention:** crash dumps, freeze reports, ETW/Perfetto/Tracy traces and logs may contain paths and code. They stay local in `<cache>/crashes`, `<cache>/traces` and `<cache>/logs`, kept ≤30 days and ≤1 GB in total (oldest deleted first) and counted in the cache budget. Invalidate Caches / Repair IDE removes them, and uninstall always removes them, even when caches are kept. They leave the machine only through Collect Logs with its redaction preview, or the opt-in upload (D26). A test ages and oversizes fixtures and asserts the pruning. Privacy statement in M14. | Policy, retention and tests M1 |
| Trust | Restricted mode, absolute-path tool resolution (§3.5). | M2 |
| Local listeners | Every listener hardened (§5.2). A verification test enumerates sockets and pipes after startup and asserts only the expected ones exist, none before L0. | M2, then per new listener |
| Windows paths and encodings | Manifest and narrow-API lint (§2.1); fixtures with a non-ASCII profile path with spaces and paths over 260 characters (M0). Build drivers decode child output by its code page (UTF-8 where possible), parse with locale-independent anchors (C\d{4}, MSB\d{4}, LNK\d{4}), have goldens for en, de and ja VS language packs, and offer an opt-in `VSLANG=1033` (M4). | M0, M4 |
| Shell integration | Single instance over an ACL'd pipe with argument forwarding. A `frost` CLI shim: `frost <path>`, `frost --wait` for GIT_EDITOR/EDITOR, `frost diff` and `frost merge` with git-compatible exit codes. Per-user file associations (HKCU ProgIDs, Linux .desktop/MIME). Jump List recents. | M2 (single instance, `frost <path>`); M16 (`--wait`, `diff`, `merge`, associations, Jump List) |
| Resource governance | A `ResourceGovernor` in the supervisor scales clangd background threads, the analyzer pool and ProjectHost workers from free RAM, commit charge and battery. Process-tree budget B26; the cache disk budget (a registry key edited in the scope files until the Settings dialog gives it a page in M15); Invalidate Caches / Repair IDE. Power Save Mode pauses T2/T3 and background indexing. | M3; Power Save M7 |
| i18n readiness | Every user-visible string goes through `frost::i18n::tr(context, text)`; core errors carry message IDs; no sentence concatenation; plurals via `%n`. frost-lint rejects literals passed to UI text setters. A pseudo-localization screenshot test (+40% length, accents). Translations stay a non-goal. | M1 |
| Keyboard and IME | Shortcut matcher with AltGr disambiguation and layout-aware mapping (tests simulate de-DE, pl-PL, fr-FR), dead keys, double-Shift suppressed during IME composition. Grapheme-cluster caret movement and deletion. Terminal IME. | M1; terminal M4 |
| Accessibility | Keyboard navigation of stripes, toolbar (Alt+PageUp), status bar (Alt+PageDown) and gutter, with focus-order tests, and the UIA TextPattern smoke (M15). Colour-deficiency palette, contrast scrollbars, reduced motion, OS high contrast / forced colours / Increase Contrast, and a terminal text provider for UIA and AT-SPI (M11). | M15, M11 |

### 2.3 Dependency upgrade policy
| Dependency | Policy |
|---|---|
| LLVM | Each major within ~6 weeks of its .1 release: rebuild, corpus differential (diagnostics, flags, completion and refactoring goldens), regenerated inspection counts, ADR. Point releases as they appear. A local `llvm-next` leg (`tools/verify.sh -LlvmNext`, run on demand, no CI) builds the engines against the newest release branch. LLVM 24 is expected around March 2027 (inferred from the ~6-month cadence), so the 35–43-month roadmap spans 5–7 majors (24 to 28, 29 or 30). ~1 unit per major. |
| Qt | Each minor within ~6 weeks of its first patch release, behind the screenshot, IME and accessibility suites; patch releases as they appear; no open Qt advisory may affect a shipped module at release. Qt ships about two minors a year, so the roadmap spans 5–7 minors (6.13 onwards). ~0.5 unit per minor. |
| Others | MSVC STL moves as a pair with LLVM (14.44 → 14.52). vcpkg baseline quarterly (about 12–14 bumps, ~1.5–2 units in total). The whole track is ~9–12 units (D20). OSV-Scanner over the SBOM in `verify -Full`. Weekly golden transcripts (run locally) for the newest Claude CLI minor; failures block the next release, not development. |

---

## 3. Architecture

### 3.1 Process topology
```
 +-----------------------------------------------------------------+
 | frost (UI process)                                              |
 |  Qt UI thread: EditorView, FrostStyle, ToolWindowManager         |
 |  Qt-free core: text, syntax(T0), project, diagnostics, search,   |
 |  vcs, term (PTY threads), agent, settings, save, sched, reactor  |
 +--+----------+----------+----------+-----------+---------+-------+
    | JSON-RPC | JSON-RPC | JSON-RPC | DAP       | stdio   | WebSocket 127.0.0.1
    v          v          v          v           v         v
 frost-clangd frost-     ProjectHost lldb-dap /  git,      claude CLI --stdio--> frostide-mcp
 (T1, index)  analyzer   (C#)        gdb /       msbuild,                        (pipe+token -> frost)
              (T2/T3)                frost-dbgeng cmake    frost-askpass (pipe+token -> frost)
 Supervisor + ResourceGovernor: Job Object / process group, restart with backoff, document replay
 crashpad_handler (out of process) for frost and every engine; `frost` CLI shim -> single-instance pipe
```

### 3.2 Repository layout
```
FrostIDE/
  CMakeLists.txt, CMakePresets.json, vcpkg.json   build entry points
  CLAUDE.md  PLAN.md           agent manual (definition of done, working agreement, clean-room rule); this plan
  README.md LICENSE NOTICE SECURITY.md CONTRIBUTING.md   repository-meta files (§6.6)
  REUSE.toml                   licence annotations for JSON, fixtures, goldens, corpora and assets (§6.6)
  cmake/  triplets/            FrostLayers.cmake, per-compiler warnings, sanitizers; vcpkg triplets
  sdk/sdk.lock.json            pinned LLVM/Qt/SDK/container artifacts with SHA-256
  docs/adr/  docs/protocols/   decision records; reverse-engineered protocol notes
  docs/progress.md             session hand-off log
  src/compat/  src/core/       polyfills; errors, strong types, logging, tracing, i18n
  src/settings/                registry, scopes, migrations, importers
  src/sched/  src/io/          scheduler, SimExecutor; reactor, processes, file system, watchers, http
  src/text/  src/syntax/       piece tree, display map, undo, anchors, SaveManager; tree-sitter host
  src/diff/                    histogram line diff, refinement, three-way merge cores (ADR 0019)
  src/input/                   shortcut matcher, keyboard layouts, keymaps, action registry, Find Action (Qt-free)
  src/storage/  src/rpc/       SQLite, FSEG, fingerprints, journal; transports and framing
  src/protocols/               generated LSP/DAP/MCP types
  src/project/                 solution/vcxproj readers, cache, CMake, presets, toolchains, trust
  src/lang/                    LSP client, diagnostics store, inspection registry, SARIF, CMake language
  src/build/  src/debug/       build drivers, run configs; DAP client, breakpoints
  src/test_runner/  src/vcs/   test discovery; git (on src/diff's diff and merge), askpass broker
  src/term/  src/search/       PTY and VT engine; search, fuzzy matcher, trigram index
  src/agent/  src/diag/        Claude bridge, stream-json client, MCP core; crash, freeze, Collect Logs
  src/update/                  updater, ComponentDownloader
  src/ui/  src/app/            Qt widgets and renderers (src/ui/input: the QKeyEvent adapter); entry point, shell
                               integration, automation (only Qt users)
  engines/                     frost-clangd, frost-analyzer, frost-format, frost-dbgeng-dap, frostide-mcp, frost-askpass
  dotnet/                      FrostIDE.ProjectHost, FrostIDE.MSBuildLogger
  tools/                       bootstrap, verify, perf, package, corpus-fetch, frost-lint, licensecheck, codegen, featurecheck, bench, synth-xl
  third_party/  resources/     tree-sitter, grammars, libvterm + patches; themes, icons, fonts
  tests/                       unit, integration, golden, fuzz, ui, perf/budgets.json, features/matrix.json, corpus/manifest.json
```

### 3.3 Interfaces and threading
- Each seam in §6.2 is an abstract interface with a deterministic double. Fallible calls return `std::expected<T, frost::Error>`; async calls return `Task<T>` and take a `std::stop_token`.
- Availability flags L0–L4 gate actions (like IntelliJ's DumbAware); the status bar shows the level. Trust state (§3.5) gates every action that can execute project-supplied code.
- **Threads:** the UI thread (input, layout, paint, T0; never blocks on I/O); N−1 scheduler workers; 1–2 reactor threads for all pipes and sockets; dedicated PTY, watcher and watchdog threads.
- Documents and models are immutable snapshots, so readers take no locks. Each service has one owning actor; remaining locks carry `FROST_GUARDED_BY`, checked by `-Wthread-safety`.

### 3.4 Data flows
**First run.** The Welcome window appears at L0 (recent solutions and Open from M2, Get from VCS from M5, New Project/Solution from M16). The first-run wizard (M16) covers theme, keymap, settings import (from M11), a toolchain report with deep links for anything missing, Claude Code detection with the official installer link (the wizard spawns `claude --version` and `claude auth status --json` through the absolute-path launcher only when its Claude page is shown, after L1, never on the cold-start path), a Defender/Dev Drive check, and consent for crash reports and network use. Nothing touches the network.

**Cold start**
1. Minimal static init, then Qt init. Theme blob and icons are mapped, and the first present happens (L0).
2. At idle, a parallel scan and native solution parsing fill the trees (B23).
3. In a trusted project, ProjectHost streams evaluations, active configuration first (B23); until M16, path (c) evaluates the projects after L1 (§7 M2). clangd starts indexing.
4. The bridge listens. The `claude` CLI is never spawned.

**Warm open**
1. Read the SQLite session and the FSEG manifest, then map segments. Editors restore from cached text and tree-sitter (B2).
2. Fingerprints are validated in parallel: import closures, absent-file probes, glob listings, captured environment, global properties, VS and SDK versions.
3. With every fingerprint valid, L1 is reached with zero .NET processes; otherwise only invalid records are re-evaluated.
4. clangd loads its persisted index, reaching L2 and then L3.

**Keystroke → paint**
1. The key event becomes a command that edits the piece tree and creates a new snapshot.
2. tree-sitter parses incrementally within 2 ms; on overrun the parse moves to a worker and the old highlights stay.
3. The display map updates dirty rows, then display list, paint and flush. `didChange` is posted asynchronously; nothing on this path waits on IPC.

**Save and external change.** SaveManager writes a temp file beside the target, flushes it and swaps it in with ReplaceFileW or rename, preserving attributes, ACLs and links; the journal entry clears. On an external change, a clean buffer reloads as an undoable edit and a dirty buffer opens a 3-way merge prompt.

**Diagnostics.** clangd debounces for 50–500 ms and publishes version-tagged results. DiagnosticsStore merges them by file, version and tier, re-anchoring or dropping stale ones. T2 results arrive after idle or save, T3 as SARIF. The error stripe and Problems window update.

**Completion.** Each request carries a generation counter; a newer keystroke cancels it via `$/cancelRequest`. The fuzzy matcher re-ranks results and the popup opens.

**Build, run, debug.** Modified documents are saved, then before-launch tasks run (from M4: Save All and the build before Run; the general task list from M16). MSBuild (with FrostIDE.MSBuildLogger from M16; parsed console output before) or `cmake --build` runs inside a Job Object. Output is decoded by code page and parsed into the Build and Problems windows. The DAP adapter launches and binds breakpoints; stacks load lazily and variables on expand.

**Claude edit → diff review**
1. The CLI calls `openDiff`; FrostIDE opens a non-blocking diff tab with an editable right side. The first answer (IDE or terminal) wins.
2. The CLI writes the file; the watcher reloads it as an undoable external edit, labelled in local history.
3. T1 runs; the post-edit `getDiagnostics` waits up to 1.5 s for results at that version. In C2, the PostToolUse hook returns new warnings.

### 3.5 Trust model
Opening a project can execute its code: MSBuild design-time builds run targets and inline tasks from Directory.Build.props/.targets; CMake configure runs scripts; test discovery runs binaries; clangd's `--query-driver` and `.clangd` CompileFlags.Compiler run compilers; compile commands and `.clang-tidy` `ExtraArgs` can load compiler plugins (`-fplugin=`, `-fpass-plugin=`, `-Xclang -load`); project settings can name clang-tidy `--load` modules and custom clang-tidy, clang-format or compiler binaries (row 104); committed `.frost/` settings and run configurations can name executables and commands (§3.6); git honours repository config (fsmonitor, hooks); `.lldbinit`, `.gdbinit`, natvis and imported run configurations carry commands.

| Mode | Allowed | Blocked |
|---|---|---|
| Restricted (default for a new root) | Native .sln/.slnx/.vcxproj/presets readers; reading existing File API replies (their targets are shown, never built or run); tree-sitter; File System and Solution views; text search; clangd with native-reader flags, `--enable-config=false` and no `--query-driver`; T1 only with FrostIDE-bundled tools at absolute paths and no plugin loading (plugin-loading flags are stripped from every command line handed to clangd or tidy, and `ExtraArgs` is ignored); read-only git with `-c core.fsmonitor=false -c core.hooksPath=<empty dir>`; the terminal, running the application-scope shell at its absolute path with the root as working directory and NoDefaultCurrentDirectoryInExePath kept (ADR 0011, E2), so a command typed there never resolves to a binary in the root | MSBuild design-time builds (ProjectHost or path (c)), CMake configure, builds (Build, Rebuild, Clean and the build before Run or Debug), running or debugging any run configuration (derived from File API replies or the solution, or project-shared), test discovery, debugger init files and project natvis, run-configuration auto-import, tasks and external tools, Claude hooks for that root; project-specified tool binaries (custom clang-tidy, clang-format, compiler, debugger or CMake paths); clang-tidy `--load` modules and compiler plugins; T2/T3 runs (frost-analyzer, CodeChecker, IWYU, cppcheck, clazy) driven by project-supplied configuration; every `executes` key in project-shared settings (§3.6) |
| Trusted | Everything | — |

- Trust is granted per root from a banner and stored in application settings; trusting a parent covers its children.
- A CMake root is the opened source directory. A solution's root is the git work tree that contains it, or the solution's directory outside a work tree, because projects often sit beside the solution's directory (R2a's Scintilla and Lexilla); projects that the solution lists outside the root are named on the banner and trusted with it.
- A CMake profile's build directory may lie outside the root (R3's `E:\corpus\R3-build` beside `R3\llvm`). Trusting the root covers configure and builds into its profiles' build directories. A build directory outside the root is used only when the user chose it, in an application-scope profile for that root, never when a file in the repository names it, so an untrusted repository cannot aim configure or build output at an arbitrary directory.
- `IProcessLauncher` resolves every tool (git, cmake, msbuild, claude, compilers) to an absolute path from detection results, never searches the current or project directory (`NoDefaultCurrentDirectoryInExePath`, `SetDefaultDllDirectories`), and logs the path.
- A malicious fixture repository proves nothing runs before trust and the planted binaries never run. It contains planted git.exe and cmake.exe, an Exec task in Directory.Build.props, an fsmonitor hook, an `.lldbinit`, a `.clang-tidy` whose `ExtraArgs` and a `compile_commands.json` whose `-fplugin=` load a planted plugin, a `.frost/settings.json` naming a planted clang-tidy and compiler path, a `.frost/runConfigurations` entry with a before-launch command, a CMake build directory inside the repository whose File API reply points an executable target at a planted binary, and planted `cmd.exe`, `pwsh.exe` and `bash` in the root. The M2 test covers project opening; M4 extends it to Build, Run, Debug and the terminal; M16 to the run-configuration importers.

### 3.6 Settings model
| Scope (low → high) | Location | Shared? |
|---|---|---|
| Defaults | the compiled registry | — |
| Application | `%APPDATA%\FrostIDE`, `$XDG_CONFIG_HOME/frostide` | per user |
| Project-shared | `.frost/settings.json`, `.frost/runConfigurations/*.json`, `.frost/inspectionProfiles/*.json` | committed |
| Project-personal | `.frost/workspace.json`: window layout, selected configuration, breakpoints | gitignored (FrostIDE writes `.frost/.gitignore`) |
| File-level | `.editorconfig`, `.clang-format`, `.clang-tidy` | committed |

- Typed registry entries: key, schema, default, allowed scopes, and a description shown in the dialog and emitted as a comment by frost-codegen.
- **Executable-bearing keys.** Keys that name an executable, command, plugin or script are marked `executes` in the registry. Examples: clang-tidy, clang-format, compiler, debugger and CMake paths; tidy plugin lists; external tools; before-launch tasks; run-configuration commands. Tool paths and plugin lists are application-scope only. Run configurations and tasks may live in project-shared files, but their `executes` keys are ignored until the root is trusted, and the trust banner lists them (§3.5). A registry test fails if an `executes` key allows project scope without the trust check.
- Diffable JSON with a `schemaVersion`; migrations are pure functions with golden tests; unknown keys survive round trips; files reload live. SQLite never holds settings.
- **Dialog (M15):** searchable tree, scope switcher, modified markers, reset, Advanced Settings; pages for every key that M1–M5 added (editor, toolchains, CMake profiles, clangd and the cache budget, build and run, debugger, terminal, Git, Claude), generated from the registry's schemas and descriptions, with hand-built pages for the Keymap (ADR 0016), CMake profiles and toolchains. Until then (M1) Find Action opens each scope file in the editor, saves are validated against the registry, and errors appear as notifications (ADR 0029).
- **Importers (M11):** Rider/CLion `.DotSettings` and `.idea` (keymaps, code style to `.clang-format`, inspection severities, live templates); VS `.vssettings`/`.vstheme`; VS Code `settings.json`/`keybindings.json`; zip export and import.

---

## 4. Performance budgets

**Reference rigs.**
- **REF-W1**, the dev machine: 24 threads, 32 GB, RTX 4070 SUPER, 5120x1440 @ 144 Hz, sources on E:.
- **REF-L1**: the B-suite on REF-W1 inside a Job Object (4 cores, 8 GB cap), with a 2560x1440 window on the main monitor switched to a 60 Hz display mode for the run (`ChangeDisplaySettingsEx`, restored afterwards) and WARP for any GPU path; budgets ×1.5. It simulates fewer cores, less memory, a smaller window and a lower refresh rate. It does not simulate an integrated GPU, thermal throttling or a battery. Battery and Power Save paths run as a scenario in which the `IPowerSource` double reports battery, low battery and OS battery saver, and the test asserts that the ResourceGovernor scales down and Power Save Mode pauses T2/T3 and background indexing. Behaviour on real iGPU and battery hardware is labelled untested unless the user adds a laptop.
- **WSL leg**: Linux budgets are measured in WSL Ubuntu 24.04 on REF-W1 (sources on ext4, not /mnt). WSL shares the host's cores and memory, so it gates instruction counts and a wall-clock smoke at 2x budget; absolute Linux numbers on bare metal are untested. There are no hosted baselines (D24).

**Corpora.** `tests/corpus/manifest.json` records URL, pinned SHA, licence, size and prerequisites; `tools/corpus-fetch` fetches into E:\corpus; third-party corpora are never committed.
- **R1** FrostIDE. **R2a** Notepad++ (Desktop C++ v143). **R2b** microsoft/terminal OpenConsole host projects, pinned (v143, Windows SDK 10.0.26100.0; the full solution needs the UWP workload and is opt-in).
- **R3** llvm-project 23.1.4 with Ninja; **R3s** the same via the "Visual Studio 18 2026" generator (v145).
- **R4** synth-xl: deterministic .sln/.slnx, 8-deep property sheets, Directory.Build.props, vcpkg imports, NMake projects; tiers S/M/L/XL (XL ≥500k files, ≥50k TUs).
- **R5** a mixed C#/C++/C++CLI .slnx. **S50** 50 named solutions for the SolutionPersistence oracle. **F1** a 2 GB file; **F2** a 50 MB single line.
- **Fixtures:** a synthetic UE-style NMake .sln (the UE EULA forbids engine files), Qt AUTOMOC and protobuf generated sources, C++20 modules, the malicious repository, non-ASCII and long paths.

**Availability levels.**

| Level | Meaning | Warm reopen | First-ever open |
|---|---|---|---|
| L0 | First frame | B1 | B1 |
| L1 | Interactive: trees, editors, Go to File, T0 | B3 | B23 |
| L2 | Navigation from the persisted index | B24 | Not promised |
| L3 | Active-TU semantics | B25 | Heavy cold preambles (2–20 s) tracked, not gated |
| L4 | Full index | Tracked, not gated | Tracked, not gated |

**Budgets.** `tests/perf/budgets.json` is the source of truth: metric, percentile, corpus, preconditions, method and gating milestone. Memory is private bytes (working set recorded alongside). "Cold" means after a reboot or standby-list purge (RAMMap -Et), with Defender and Dev Drive state recorded per sample.

| ID | Metric (preconditions) | Target | Corpus | Gate from |
|---|---|---|---|---|
| B1 | First frame, warm / cold | p50 150 / 400 ms; p99 250 / 600 ms | empty, R3 | M1 |
| B2 | Highlighted active tab, warm | p99 300 ms | R3 | M2 |
| B3 | Warm reopen to L1 (unchanged tree, all fingerprints valid, zero .NET processes) | p50 700 ms; p99 1 s | R2b, R3, R4-L | M2 |
| B4 | Fingerprint validation | p50 150 / 300 ms; p99 300 / 600 ms | R2b / R4-L | M2 |
| B5 | Keystroke → paint end (ETW), soft wrap off and on | p50 3 ms (target 2); p99 8 ms | R3 | M1 |
| B6 | 10 s fling: UI CPU per frame ≤5 ms p99; missed compositions ≤1%; no content interval >16.7 ms | as stated | 5120x1440 @ 144 Hz | M1 |
| B7 | Go to File (≤16 ms p50 at 500k files) | p50 15 ms; p99 50 ms | R3, R4-XL | M2 |
| B8 | Completion | p50 60 ms; p95 150 ms | R3 | M3 |
| B9 | Go to definition | p50 40 ms; p95 100 ms | R3 | M3 |
| B10 | Find Usages, first result | p50 150 ms; p99 300 ms | R3 | M3 |
| B11 | T1 diagnostics after an edit | p50 500 ms; p95 1.5 s | R3 | M3 |
| B12 | T2 per file, warm / cancel | p50 10 s / p99 200 ms | R3 | M7 |
| B13 | Find in Files first hit, unindexed / indexed | p50 300 / 100 ms; p99 600 / 200 ms | R3 | M2 / M13 |
| B14 | git status with fsmonitor | p50 150 ms; p99 300 ms | R3 | M5 |
| B15 | F1 first screen (private bytes +≤64 MB; line index ≤2 s) | p50 150 ms; p99 300 ms | F1 | M15 |
| B16 | F5 → breakpoint (up-to-date check included, compilation excluded) / step | p50 1.0 s / 60 ms; p99 1.5 s / 100 ms | R1 | M4 |
| B17 | Terminal echo | p99 10 ms | — | M4 |
| B18 | getDiagnostics baseline / openDiff visible | p99 50 / 100 ms | R1 | M5 |
| B19 | UI process, no project / R3 (children excluded) | ≤80 / ≤250 MB | — | M1 / M3 |
| B20 | Exit to process gone | p50 150 ms; p99 300 ms | R3 | M1 |
| B21 | Stalls >50 ms in a 10-minute script (the script grows each milestone) | 0 | R3 | M1 |
| B22 | Engine crash → recovered | p99 2 s, no UI stall | R3 | M3 |
| B23 | First-ever open of a new root. **Trees** are measured in restricted mode: the File System view plus the native-reader Solution or CMake target tree. For R3, a build directory already configured with the `client-frostide` query is required; without one, only the File System view is promised. **First project streamed** is measured after trust: the trust click is excluded from the timing, and the evaluator's cold start is included (path (c)'s `MSBuild.exe` at M2, ProjectHost's at M16). budgets.json records these preconditions. | p50 1.5 s / 3 s (trees: provisional until M2's first trees measurement; first project: final value set by S6, which also binds the M2 gate once set) | R3 / R2b | M2 / M2 (path (c)) and M16 (ProjectHost) |
| B24 | L2 warm: first workspace/symbol and cross-file definition from the persisted index | M3: S7 baseline ×1.2; M13: p50 2 s, p99 3 s | R3 | M3, M13 |
| B25 | L3: typical active TU to clangd.fileStatus idle with semantic tokens | p50 3 s | R3 TU set | M3 |
| B26 | Process-tree memory, peak / idle | ≤8 / ≤4 GB; REF-L1 ≤5 GB peak | R3 | M3; REF-L1 at M13 |
| B27 | Installed size, including LLVM components | ≤400 MB | packages | M5 |
| B28 | Dev loop: one-.cpp change to rebuilt and linked / `verify -Quick` | p50 20 s / p50 5 min | R1 | M0 |

**Measurement.** ETW markers (input, paint end, flush end, availability levels); DWM composition timing and a Desktop Duplication probe for on-screen timing (PresentMon only for QRhi); an optional SendInput + photon probe; clangd.fileStatus and LSP markers for L2/L3; `frost-bench` scripts through the automation endpoint.

**Enforcement.**
- The WSL leg (`tools/verify.sh -Perf`) gates on instruction counts (fail above +3%) and on wall-clock smoke (fail at 2x budget).
- REF-W1 full perf run (`tools/perf.ps1 -Full`, started by the user or by Claude in a session, never by a CI service, §6.3): 20 samples, Mann-Whitney U at p<0.01, a p50 regression above 5% fails. It runs only when the session-state precheck passes (§6.3) and aborts on input. A run that fails the precheck or is aborted is marked invalid, not red, and never enters the history.
- Results go to a `perf-history` SQLite file outside the repository (E:\FrostBuilds\perf); demo packs plot trends. `perf.ps1 -Smoke` before committing text, render, sched, storage or startup changes.
- Changing a budget needs an ADR and user approval. Budgeted functions cite their ID; frost-lint checks it exists. ADR 0029 (the thin path, D29) changed no target. B15's gate moved from M1 to M15 with large-file mode, and B16's from M6 to M4 with U1's minimal debugger. B23's first-project-streamed variant is gated twice: at M2 on path (c), U1's evaluator, with `MSBuild.exe`'s cold start in place of ProjectHost's, and at M16 on ProjectHost; its trees variant stays gated at M2. Release needs 14 consecutive green local full-verification runs (`verify.ps1 -Full` plus `verify.sh -Full` plus the full perf run, on separate days; invalid perf runs neither extend nor break the streak).

---

## 5. Claude Code integration

### 5.1 Phases
**C1: IDE bridge (M5).**

- **Transport.** WebSocket on 127.0.0.1, OS-assigned port, subprotocol `mcp`. Lock file `<CLAUDE_CONFIG_DIR or ~/.claude>/ide/<port>.lock` with `{pid, workspaceFolders, ideName:"FrostIDE", transport:"ws", runningInWindows, authToken}`, deleted on exit, stale locks cleaned at startup. `serverInfo.name` "FrostIDE"; protocol versions 2025-03-26 through 2026-07-28.
- **Multiple windows.** One bridge and lock file per project window (`workspaceFolders` = that window's roots); several CLIs may connect to one bridge; selection and at-mention notifications go only to that window's clients.
- **Tools.** `openDiff {old_file_path, new_file_path, new_file_contents, tab_name}`: non-blocking and editable, returning FILE_SAVED (with final text), DIFF_REJECTED or TAB_CLOSED; first answer (IDE or terminal) wins. `close_tab`, `closeAllDiffTabs`. `getDiagnostics`: the `{uri}` baseline from cache in ≤50 ms (the CLI allows 500 ms and disables it after 3 timeouts); the post-edit `{}` call waits up to 1.5 s for T1 at the edited version (CLI limit 2 s); 0-based ranges. For compatibility: `openFile`, `getOpenEditors`, `getWorkspaceFolders`, `checkDocumentDirty`, `saveDocument`.
- **Notifications.** Sends `selection_changed` (75 ms debounce) and `at_mentioned` (Ctrl+Alt+K); consumes `ide_connected` and `log_event`.
- **Agent Terminal.** A shortcut runs `claude.exe` directly (never `.cmd` shims) with `CLAUDE_CODE_SSE_PORT`. Ctrl+Esc is the JetBrains default but Windows may reserve it for Start; an M5 test decides whether Windows ships another default. Detection via `claude --version` and `claude auth status --json`; minimum CLI 2.1.316, older gets terminal-only mode. `/ide` works from external terminals; a status-bar widget shows connected CLIs.
- **WSL.** `runningInWindows` plus path mapping; mirrored networking (`.wslconfig`) preferred, all-interface binding opt-in with a warning; `bootstrap-wsl.sh` installs claude inside WSL.

**C2: tools and hooks (M8).**
- **Separate `frostide` server**, because the CLI passes only `getDiagnostics` and `executeCode` from the IDE server to the model. `frostide-mcp` is a stdio proxy reaching the IDE over an ACL'd pipe or AF_UNIX socket with a token. FrostIDE-launched sessions get it via `--mcp-config` (a 0600 temp file); external sessions use `claude mcp add frostide -- frostide-mcp`, and the proxy finds the pipe and per-window token through a per-user discovery file with the lock file's ACL, matched by cwd.
- **Read-only tools (`readOnlyHint`):** find_usages, go_to_definition, symbol_search, type_hierarchy, call_hierarchy, file_problems, workspace_diagnostics, inspection_info, project_model, build_status, test_results; debugger_state via list_changed during debugging.
- **Mutating tools (`anthropic/requiresUserInteraction`):** rename_symbol, apply_quick_fix, build, run_tests, debug_*; long-running ones are MCP tasks (protocol 2026-07-28, CLI v2.1.304+).
- **Limits.** `anthropic/alwaysLoad` on 4 tools; instructions ≤4,096 characters; results ≤50,000 characters and ≤25,000 tokens, paginated beyond (the CLI saves larger text to a file); workspace_diagnostics and project_model set `anthropic/maxResultSizeChars` (≤500,000). Conformance tests at each threshold.
- **Hooks** via `--settings` with per-session tokens. PostToolUse on `Edit|Write` returns new warnings through an `mcp_tool` handler (no extra listener); fallback is a loopback HTTP endpoint hardened like the bridge. A Stop hook blocks completion while edited files have new errors; it honours `stop_hook_active`, blocks at most 3 times per error set, then downgrades to a warning. Merging with user and project hooks is tested against the real CLI.
- The supervisor sweeps stale temp config files at startup. Optional `frostide-tools` plugin ships an `.lsp.json` bridge so Claude's LSP tool reuses FrostIDE's engine.

**C3: native chat (M12).**
- **Auth gate, enforced in code.** Before every session FrostIDE runs `claude auth status --json`. If `authMethod` is a claude.ai subscription and no approval is recorded, it refuses with an explanation. "Recorded" means a compiled constant that changes only through an ADR citing Anthropic's written approval (D10). The refusal offers three paths: an API key or `apiKeyHelper` through `--bare`, a third-party provider, or the Agent Terminal. The ADR documents what `--bare` changes (it never reads OAuth credentials and connects to the IDE only with `--ide`), verified against the CLI at M12.
- **Scope of the policy.** The gate is on in every build. D22 is decided as personal use first, possibly public and free later; Anthropic's approval for subscription auth (D10) is therefore only worth seeking if FrostIDE goes public. Until then C3 runs on API-key or third-party-provider auth. Terminal mode, where the user runs their own unmodified CLI in the Agent Terminal, is unaffected: Anthropic's terms allow an end user to sign in to the unmodified binary with their own subscription.
- **Launch.** Common arguments: `claude -p --input-format stream-json --output-format stream-json --verbose --include-partial-messages --replay-user-messages --permission-prompt-tool stdio [--resume=<id> | --fork-session | --resume-session-at=<uuid>] [--model] [--effort] --ide --settings <per-session hooks file> --mcp-config <frostide>`, with `CLAUDE_CODE_ENABLE_SDK_FILE_CHECKPOINTING=true`. Hooks, the IDE connection and the frostide server are always passed explicitly, because -p mode does not rely on IDE auto-connect. There are two variants, chosen by the gate:
  - **(a) Subscription auth,** only once an approval is recorded (D10): the common arguments without `--bare`.
  - **(b) API key or `apiKeyHelper`:** `--bare` plus the common arguments. Under `--bare` the CLI never reads OAuth credentials and connects to the IDE only with `--ide`, so dropping `--ide`, `--settings` or `--mcp-config` would silently lose the IDE connection, hooks or tools. Third-party providers (Bedrock, Vertex) use whichever variant the M12 ADR verifies for them.
- **Protocol.** NDJSON client with a capped line buffer. `control_request`/`control_response`/`control_cancel_request` with 60 s timeouts. Host → CLI: initialize, interrupt, set_permission_mode, set_model, rewind_files, mcp_*, get_context_usage. CLI → host: can_use_tool, hook_callback, mcp_message. Features gated on `system/init` capabilities and `claude_code_version`.
- **UI.** `can_use_tool` → per-hunk inline diffs plus a permission dialog; AskUserQuestion → cards; ExitPlanMode → plan review; `rewind_files` plus fork → checkpoints (Write and Edit only); costs → usage widget. `/login` runs `claude auth login` in a terminal, then re-checks the gate. Sessions come from FrostIDE's own index; JSONL transcripts are read only, defensively.

**C4: ACP client (post-1.0).** ACP v1 schema; the Claude adapter (claude-agent-acp) is third-party.

### 5.2 Security
| Listener | Transport | Rules |
|---|---|---|
| IDE bridge | WebSocket on 127.0.0.1, OS-assigned port | Constant-time comparison of `X-Claude-Code-Ide-Authorization` against a 128-bit CSPRNG token. Any request with an `Origin` header is rejected (the CVE-2025-52882 class). No SSE fallback. Listens only after the first frame. Lock file with a current-user DACL, or 0600 inside a 0700 directory. |
| frostide-mcp pipe | ACL'd named pipe or AF_UNIX socket | Per-session tokens; the discovery file has the same ACL. |
| Hook endpoint (fallback only) | Loopback HTTP | Token check, Origin rejection, body-size cap, fuzzed with the bridge framing fuzzer. |
| Automation endpoint | ACL'd named pipe or AF_UNIX socket, never TCP | Compiled only into dev and test builds. Release builds refuse it unless started with `--automation=<token-file>`. |
| Single-instance pipe | ACL'd named pipe or AF_UNIX socket | Current user only; argument size cap. |
| frost-askpass pipe | ACL'd named pipe or AF_UNIX socket | Per-invocation token; prompts are always shown in the UI. |

Every mutating tool call requires approval (brave mode off); `claude` is never spawned on the cold-start path (§2.1, §3.4). The listener-enumeration test (§2.2) and negative tests cover every row above.

### 5.3 Terms and branding
- Never read, store or relay credentials (`.credentials.json` and the keychain are off-limits). Never bundle or modify the CLI; link to the official installer.
- Generic names ("AI Agent", "Agent Terminal (runs Claude Code, installed separately)"); no Anthropic logos or Claude Code ASCII art; plugin names avoid the reserved `claude-` and `anthropic-` prefixes.
- Native chat uses the enforced gate in §5.1; no unattended runs on subscription auth (Consumer Terms 3(7)).

### 5.4 Testing
- **cli-mimic** replays the CLI 2.1.316 handshake; negative tests (wrong token 401, `Origin` 403, lock-file ACL, stale locks) all fail closed; multi-window routing.
- **fake-claude** replays transcripts, malformed ones included: the Stop-hook loop guard, MCP size thresholds, and the C3 gate (a subscription-auth fixture refused before any session; API-key and third-party fixtures proceed). Argument goldens pin both launch variants: (b) always carries `--bare --ide --settings --mcp-config`, and (a) never carries `--bare`. Variant (a)'s golden uses an approval double that is linked only into test binaries. Release builds contain only the compiled constant, and a symbol check on release artifacts confirms the double is absent.
- **Zero-token real-CLI handshake** asserting `ide_connected`: the only real-CLI milestone gate. From M12 it also covers the C3 variant (b): `--bare --ide` with an `apiKeyHelper` fixture, `initialize` only and no user turn, asserting `system/init`, `ide_connected` and a connected `frostide` server. Variant (a) gets a real-CLI test only after an approval is recorded. Weekly golden transcripts per CLI minor (minimum 2.1.316); `docs/protocols/` records each field with its CLI version. Optional local E2E run (in `verify -Nightly`, never a CI job) with `--max-budget-usd 0.50` (D9).

---

## 6. Quality engineering

### 6.1 Test pyramid
- **Unit.** GoogleTest 1.18 and GMock, discovered with `gtest_discover_tests PRE_TEST`. Line coverage ≥80% on Qt-free libraries and ≥90% on parsers.
- **Integration.** Real MSBuild, CMake, clangd, git and lldb-dap against fixtures. In dual mode, every service also runs over the serialising transport.
- **Golden and differential**, with independent oracles where one exists (consistency checks are labelled as such): piece tree vs std::string; display map vs brute-force row mapping; grapheme navigation vs QTextBoundaryFinder; sln/slnx vs SolutionPersistence; extracted MSBuild flags compiled by `cl.exe /c` (independent) and compared with msbuild-extractor-sample (consistency, same engine); Clang false errors measured on the clean-TU set, defined beforehand as every TU cl.exe compiles without errors; File API vs Ninja compdb; diff vs `git diff --histogram`; formatter vs the clang-format binary; settings migrations vs goldens; VT, LSP and DAP vs replayed transcripts; frost-tidy via `check_clang_tidy`-style tests; refactorings compiled by clang-cl and cl.exe; natvis value goldens; GPU renderer vs raster by perceptual diff.
- **Fuzz.** libFuzzer with ASan (Windows: Qt-free /MT targets; the rest on Linux): sln, slnx, vcxproj, presets, File API, JSON-RPC/WebSocket framing, VT, piece-tree ops, `.theme.json`, settings, FSEG, journal, HTTP responses, pipe framing, natvis, SARIF. 60 s per target in `verify -Full`, 30 min in `verify -Nightly`; corpora committed.
- **Concurrency.** TSan over SimExecutor seeds (scheduling logic); TSan stress on the real Chase-Lev pool (races); a Relacy-style deque model check; MSan of the Qt-free core in its own configuration with MSan-instrumented libc++ (`verify.sh -Nightly`).
- **UI.** Qt Test offscreen with screenshot goldens (both themes, 100/125/150/175%, Windows and Linux; the agent reviews the per-OS goldens produced by local verification); pseudo-localization screenshots; scripted scenarios via the automation endpoint; UIA TextPattern and AT-SPI smoke; a scripted clean-profile first run (APPDATA, LOCALAPPDATA, XDG and CLAUDE_CONFIG_DIR redirected) on Windows and Linux.
- **Fault injection.** Killed child recovers within 2 s; kill -9 mid-save leaves the old or new file intact; disk full; locked file; symlink preserved; byte-exact CRLF/BOM; `--selftest=crash`/`=freeze` artifacts symbolize in `verify -Full`.
- **Security.** Malicious repository (§3.5), listener enumeration, no-network test.
- **Feature matrix.** `frost-featurecheck` requires every P0, P1 and infrastructure row to have an owning milestone (Appendix A) and a green acceptance test at that exit; it diffs the matrix against a checklist of IntelliJ editor settings pages. It also reads the generated inspection counts and fails a milestone exit when a target that milestone owns is missed: the registry matching LLVM's tables, the frost and CFG check counts, and ReSharper C++ coverage (M7 ≥30 frost, ≥2 CFG, ≥45%; M9 ≥45, ≥4, ≥60%; M13 ≥60, 5, ≥75%).
- **Manual checks.** NVDA, Narrator, Orca and IME passes are user-owned checklist items on REF-W1 (Orca through WSLg, or deferred if it is unusable there); macOS checks do not exist in v1 (D24). The agent never claims them as passed exit criteria.

"It works" means the scripted scenario passed and its screenshots were reviewed.

### 6.2 Deterministic doubles
IFileSystem/IFileWatcher/IClock → MemFs with a virtual clock; IProcessLauncher → scripted processes; IScheduler → seeded SimExecutor; ILanguageClient/IDebugAdapterClient → fake-lsp/fake-dap; IMsBuildEvaluator → fake-projecthost; ICMakeDriver → recorded File API replies; IVcs → scripted repos; IPty/IVtEngine → byte-stream replayer; IHttpClient → recorded responses with fault injection; ISettingsStore → in-memory scopes; ICrashReporter → in-memory sink; IPowerSource → scripted AC, battery, low-battery and battery-saver states; ISessionState → scripted locked/unlocked desktop and display-off states for the perf precheck; IAgentBackend → fake-claude and cli-mimic; IEditorRenderer/ITextShaper → offscreen golden renderer.

### 6.3 Local verification matrix
There is no CI/CD of any kind (D24): no GitHub Actions or other hosted pipeline, no hosted or self-hosted runner service, no cloud Mac. Every leg is a local script on REF-W1 or in WSL Ubuntu 24.04 on it, run by Claude during a session or by the user. The GitHub repository only hosts code, branches and PRs.

| Leg | When | Content |
|---|---|---|
| Windows (`tools/verify.ps1`) | Every change (`-Quick`); before every merge (`-Full`) | clang-cl (pinned LLVM) on STL 14.44: Debug, Release, ASan/UBSan, /MT fuzz smoke, `cl.exe /c` oracle, v143 fixtures (v145 once VS 2026 Build Tools are installed), listener and no-network tests, non-ASCII and long-path fixtures. |
| Linux (`tools/verify.sh` in WSL) | Every change (`-Quick`); before every merge (`-Full`) | The floor leg itself: clang (pinned LLVM) + libstdc++ 14 on Ubuntu 24.04, sources rsynced to ext4. ASan/UBSan, TSan with real-pool stress, coverage, clang-tidy, fuzz smoke, instruction-count perf and the wall-clock smoke (2x budget), no-network test; AppImage from M5. |
| Checks (both scripts) | Every change | Format, frost-lint, licence/SBOM against the SPDX allowlist (Qt Charts and Canvas Painter negative tests), layering, allowlist probe, vsdbg/cpptools artifact scan; OSV-Scanner in `-Full`. |
| Nightly tier (`verify.ps1 -Nightly`, `verify.sh -Nightly`) | Run manually, before milestone exits, or whenever the user chooses; never triggered by a CI service | 30-minute fuzz; MSan; the full REF-W1 perf run; corpus differentials; push to a throwaway private repository (from M5); opt-in real-CLI E2E (D9). |
| Optional legs | Once set up locally | GCC 16.2 second compiler and libstdc++ 15 (local podman containers in WSL); `llvm-next`; headless weston smoke; AppImage in Fedora, Debian, Arch and openSUSE containers. Until a leg exists it is reported as not run, never as passed. |
| Periodic probes | Weekly, run locally | Newest Claude CLI goldens; LLVM, Qt and vcpkg upgrade probes. |
| SDK | On demand | Official LLVM binaries where S5 allows; otherwise win-x64 built on REF-W1, linux-x64 in WSL. Artifacts stay under E:\sdk and `~/sdk`. |

**Perf runs on REF-W1.** Perf, S1 and on-screen UI runs happen in the logged-in interactive session (display kept awake with `SetThreadExecutionState(ES_DISPLAY_REQUIRED)`), never from a service: a service runs in session 0 without a desktop or DWM, which would invalidate S1, B6 and UI tests.
- **Session-state precheck.** A locked session or a powered-off monitor also invalidates frame pacing, because the user desktop is not composed and the DWM and Desktop Duplication probes return invalid samples. Before sampling, and again at the end, every perf, S1 and on-screen UI run therefore checks three things: the input desktop is unlocked (`OpenInputDesktop` plus the WTS session lock state); the session is the active console session; and DXGI reports an attached output whose monitor is powered on. If any check fails, the run is marked **invalid** (neither green nor red) and repeated later. `ISessionState` tests cover the precheck.
- **Unattended runs.** If the user wants a long perf run while away, `tools/perf-arm.ps1` turns off auto-lock, the screen saver and display sleep for that run and restores them afterwards. Locking the workstation manually aborts the run, which counts as invalid, not failed.

### 6.4 Local verification (CLAUDE.md definition of done)
`tools/verify.ps1 -Quick`:
1. Build `win-clangcl-debug` with sccache; `ctest -L unit|integration` for affected modules.
2. clang-format and clang-tidy on changed files; frost-lint, frost-licensecheck, layering check.
3. `wsl -d Ubuntu-24.04 -- tools/verify.sh`: directly in the WSL distro (it lives on E:\WSL\Ubuntu-24.04; toolchain in `~/sdk` from `tools/bootstrap-wsl.sh`, no container), rsync to `~/frost-src` on ext4, build `linux-clang-debug`, run unit tests offscreen, add TSan when sched, io or rpc changed.

`-Full` adds ASan/UBSan, fuzz smoke, screenshot tests, perf smoke and the real-tool suites (lldb-dap from M4, GDB ≥16 in `~/sdk` from M6); the GCC 16 build joins once its optional container exists. `-Full` on both sides is the merge gate (§6.7).

No test or budget is weakened without an ADR and user approval. Every commit carries a verification footer (e.g. `Verified: win 412/412, wsl 398/398, perf-smoke ok`). B28 tracks this loop's speed.

### 6.5 Coding standards
- LLVM-based clang-format, 4-space indent, 120 columns; `snake_case.hpp/.cpp` with `#pragma once`.
- Naming: namespaces `frost::lower_snake`, types PascalCase, functions and variables camelCase, members `m_camelCase`, constants `kPascalCase`, macros `FROST_`.
- Fallible APIs return `[[nodiscard]] std::expected<T, frost::Error>`; no exceptions for control flow or across modules (third-party exceptions caught at task boundaries). No raw owning `new`; strong offset types (`ByteOffset`, `Utf16Offset`, `LineIndex`).
- No narrow Win32 path APIs; child processes only through `IProcessLauncher` with absolute paths; user-visible strings through `frost::i18n::tr`; Qt only in `src/ui` and `src/app`.
- Each new dependency needs an SPDX allowlist check, an SBOM entry and an ADR.

### 6.6 Documentation convention
Code is documented with ordinary comments in every language in the repository. C++ and C# use `//` lines for short notes (up to three lines) and `/* */` blocks for longer explanations; CMake, PowerShell, bash, Python and workflow YAML use `#` lines. Doxygen and XML-doc tags are not required.

**Must be documented:**
- **Every source file** (C++, C#, CMake, PowerShell, bash, Python, workflow YAML): a header comment (purpose, module) and an `SPDX-License-Identifier:` line (D7).
- **Every other file** either has no comment syntax or must stay byte-exact. This covers JSON (budgets.json, matrix.json, the corpus manifest, settings, `.theme.json`, vcpkg.json), byte-exact fixtures and goldens (.sln/.vcxproj fixtures, CRLF/BOM goldens, transcripts), fuzz corpora and binary assets (icons, fonts). Its licence and copyright come from a root `REUSE.toml` (REUSE 3.x annotations by path glob) or a `.license` sidecar.
- **Public C++/C# declarations:** behaviour, ownership and lifetime, thread safety, preconditions, complexity, errors, and the budget ID on budgeted paths.
- **Script and CMake functions:** purpose, parameters, side effects (files written, environment variables, exit codes).
- **Non-obvious algorithms** cite their MIT, Apache or BSD source. **Workarounds** give the tool version, an issue link and the removal condition. **Magic numbers** give units. **Locks and atomics** state what they guard and the memory-ordering rationale. **Reverse-engineered fields** give source and CLI version. **TODOs** read `// TODO(M7): ...` or `# TODO(M7): ...`. **Generated code** carries each schema `description` as a comment.

**Enforcement.**
- frost-lint: tree-sitter rules for C++, C# and CMake; header and function-comment rules for PowerShell, bash, Python and workflow YAML; SPDX lines on source files; and, for every other file, coverage by `REUSE.toml` or a `.license` sidecar (frost-lint checks this itself, a `reuse lint` equivalent). Every `Budget: Bn` must exist in budgets.json; a `TODO(Mx)` must not name a closed milestone. Full rules from M0, no baseline exemptions.
- Each milestone exit includes a comment-accuracy review: an independent agent checks comments against behaviour for the code changed in that milestone; findings are fixed before sign-off.
- Separate Markdown is limited to CLAUDE.md, PLAN.md, ADRs, protocol notes and the progress log, never a substitute for code comments. Repository-meta files are allowed because they are not code documentation: README, LICENSE, NOTICE, SECURITY.md (a private vulnerability-reporting channel, which matters for the six local listeners in §5.2) and CONTRIBUTING. Release documents (notices, privacy statement, release notes) are generated artifacts.

```cpp
// Returns the line containing `offset`. O(log n); safe from any thread (snapshots are immutable).
// Budget: B5.
[[nodiscard]] LineIndex lineAt(ByteOffset offset) const noexcept;

/*
 * PieceTree stores a document as a persistent B+tree of pieces. Leaves reference
 * ranges in the original (possibly memory-mapped) file or in append-only add buffers;
 * inner nodes cache byte/newline/UTF-16 summaries so offset<->line<->UTF-16 lookups
 * are O(log n). Copying a snapshot only increments a reference count.
 */
```

### 6.7 Working agreement
| Topic | Rule |
|---|---|
| Commit permission | Granted (D23, 2026-10-31): Claude commits, pushes and merges under the standing permission recorded in CLAUDE.md. Repository: https://github.com/Frowsty/FrostIDE (private); pushes authenticate through Git Credential Manager. |
| Branches and PRs | One feature branch and PR per vertical slice; merge only with `verify.ps1 -Full` and `verify.sh -Full` green on REF-W1 and WSL and the verification footer present. No CI/CD pipeline is ever added (D24). |
| Review | An independent review (`/code-review` or a second agent) on every PR touching core, sched, text, storage or security code (listeners, trust, auth gate, updater, licence check); findings resolved before merge. |
| Feature-first phase | ADR 0031 (user decision, 2026-11-01): until U1, one agent per slice with simple functional tests and `-Quick`; the `-Full` and independent-review rules above, sanitizers, fuzzing, stress, perf gates and goldens move to a hardening phase after U1. Existing tests and budgets are never weakened. |
| Hand-off | `docs/progress.md` is the session log (done, next, blockers, questions); GitHub Issues is the backlog, labelled by milestone and matrix row. |
| Milestone exit | A demo pack: packages (from M5), screenshots or a recording, a perf report with trends, the feature-matrix diff, the ADR list, the comment-accuracy result. The next milestone starts only after explicit user sign-off. |
| Escalation | Claude stops and asks on legal questions (licences, terms, trademarks, borrowed code), costs (services, certificates), scope changes (any row moved or slice redefined) and any weakening of a budget or test. |
| Clean room | CLAUDE.md rule: never consult or copy GPL or proprietary IDE sources (Qt Creator, KDDockWidgets, Zed's GPL crates, JetBrains code); borrowed algorithms cite their MIT, Apache or BSD source in comments. The rule and the independent review are the primary controls; reviewers flag code that looks recalled from a named GPL or proprietary project. frost-licensecheck runs ScanCode Toolkit on diffs, but only for licence-notice detection (licence texts, SPDX tags, copyright statements). It does not match unattributed code fragments. Snippet matching would need ScanCode.io/MatchCode with a large local index, or SCANOSS, which sends fingerprints to a remote knowledge base. Neither is used in v1. Adopting one needs an ADR, would run only in local verification (never in the product), and would be recorded as an outbound endpoint (§2.2). |

---

## 7. Roadmap

**Estimate.** One unit is roughly one focused work-week of Claude Code sessions. 129–156 milestone units plus 9–12 for the upgrade track gives 138–168 units; with 10% contingency, about 152–185 weeks or 2.9–3.6 years sequentially (M14 around months 35–43). The upgrade track covers 5–7 LLVM majors at ~1 unit each, 5–7 Qt minors at ~0.5 each, and 12–14 quarterly vcpkg bumps (~1.5–2 units), because LLVM 24 lands around March 2027 and both projects ship about every six months (§2.3). The drivers are the full P0/P1 matrix (Appendix A), product infrastructure (§2.2) and review and sign-off at each exit. The thin path (D29, ADR 0029) costs about 9–11 units against the previous 120–145: three more milestone exits, interim versions that are later replaced (path (c) before ProjectHost, the large-file guard before large-file mode, the native frame before the frameless window, JSON scope files before the Settings dialog, parsed build output before the MSBuild logger), work done twice or added (M15's settings pages for keys M1–M5 added, a test-only SolutionPersistence oracle in M2, U1's acceptance scenario and its trust tests), M16's share of milestone overhead, and U1's minimal debugger, which moves about 3 units from M6 to M4 and adds up to 2 there (spike S10, its trust tests and the scenario's debug steps). In return the first usable build U1, with that debugger, arrives around month 10–12 instead of the old M4's month 12–15.

**Order.** The milestones run in the order of this section: M0, M1, M2, M3, M4 (first usable build U1), M5 (dogfood v0.1), M15, M16, M17, M6 (Preview 0.1), then M7–M14. IDs are identities, not positions (ADR 0029). M15–M17 are the deferred halves of M1–M4; they run before M6 so that every P0 row still lands by Preview 0.1 (§1.4).

**W1 first, then the rest of U1** (ADR 0032, the user's requirement statement of 2026-11-01: "I want a usable IDE to replace my current Visual Studio even if it has quirks and bugs which we will iron out with time", and for their generated solutions "basically what I need is something that can just open the .sln/.slx files, show my source files, let me edit them, show errors/warnings etc and let me compile"). Inside M1–M4 the work runs in this order:
1. **W1, the work-ready build,** right after M1's editor and shell: open a `.sln` or `.slnx` from the command line, File > Open and the recent list; a Solution Explorer built from static readers of `.sln`, `.slnx`, `.vcxproj` and `.vcxproj.filters`, without MSBuild evaluation; edit and save; Build, Rebuild and Clean of the solution or one project through the installed MSBuild (found with vswhere), with Configuration|Platform taken from the solution; Save All before a build and a trust prompt before the first build of a folder; the build output, and an error list of errors and warnings with click-to-jump and marks in the editor. These pieces come forward from M2 (the readers, trust) and M4 (the build driver, the error list) in minimal form; their full versions stay in M2 and M4. **Acceptance:** open, browse, edit and build a small generated solution of several linked projects with the real MSBuild on REF-W1, using a synthetic fixture in `tests/` shaped like the user's generated projects (the user's internal SDK is never committed).
2. **clangd diagnostics** as you type (M3), from compile commands: the generated CMake side's, or derived from the projects.
3. **Navigation and completion** (M3's core).
4. **The rest of U1**, as M1–M4 and "First usable build" below define it, the minimal debugger included (ADR 0029).

W1 is a checkpoint inside M1–M4, not a milestone: it has no ID in the ladder, and the milestone units, budgets and U1's content stay as they are. The static readers miss what imported `.props` and `.targets` files add until path (c)'s evaluation (M2) closes that gap, and errors appear after a build until live diagnostics follow (ADR 0032).

**Levers** (part of D5):
- Two agent streams in separate worktrees after U1 (e.g. M15 and then M17 alongside M5, with M16 after both, because M16 needs M5 and M15; or M7–M9 alongside M6 and M10): about 25–30% shorter calendar time, roughly twice the review load.
- Defer M10 (DbgEng adapter, memory views, extra test frameworks) and M12 to 1.1, saving 14–17 units; lldb-dap stays the Windows debugger and the Agent Terminal remains the way to chat.
- Move named P1 rows from Appendix A to 1.x.

**Every milestone exit also requires:** `verify.ps1 -Full` and `verify.sh -Full` green on REF-W1 and WSL, with the verification footer in the merge commit, including offscreen UI tests with Windows and Linux goldens reviewed (from M1); the no-network (from M1) and listener-enumeration (from M2) tests green; a tagged build, updated ADRs and a green matrix slice; the demo pack, the comment-accuracy review and explicit user sign-off (§6.7).

**Feature-first phase until U1** (ADR 0031, the user's decision of 2026-11-01: "I want to focus on pumping features. Make sure we get everything in, no need for the hour and minute long tests ... Once we have a functional IDE that can be used that's when we start going over the code"). From then until U1, or until the user ends it:
- One agent builds each slice: its whole scope (no feature is cut), simple functional tests (the happy path and the obvious edge cases, each fast), `verify.ps1 -Quick` on Windows and WSL, one commit. The orchestrator reads the agent's summary as the review and merges slices in batches, with one `-Quick` run of each merged batch (§6.7, "Feature-first phase").
- Not done in the phase: design critics, independent reviews, sanitizer builds, fuzzing, stress and differential runs, mutation checks, perf and budget gates, screenshot goldens and `-Full`. Each slice lists what it deferred, and `docs/progress.md` keeps the lists. For the phase this overrides §6.7's `-Full` and review rows, CLAUDE.md's independent-review rule, and the `-Full`, goldens and budget parts of the paragraph above.
- Still required, because they are cheap or legal: Clang only, FrostLayers layering, `std::expected` errors, wide Windows APIs, child processes through `IProcessLauncher`, the clean room, licence checks and a short ADR for each new dependency, SPDX lines, header and doc comments, no CI. No existing test, budget or warning is weakened or deleted, and every exit criterion and budget target of M1–M4 stays as written: budgets stay the targets, not gates, during the phase.
- **The hardening phase after U1** works through the deferred lists, gives the core, sched, text, storage and security code merged in the phase its independent review, and runs `-Full`, the sanitizers and the budget gates.

### M0: Bootstrap (3–4 units), signed off 2026-11-01
**Status.** D1 and D2 approved. `tools/bootstrap.ps1` has installed the pinned Windows toolchain under E:\sdk (LLVM 23.1.4, CMake 4.4.5, Ninja 1.13.2, gh 2.103.3, sccache 0.19.0, .NET SDK 10.0.403, a Python venv, Qt 6.12.0 msvc2022_64 via `tools/qt-fetch.py`, vcpkg 2026.10.16); MSVC 14.44.35207 and Windows SDK 10.0.26100.0 come from the existing VS 2022 Build Tools 17.14. Both phases of `tools/bootstrap-wsl.sh` are done: apt packages, then LLVM, CMake, Ninja, Qt 6.12.0 gcc_64 and vcpkg in `~/sdk`. The repository foundation, frost::sched, frost::io, the start-up hardening and the repository checks are merged on `m0/foundation`, with sccache as the Windows compiler cache. Spikes S3, S4, S5 and S9 have results (ADR 0001), as have S2 and S6 (ADRs 0005, 0006), S1 (ADR 0007), S7 (ADR 0008) and the Tracy version (ADR 0009); B28 is met (ADR 0010). **M0's exit is met (2026-11-01).** The M0 decisions are recorded in ADR 0011, on the user's instruction to follow the recommendations. The launch from a path over 260 characters is met with LongPathsEnabled on or off (ADR 0012, on the user's decisions "Use the launcher design" and "Restart via short link (Recommended)"): frost.exe is a small launcher that loads frost_app.dll through `\\?\`, and a start that may load a GPU driver from an image path of 260 UTF-16 units or more restarts through a short junction, because the driver ends such processes; matrix row I7 is green. `FrostProcess.PlatformPluginPathBoundary`'s ASan failure is resolved by the launcher, so `m0/e4-asan-runtime` is not merged (ADR 0011, E4). `verify.ps1 -Full` and `verify.sh -Full` are green on both platforms (`docs/progress.md`, entry "M0 complete"). B1's warm first frame still misses its 150 ms target, which stays M1's target (ADR 0011, A3).

**Deliverables.**
- `tools/bootstrap.ps1` (D1, D2 approved):
  - A precheck that stops if C: would fall below 15 GB or E: below 150 GB free. `bootstrap.ps1 -Vm` (run at M6) installs VirtualBox, sets its default machine folder to E:\vm (never `%USERPROFILE%` on C:), and repeats the precheck with ≥64 GB more required on E: for the install-smoke VM.
  - Into E:\sdk: LLVM 23.1.4 (the `.tar.xz`; the `.tar.zst` uses a 2 GiB zstd window the in-box bsdtar refuses), CMake 4.4.5, Ninja 1.13.2, a Python venv (the installed Python 3.14), Qt 6.12.0 win64_msvc2022_64 through `tools/qt-fetch.py`, vcpkg 2026.10.16, gh, sccache, the .NET 10 SDK (.NET 9 support ends 2026-11-10). No machine-wide changes; `tools/env.ps1` sets PATH and points `NUGET_PACKAGES`, `VCPKG_DOWNLOADS`, the vcpkg binary cache and `SCCACHE_DIR` at E: per session.
  - VS 2026 Build Tools (C++ v145) are deferred: the installer needs elevation, which the user runs; until then v145 fixtures and the 14.52 STL pair are not covered. VS 2022 Build Tools 17.14 (MSVC 14.44, SDK 10.0.26100) are already installed and checked by the script.
  - The Ubuntu-24.04 distro already lives on E:\WSL\Ubuntu-24.04, so no move is needed. `.wslconfig` mirrored networking is written when C1 needs it (M5).
- `tools/bootstrap-wsl.sh`: a root phase (`wsl -u root`; apt: g++-14/libstdc++-14-dev, zstd, Qt's xcb runtime libraries, python3-venv, rsync, gdb) and a user phase (pinned LLVM, CMake, Ninja, Qt 6.12.0 gcc_64 and vcpkg into `~/sdk` on ext4, plus `~/sdk/env.sh`). No container in v1. Claude inside WSL and GDB ≥16 are added when M5 and M6 need them.
- Repository foundation: `git init`, presets, triplets, per-compiler warnings; skeletons for `compat`, `core`, `sched` (with SimExecutor) and `io`; a `frost` window with the Windows manifest, `--version` and `--selftest`; verify scripts; frost-lint v0 (all languages, SPDX headers on source files, `REUSE.toml` coverage for everything else), frost-licensecheck (with every vcpkg port's declared licence expression checked against the allowlist), FrostLayers.cmake; CLAUDE.md (with the D23 commit permission and the D24 no-CI rule), README, LICENSE, NOTICE, SECURITY.md, the ADR template, `docs/progress.md`; no `.github/workflows` (D24); `matrix.json` generated from Appendix A; the corpus manifest and `corpus-fetch`.

**Spikes**, each an ADR with numbers:
- **S1:** cold and warm first frame; raster versus a minimal QRhiWidget atlas on 5120x1440, with the backend-neutral probes validated by injecting known delays.
- **S2:** `-getTargetResult:GetClCommandLines` latency on a fixture and R2a.
- **S3:** aqt offers the current Qt on Windows and Linux. **Result (Windows):** aqtinstall 3.3.0 cannot locate Qt 6.12.0, because the Windows repository moved to one directory per compiler (`qt6_6120/qt6_6120_msvc2022_64/Updates.xml`). `tools/qt-fetch.py` therefore installs Qt straight from download.qt.io, checking Updates.xml and every archive against SHA-256s pinned in `sdk/sdk.lock.json` (the SHA-1 published next to each archive only distinguishes a corrupt download) and extracting with bsdtar, whose output matched py7zr's file for file on both platforms. aqt still lists the Linux layout (`linux_gcc_64`).
- **S4:** the pinned LLVM, STL 14.44 and Qt msvc2022_64 link together.
- **S5:** which tools the official LLVM 23.1.4 packages contain, per OS. **Result (Windows):** clang-cl, lld-link, clangd, clang-tidy, clang-format, clang-include-cleaner, diagtool, clang-tblgen, llvm-rc, llvm-mt, the ASan/UBSan/libFuzzer/profile runtimes, and the Clang/LLVM CMake packages with libraries (so frost-clangd may build against it). Missing: lldb and lldb-dap, so U1's Windows lldb-dap is built from the pinned LLVM sources in M4 (ADR 0029, decision 12; before that, an open item for M6). **Result (Linux):** the same tools plus lldb, lldb-dap and lldb-server, and the TSan, MSan and HWASan runtimes; lldb-dap links `libpython3.14.so.1.0`, which Ubuntu 24.04 lacks (it ships 3.12), so M4 builds it without Python scripting or supplies a matching libpython. Neither package has libc++ (ADR 0001).
- **S6:** ProjectHost cold start (net472, MSBuildLocator, StreamJsonRpc) plus one R2a design-time build; fixes B23's first-project value before D3 is final, from 10 cold samples taken one per ordinary reboot (ADR 0011). B23's trees value stays provisional until M2's first trees measurement on R3.
- **S7:** stock clangd index load and first workspace/symbol on R3; sets B24's M3 value. **Result:** clangd 23.1.4 serves R3's persisted index (12,364 shards, 381 MiB) only once all of it is loaded, on one core: p50 15.3 s to both answers on a quiet machine (20.9 s under the user's own workload). Confirmed on 2026-11-01 (ADR 0011, C1): B24's M3 value of p50 18.4 s from the quiet run, and a quiet machine for the M3 gate (ADR 0008).
- **S9:** the baseline inspection count of the pinned LLVM 23.1.x. Configurable warning groups come from the diagnostic tables (`diagtool tree` if the package ships it, otherwise `clang-tblgen -gen-clang-diag-groups` over the 23.1.x sources), primary tidy checks from `clang-tidy -checks=* --list-checks` with aliases removed (406 expected), and CSA checkers from `clang -cc1 -analyzer-checker-help`. The counts are written to `matrix.json` as the reference that frost-featurecheck gates against, and replace any borrowed figure.
- Tracy version: 0.14.1, chosen in ADR 0009.

**Exit.**
- `verify.ps1 -Quick` and `verify.sh` green; ≥40 unit tests; `toolchain_probe` compiles the allowlist and rejects a banned construct.
- Interactive dispatch p99 ≤1 ms with 10k Background tasks queued and Background workers saturated.
- TSan clean over 10k SimExecutor seeds and a real-pool stress run (1M tasks across all lanes with cancellation, 100 iterations).
- Offscreen window test; manifest test (longPathAware, UTF-8 code page); launch from a path with non-ASCII characters, spaces and >260 characters.
- Licence negative tests (Qt Charts, a GPL-only fixture) fail as designed; frost-lint passes on every file (SPDX headers on source files, `REUSE.toml` or sidecar coverage for the rest); B28 met; the S9 counts are recorded.
- `verify.ps1 -Full` and `verify.sh -Full` green on REF-W1 and WSL; the M0 work is pushed to the private repository (D4). D2 is approved.

**Depends on** D1–D5, D7, D22 and D23 (all decided 2026-10-31).

### M1: Editor, shell and data-safety core (8–9 units)
**Thin path (ADR 0029).** M1 keeps what the first usable build U1 needs from the editor and the shell, and all of data safety. Everything else moved to M15 (listed under "Moved out") and is still required there.

**Scope.**
- **Editor:** piece tree, encodings, the undo tree and anchors; raster EditorView over the layered display map, built in its final order (folds → soft wraps → inlays → block decorations, §2): the soft-wrap and block-decoration layers work, and the fold and inlay layers are identity layers that M15 fills, so nothing below or above them changes then; soft wrap; block decorations for the compare/merge view's alignment fillers (and M5's diff viewer); tree-sitter highlight captures painted through the theme; smart keys, line operations; gutter; find/replace; Go to Line and next/previous member; tree-sitter (7 grammars, highlight and outline queries); grapheme navigation; IME preedit and commit. **A large-file guard until M15's large-file mode,** applied by the Qt-free document load service under every open path (the File System view, Ctrl+E, Go to File and Find in Files results, `frost <path>`, Build-window and Problems navigation, go-to-definition, session restore), on reload after an external change and on hot-exit restore: a file at or above the 128 MB mapping threshold is refused with a notification, and a file whose longest line exceeds the view's cap (set against B5) opens read-only, so neither can stall the UI thread. A reload or merge that would cross the threshold keeps the buffer and its journal entry as they are, marked out of date.
- **Data safety:** SaveManager, autosave, hot-exit journal, external-change handling with a minimal compare/merge view (histogram diff and merge cores); local-history snapshots on save.
- **Shell:** main window with the native frame and the toolbar; tabs; status bar (line:col, line separator, encoding, read-only lock, indent, processes); ToolWindowManager v1 (stripes, show and hide, resizing at fixed docking sides); popup framework; File System view with speed search; Ctrl+E, recent locations, back/forward, Find Action; notifications and background tasks; IntelliJ keymap (Windows/Linux; the macOS variant is defined but untested, D24), with every default shortcut reachable on de-DE; Glacier themes, compact mode, IDE zoom; session restore.
- **Foundations:** settings registry, scopes, `.frost/` and migrations, with the scope files opened in the editor from Find Action and reloaded live (the Settings dialog is M15's); crash handler and freeze reports; i18n and pseudo-localization; shortcut matcher; the no-network test; the automation endpoint (dev/test builds); frost-bench v1.

**Exit.**
- Piece-tree differential: 10M ops, 0 mismatches; clean 10-minute fuzz. Display-map property test against the brute-force oracle on the full four-layer stack, with soft wraps and block decorations in use and the fold and inlay layers as identities (M15 runs it again with folds and inlays).
- B1, B5 and B6 (soft wrap on), B19 (no project), B20 and B21 met; rendering-backend ADR recorded. The large-file guard's tests, through the load service: a 128 MB file is refused on open, F2 opens read-only, a clean buffer whose file grows past 128 MB is not reloaded, a dirty one's merge against such a file is refused with its journal entry kept, and a hot-exit restore next to a large on-disk file restores the journalled text; none of them stalls the UI thread for more than 50 ms.
- Data-safety fault injection green (kill -9 mid-save, disk full, locked file, symlink, byte-exact CRLF/BOM, journal restore after a kill). `--selftest=crash` and `--selftest=freeze` artifacts symbolize in `verify -Full`.
- Settings migration goldens and scope-precedence tests; shortcut tests for de-DE, pl-PL and fr-FR; grapheme tests.
- Screenshot goldens (both themes, 100/125/150/175%, Windows and Linux), contrast and pseudo-localization tests.
- The matrix rows owned by M1 are green, apart from the parts of rows 35, 55, 57, 94, 218, 227, 239, 240 and 242 that complete in a later milestone ("(+Mx)", Appendix A); their M1 parts are green.

**Moved out** (ADR 0029). To M15: multi-caret and column mode, folding with persistence, the contents of the display map's fold and inlay layers, whitespace and indent guides, Ctrl+wheel zoom, extend selection, EditorConfig, large-file mode with B15 and "F2 editable", the Structure view, rulers, clipboard history, drag and drop, splits, the frameless window with snap layouts, tool-window drag-docking and floating, the Settings dialog, keyboard navigation of stripes, toolbar and status bar with the focus-order tests, and the UIA TextPattern smoke.

**Depends on** M0, D13, D14, D20 and D24.

### M2: Project models and trust (9–10 units)
**Thin path (ADR 0029).** U1 opens, browses and evaluates CMake projects and MSVC solutions, and runs no project code before trust. MSBuild evaluation goes through path (c) (`MSBuild.exe -getTargetResult` with the wrapper target of ADR 0005). It runs the same engine and the same design-time targets as ProjectHost, so the fidelity oracles below apply unchanged; ProjectHost itself, solution writes and C# moved to M16. W1 (ADR 0032; §7, "W1 first") brings the static .sln, .slnx, .vcxproj and .filters readers, the Solution view and a trust prompt before a folder's first build forward in minimal form; the scope below is their full version.

**Scope.**
- **Solutions and MSBuild:** .sln/.slnx/.slnf readers; static .vcxproj reader with NMake projects; Solution view; path (c) as the evaluator, started only after trust and after L1, its results stored in ProjectModelCache with fingerprints; configuration switcher; the row 31 project actions Set as Startup Project, Open in Explorer and Copy Path.
  - **Deterministic MSBuild environment.** Path (c)'s `MSBuild.exe` starts from an empty environment (`ProcessSpec::cleanEnvironment`), never a copy of frost's own: the user's default environment block (`CreateEnvironmentBlock` without inheritance), the cached toolchain environment for the project's instance, architecture, SDK and vcvars_ver (§2, "Toolchain detection"), and the ADR 0011 E2 policy for trusted user-script children. The captured-environment fingerprint covers that whole block until ProjectHost's EnvironmentVariableRead capture narrows it in M16, so how frost was started (WT_SESSION, a changed TEMP, shell variables) cannot invalidate a record.
  - **Live re-evaluation.** A change to a .sln/.slnx, .vcxproj or .filters file, or to any .props or .targets file in a record's import closure (Directory.Build.* included), invalidates the affected records through the watcher, and path (c) re-evaluates them in the background, active configuration first (the MSBuild counterpart of CMake auto-reload, row 11).
- **CMake:** File API (generated sources flagged), presets, profiles (defined in settings), background configure and the CMake tool window.
- **Other sources and search:** compile_commands and open folder; Find/Replace in Files; exclude and mark directories.
- **Infrastructure:** toolchain detection; ProjectModelCache on FSEG; parallel scan and Go to File; synth-xl; the trust model; the Welcome window (recent projects and Open; Get from VCS works once M5's clone lands); single instance and `frost <path>`; listener hardening and the enumeration test.

**Exit.**
- **Fidelity:** SolutionPersistence equality on S50: 100% (SolutionPersistence runs in a C# test-only oracle that ships in no package; ProjectHost uses it for writes from M16). Every extracted command line for R2a and R2b compiles with `cl.exe /c` (independent) and matches msbuild-extractor-sample after normalization (consistency). File API vs Ninja compdb ≥99.9%. NMake fixture flags match its properties exactly.
- **Robustness:** all fuzzers clean; a mutation test per fingerprint class. Relaunching frost from a different parent environment (another shell, WT_SESSION set or unset, TEMP and unrelated variables changed) keeps every fingerprint valid and starts zero .NET processes; editing a .props file in a project's import closure re-evaluates exactly the records that import it.
- **Performance:** B2, B3 (zero .NET processes), B4, B7, B13 (unindexed), B23's trees variant and B23's first project streamed through path (c) on R2b met, each under its stated preconditions (for the first project: trust excluded, `MSBuild.exe`'s cold start included, the target of B23's row at the M2 exit). R2b cold open reported (target ≤30 s, not gated).
- **Security:** the malicious-repository test (including the planted tidy plugin and the `.frost/settings.json` tool path) runs nothing before trust, starts no path (c) `MSBuild.exe` and never runs the planted binaries; a CMake profile whose build directory outside the root comes from a repository file is ignored; the registry test finds no `executes` key honoured from project scope before trust; listener enumeration green.
- **Shell:** `frost path\to\R2b.sln` opens in the running instance within B3.
- **Platforms:** approximate-mode badge on Linux; tested against VS 2022 17.14, and against VS 2026 Build Tools once the user has installed them (otherwise reported as not run); the generated-sources hint appears.

**Moved out** (ADR 0029). To M16: ProjectHost (2–4 workers, EnvironmentVariableRead capture, the launch before L1 of ADR 0011 B4) with B23's first-project gate on ProjectHost and D3's finalisation, `.sln`↔`.slnx` conversion, New Project/Solution, the first-run wizard with its scripted clean-profile first run, C# projects in the tree, the Unload/Reload project and Properties actions, Show All Files and nesting, `frost --wait`, `diff` and `merge` with the `core.editor` round trip, file associations and the Jump List. To M15: the TODO window.

**Depends on** M1 and D16.

### M3: C++ intelligence core on stock clangd (7–8 units)
**Thin path (ADR 0029).** U1's clangd: completion, diagnostics and navigation with MSVC fidelity. The rest of C++ intelligence moved to M17. After W1, diagnostics as you type come first, then navigation and completion (ADR 0032).

**Scope.**
- LSP client and supervisor; clang-cl command synthesis; ResourceGovernor v1 and cache disk budget; Invalidate Caches / Repair IDE; system and SDK header indexing.
- Semantic tokens, inactive regions; diagnostics in the error stripe, the inspection widget, the status-bar error indicator and the Problems window.
- Completion (basic, #include, auto-import), Parameter Info, Quick Documentation.
- Navigation: declaration, base/implementations, header/source, Find Usages, Go to Class/Symbol, Search Everywhere v1.
- Alt+Enter fixes and tweaks; rename; clang-format, `.clang-tidy`; PCH and forced-include awareness; the MSVC dialect; C++23 (row 118, part 1).

**Exit.**
- LSP replay suite passes; B8–B11, B19 (R3), B22, B24 (M3 value), B25 and B26 met on R3.
- Zero Clang errors on R2a's clean-TU set; residual warnings published.
- Invalidate Caches removes every cache file of the project (directory listing) and the next open rebuilds them.
- A C++23 conformance fixture (deducing this, `if consteval`, multidimensional subscript, …) shows zero false errors.

**Moved out** (ADR 0029). To M17: inspection framework v1 (severities, suppression comments), inlay hints, Quick Definition, macro expansion, smart completion, Navigate To, type and call hierarchy, Select In and the navigation bar, intention preview, include-cleaner (unused and missing includes, optimize includes) and resolve context. No exit criterion moved.

**Depends on** M2.

### M4: Build, Run, Debug and Terminal. First usable build U1 (9–12 units)
**Thin path (ADR 0029).** U1's build, run, minimal debugger and terminal, and U1's acceptance scenario. C#, the MSBuild logger, run-configuration importers and the CMake language service moved to M16. The minimal debugger moved here from M6 on the user's answer to ADR 0029's Q4; the rest of the debugger stays in M6 and M10. W1 (ADR 0032; §7, "W1 first") brings an MSBuild build driver (the installed MSBuild found with vswhere, Configuration|Platform from the solution, Save All first) and an error list with click-to-jump and editor marks forward in minimal form; the scope below is their full version.

**Scope.**
- Build drivers for MSBuild and CMake inside a Job Object; the Build window, whose error and warning locations open the file at that line and column, feeding the Problems window; output decoded by code page and parsed with locale-independent anchors (localized-output decoding).
- Run configurations: the model (configurations derived from CMake executable targets and from the solution's startup project), the Run widget with Run, Debug and Stop, run options, Run console; Run and Debug build the target first (the general before-launch task list is M16's).
- Build, Rebuild, Clean, Run and Debug first save every modified document through SaveManager (Save All, as Rider does), so a build never compiles stale disk contents.
- The row 31 actions Build/Rebuild/Clean and Open in Terminal.
- **Debugger, the minimal one U1 needs** (ADR 0029, decision 12). The DAP client (`IDebugAdapterClient`, its fake-dap double, the DAP 1.71 types from frost-codegen) and one engine on both operating systems: **lldb-dap built from the pinned LLVM 23.1.4 sources**, the LLVM SDK artifacts where S5 found gaps (win-x64 on REF-W1 into E:\sdk, linux-x64 in WSL into `~/sdk`, §6.3), started under the supervisor at its absolute path and spoken to over stdio. It debugs MSVC-built programs through their PDBs on Windows and DWARF programs on Linux.
  - Debug from a run configuration (the Run widget's Debug action), with the same Save All and build first as Run.
  - Line breakpoints toggled in the gutter, shown as verified or unverified with the adapter's reason and kept per root in `.frost/workspace.json` (§3.6); the execution point in the gutter and the editor.
  - Step over, into and out; continue, pause and stop.
  - The Debug tool window: the call stack (frames loaded lazily), threads, variables (children loaded on expand), watches, Evaluate Expression and the program's output.
  - A stop on a crash or an unhandled exception (an access violation, `abort()`, a fatal signal, an unhandled C++ exception), with the faulting frame selected and the reason shown. Breaking when an exception is thrown is an exception breakpoint (row 170, M6).
  - Values as lldb-dap's default formatters show them; FrostIDE's STL formatters are M6's.
  - No symbol servers or debuginfod, which stay off until enabled per project (§2.2): lldb-dap starts without `_NT_SYMBOL_PATH`, `_NT_ALT_SYMBOL_PATH` and `DEBUGINFOD_URLS` in its environment.
  - **Spike S10, first in M4:** lldb-dap 23.1.4 on R2a's cl.exe-built Notepad++ (Debug|x64, with PDBs): breakpoints bind, stepping, the stack through the CRT start-up frames, threads, locals of built-in types, pointers, arrays and structs, a crash fixture, and what the default formatters show for MSVC STL types (recorded, not gated); and on each OS whether those default formatters need lldb's Python scripting, which decides ADR 0001's open choice for the build. If S10 shows that lldb-dap cannot carry the U1 scenario's Windows debug step, M4 stops and the user decides: the alternative, the DbgEng adapter (M10), needs the ComponentDownloader, network consent and a legal review first (D12, L5).
- **Trust:** in an untrusted root, Build, Rebuild, Clean, Run, Debug and the build before Run or Debug are refused with the trust banner, and the terminal follows §3.5's restricted rule (the application-scope shell, NoDefaultCurrentDirectoryInExePath kept).
- Terminal: ConPTY or forkpty, patched libvterm, win32-input-mode and terminal IME. **Spike S8:** which VT queries the bundled ConPTY answers itself and which reach the host when claude.exe runs, and which claude state the TUI probe needs (below).
- The U1 acceptance scenario (below) with its corpus setup, and a tagged U1 build under E:\FrostBuilds.

**Exit.**
- R2b and R3 build from inside the IDE; Job Object cancel test; parser goldens for en, de and ja VS output.
- Terminal goldens; the Claude TUI probe passes on Windows (through ConPTY) and Linux: DECRQM 2026, XTVERSION, Shift+Enter reaching claude.exe with its modifier, ESC reaching the PTY; composed IME text reaches ConPTY. B17 met. The probe runs the user's claude.exe with its own `CLAUDE_CONFIG_DIR` set to a pre-seeded probe configuration (onboarding complete, auto-update and non-essential traffic off through `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1`, no credentials unless S8 shows the input prompt needs them, then the D9 test key); it never submits a prompt, so it spends no tokens (D9).
- The lldb-dap builds (the SDK artifacts built here) are stored under E:\sdk and `~/sdk` and pinned with hashes in `sdk/sdk.lock.json`.
- **Debugger:** the DAP conformance scenarios for every request and event M4 uses (initialize, launch, setBreakpoints, configurationDone, threads, stackTrace, scopes, variables, evaluate, next, stepIn, stepOut, continue, pause, disconnect and terminate; stops for a breakpoint, a step, a pause, an exception and a signal; output, exited and terminated) pass against lldb-dap on Windows and Linux and replay against fake-dap; M6 grows the suite to ≥60 scenarios. Crash fixtures built with cl.exe on Windows and clang on Linux (a null dereference, `abort()`, an unhandled C++ exception) stop with the faulting frame selected. Variable goldens per OS for built-in types, pointers, arrays, structs and classes (the STL value goldens are M6's). FrostIDE debugs itself (R1); B16 met.
- The malicious-repository test (§3.5) extended to M4: before trust, Build, Rebuild, Clean, Run, Debug and the build before Run or Debug are refused, and no lldb-dap starts; the planted File API reply's target binary and the `.frost/runConfigurations` command never run; the terminal starts the application-scope shell, never a planted one, and `git status` typed in cmd.exe there does not run the planted `git.exe`.
- The no-network test and the listener enumeration cover a debug session on each OS: lldb-dap's process tree opens no connection and no listener.
- The U1 acceptance scenario, with its debug steps, passes on Windows and Linux.

**Moved in** (ADR 0029, Q4). From M6, for U1's minimal debugger: the DAP client and lldb-dap (the debugger engine, row 167, whose GDB DAP profile stays in M6) with its Windows source, the open item S5 left, now built from the pinned LLVM sources with the LLVM SDK artifacts and their pinning; line breakpoints (row 169; conditions, hit counts and log messages stay in M6); stepping (row 173; Run to Cursor and the other stepping actions stay in M6); the Debug window (row 175); Evaluate and watches (row 178); FrostIDE debugging itself and B16.

**Moved out** (ADR 0029). To M16: the dotnet build driver, FrostIDE.MSBuildLogger, C# solution citizenship (R5 builds from inside the IDE), the run-configuration importers, the before-launch task list, gutter run icons, the CMake language service with its completion and navigation goldens on R3, and the cache viewer.

**Depends on** M2 and M3 (U1 needs both).

### First usable build (U1, the M4 exit)
**Definition.** The smallest FrostIDE with which the user does real C++ work on a CMake project and on an MSVC .sln/.slnx/.vcxproj solution (D29, ADR 0029). Everything below is built and gated in M1–M4; anything not listed is in M5 or later and still required there. W1, the work-ready build (ADR 0032), is the first checkpoint on the way, and the feature-first phase (ADR 0031) defers U1's sanitizer, fuzz, stress, perf-gate, golden and `-Full` checks to the hardening phase after U1's features are in (§7, "W1 first" and "Feature-first phase").
- **Open:** the Welcome window and `frost <path>` (single instance); .sln/.slnx/.slnf and .vcxproj through the native readers, evaluated by path (c) after trust; CMake through the File API, presets and profiles with background configure; compile_commands and open folder; restricted mode until the root is trusted.
- **Browse:** Solution view with .filters folders, the CMake target tree, the File System view with speed search; Go to File, Class and Symbol; Search Everywhere v1; Find/Replace in Files; recent files and locations; back and forward.
- **Edit:** tree-sitter highlighting, then semantic tokens and inactive regions; undo and redo; smart keys and line operations; find/replace; soft wrap; encodings, BOM and EOL preserved; IME; the IntelliJ keymap on any supported layout (de-DE included); Find Action; tabs; Glacier themes.
- **Data safety:** atomic saves that preserve encoding, EOL, permissions and links; autosave; the hot-exit journal and crash recovery; external changes reloaded (clean) or merged (dirty); local-history snapshots; the fault-injection suite.
- **Intelligence:** clangd with clang-cl command synthesis, PCH and forced includes: completion with #include and auto-import, Parameter Info, Quick Documentation, diagnostics in the editor, error stripe and Problems window, Alt+Enter fixes, declaration, implementations, header/source, Find Usages, rename, clang-format.
- **Build and run:** MSBuild and CMake builds with cancel, after Save All; the Build window's errors open their location; run configurations with build-before-run, the Run widget, the Run console and Stop.
- **Debug:** a minimal debugger on Windows and Linux, lldb-dap built from the pinned LLVM sources (ADR 0029, decision 12): Debug from a run configuration, after Save All and the build, as Run does; line breakpoints in the gutter; step over, into and out, continue, pause and stop; the Debug window with the call stack, threads, variables, watches, Evaluate and the program's output; a stop at the faulting frame on a crash or an unhandled exception. MSVC-built programs are debugged through their PDBs.
- **Terminal:** ConPTY or forkpty with libvterm, passing the Claude TUI probe, so the user's own Claude Code CLI runs in it until the C1 bridge (M5).
- **Fast:** B1, B5, B6, B19 (no project), B20 and B21 (M1); B2, B3, B4, B7, B13 (unindexed), B23 trees and B23's first project through path (c) (M2); B8–B11, B19 (R3), B22, B24 (M3 value), B25 and B26 (M3); B16 and B17 (M4).
- **Safe:** the trust model with the malicious-repository test (nothing runs before trust, Build, Run, Debug and the terminal included); listener hardening and the enumeration test; the no-network test; the `executes`-key registry test; crash and freeze reports kept locally.

**Acceptance scenario** (`tests/e2e/u1`, driven by frost-bench through the automation endpoint, under a clean profile: APPDATA, LOCALAPPDATA, the XDG directories and CLAUDE_CONFIG_DIR redirected, with no recent projects, trust or session state).
- **Setup** (`tests/e2e/u1/setup`, outside the scenario and its timings; once per corpus pin, and again when the pin changes):
  - **R3 on Windows.** Configure `E:\corpus\R3-build` from the source directory `E:\corpus\R3\llvm` with R3's pinned configuration (`tests/corpus/manifest.json`) and a toolchain file outside every worktree: the existing cache names the toolchain file of a worktree that has since been removed, so it cannot reconfigure. Write FrostIDE's `client-frostide` File API query into it before that configure, so its replies exist, and build `not` with its dependencies, which `R3-build` lacks today (`bin\not.exe` is absent), so that no LLVMSupport build happens inside the scenario.
  - **R3 on Linux.** corpus-fetch R3 at the same pin into `~/corpus/R3` on ext4, and configure `~/corpus/R3-build` with Ninja, Debug, `LLVM_ENABLE_PROJECTS=clang` and the pinned clang with libstdc++ 14, recorded in the manifest as R3's Linux configuration (none is pinned today), with the same query; then build `not`. Debug, not Release, because step 7 debugs `not` and LLVMSupport: a Release build has no line tables, so its breakpoints would not bind, and optimized locals would not be readable.
  - **Profile seed.** The clean profile gets two things and nothing else: an application-scope CMake profile for the root `R3\llvm` naming that build directory, Ninja and its build type (Release on Windows, Debug on Linux; llvm's CMakePresets.json defines no configure preset with a build directory, so a clean profile would otherwise configure a new one), and R3's persisted clangd index in the profile's cache, built once per pin on the same build directory by an unrecorded run, as for B24.
- **Recorders.** The no-network recorder (ETW Kernel-Network on Windows, `strace -f -e trace=connect` on Linux) and the listener enumeration run for the whole scenario. They attribute every connection and listener to a process and its ancestors, and they assert on FrostIDE's own process tree: frost, its crash handler, and the engines and tools it supervises (clangd, path (c)'s `MSBuild.exe`, CMake configure, git, lldb-dap and the debug server it starts). That tree opens no network connection and only the expected listeners (§5.2). The trees that Build, Run, Debug and the terminal start (MSBuild nodes and their pipes, `mspdbsrv.exe`, ninja and the compilers, `not`, Notepad++, the shell and whatever runs in it) are the user's code: their connections and listeners are recorded and listed in the report, not asserted, even when lldb-dap is the program's ancestor.
- **Samples.** The scenario records its own B1, B2, B3, B8, B9, B10, B11, B17 and B20 samples and fails any of them above 2x budget, the wall-clock smoke rule of §4; the gating numbers come from the B-suite. B16's corpus is R1, so the B-suite gates it there, not this scenario.
- **Clean-up.** Corpus files it edits are restored. The scenario ends by asserting that `git status --porcelain --untracked-files=no` of each corpus is empty and that the only untracked path FrostIDE created there is the root's `.frost/` directory (project-personal state, §3.6).

1. **CMake (R3, Windows; on Linux the same steps run on `~/corpus/R3`).** Start frost and open the source directory `E:\corpus\R3\llvm`. In restricted mode the File System view and, from the existing replies in `R3-build`, the target tree appear (B23 trees), and no project process starts. Trust the root from the banner: the background configure runs (CMake tool window), and the target tree still shows `not`.
2. Go to File `utils/not/not.cpp`: tree-sitter highlighting, then semantic tokens. Go to Declaration on `ExecuteAndWait` lands in `include/llvm/Support/Program.h`; Back returns. Once the status bar shows L2 (the seeded index loaded, B24), Find Usages lists its call sites, among them `not.cpp`, `lib/Support/GraphWriter.cpp` and `lib/Support/Signals.cpp`, and Switch Header/Source on `include/llvm/Support/WithColor.h` opens `lib/Support/WithColor.cpp`.
3. At the top of `main`'s body, before `if (argc == 0) return 1;` (where a run without arguments returns), type `llvm::ou` and accept `outs` from completion, completing the line `llvm::outs() << "frost-u1\n";`. Below it add `int frostU1 = undeclaredName;`: the diagnostic appears in the editor and in the Problems window.
4. Kill frost (TerminateProcess on Windows, SIGKILL on Linux) with the buffer dirty and restart: the hot-exit journal restores the buffer byte-exactly and still dirty, and the root is still trusted.
5. Build `not`: Save All writes the restored buffer first (M4), the Build window shows the compile error, and opening it puts the caret on that line and column. Delete the line with Ctrl+Y, save, build again: the build succeeds.
6. Run the `not` configuration with no arguments: the Run console shows `frost-u1` and exit code 1.
7. **Debug (Linux).** Toggle a line breakpoint in the gutter on the `llvm::outs() << "frost-u1\n";` line and start the `not` configuration with Debug and no arguments: Save All and the build run first and find nothing to rebuild, lldb-dap stops at the breakpoint with the execution point on that line, and the Debug window shows `main` on the call stack, the main thread, and `argc` as 1 in Variables. Step Into enters `llvm::outs()` in `lib/Support/raw_ostream.cpp`; Step Out returns to `main`; Step Over until the execution point is on the line after `--argc;`: a watch on `argc` shows 0, and Evaluate `ExpectCrash || argc != 0` shows `false`. Continue: the program exits with code 1, and the Debug window shows `frost-u1` in the program's output and the exit code. On Windows this step is not run: R3's Windows build directory is Release, and the Windows debug step uses R2a (step 12).
8. In the terminal (the root is trusted), run `git -C <root> checkout -- utils/not/not.cpp` (an external change, as Claude Code in the terminal makes them): the clean buffer reloads as an undoable edit. Build and run again: no `frost-u1` line. Exit frost (B20), start it again: R3 reopens to L1 within B3 with zero .NET processes, and the active tab is highlighted within B2.
9. **MSVC (R2a, Windows).** Open `E:\corpus\R2a\PowerEditor\visual.net\notepadPlus.sln` from the Welcome window: the Solution view with its .filters folders appears in restricted mode, and no MSBuild process starts before trust. The banner offers the root `E:\corpus\R2a`, the solution's git work tree, which holds the Scintilla and Lexilla projects beside `PowerEditor` (§3.5). Trust it: path (c) extracts the command lines, and clangd shows no error in `PowerEditor\src\EncodingMapper.cpp` (a TU of R2a's clean-TU set, M3), where completion and Go to Declaration work.
10. Select Debug|x64 in the configuration switcher. Add an undeclared identifier to `EncodingMapper.cpp` and Build: Save All writes it, the Build window shows MSVC's C2065, decoded from the build's code page, and opening it puts the caret on the line; Problems lists it. Delete the line, save and build: Notepad++, Scintilla and Lexilla build.
11. Run the Notepad++ startup project with `-multiInst -nosession -noPlugin -settingsDir=<scratch>`, so it writes nothing outside the build tree and the scratch directory: the Run widget shows it running, Stop ends its process tree through the Job Object, and the Run console reports the exit.
12. **Debug (R2a, Windows).** Toggle a line breakpoint on `parseCommandLine(pCmdLine, params);` in `wWinMain` (`PowerEditor\src\winmain.cpp`) and start the Notepad++ configuration with Debug and step 11's arguments: the Debug|x64 binary that cl.exe built stops there through its PDB, and the Debug window shows `wWinMain` on the call stack above the CRT start-up frames, the main thread, and `pCmdLine` holding the arguments in Variables. Step Into enters `parseCommandLine`; Step Out returns to `wWinMain`; Step Over until the execution point is on the line after `bool doPrintAndQuit = isInList(FLAG_PRINTANDQUIT, params);`: a watch on `isMultiInst` shows `true`, and Evaluate `doPrintAndQuit || doFunctionListExport` shows `false`. Continue: Notepad++'s window appears. Pause: the Debug window lists Notepad++'s threads with the selected thread's stack. Stop ends the session and Notepad++'s process, and the Debug window reports the exit.
13. In the same session's terminal, run the M4 Claude TUI probe with its pre-seeded probe configuration (M4 exit): the terminal's `CLAUDE_CONFIG_DIR` points at a copy of that configuration, not at the profile's empty one, and the probe submits no prompt.
14. **Linux, approximate mode.** Open R2a's solution in WSL (read from `/mnt/e`; nothing is built there): the approximate-mode badge, the Solution view and highlighting appear. Build, Run and Debug of MSVC solutions are Windows-only.

**Not in U1** (ADR 0029 lists every item and where it went): Git and the Claude C1 bridge (M5); editor and shell polish (M15); ProjectHost, solution writes, C# and the CMake language service (M16); the rest of C++ intelligence (M17); the rest of the debugger (M6: the GDB DAP profile, function and exception breakpoints, the Breakpoints dialog, conditions and log messages, Run to Cursor, Set Value, inline values, the debugger console, attach, STL formatters beyond lldb-dap's defaults, the CMake debugger; M10: the DbgEng adapter with natvis, minidumps and symbol servers) and the test runner (M6); adding, removing and renaming files in a project (M9; until then the files are edited in the .vcxproj or CMakeLists, which re-evaluate live); and everything planned for M7 and later.

### M5: Git, Claude C1 and packages. Dogfood v0.1 (8–10 units)
**Scope.**
- **Git:** status; Commit window (changelists, partial commits, amend); branches; stash and shelve; diff viewer; compare files, folders, clipboard; VCS colours; clone, fetch, pull (merge/rebase), push (force-with-lease), remotes, Update Project; `frost-askpass`, commit-signing detection and prompts (§2 VCS), LFS detection, hook-output console; conflict list with accept ours/theirs.
- **Claude:** the C1 bridge with multi-window semantics, the Agent Terminal and the status widget.
- **Distribution:** portable packages for Windows and Linux, carrying U1's lldb-dap build (kept local or in the private repository, D8), symbol archival, Collect Logs, `IHttpClient` on Windows, the AppImage build.

**Exit.**
- Diff output matches `git diff --histogram`; B14 and B18 met.
- Git remote tests: local bare repositories with scripted askpass; an SSH server in WSL (host-key and passphrase prompts); a push to a throwaway private repository in `verify -Nightly`.
- Signed-commit tests: `gpg.format=ssh` with a passphrase-protected key through frost-askpass and through a running ssh-agent; OpenPGP with a test key in a scripted gpg-agent; and a no-agent / TTY-only-pinentry case that is detected before the commit or fails within the timeout with a clear error, never hanging.
- cli-mimic, security and fake-claude suites pass; zero-token real-CLI handshake on Windows and WSL; the Ctrl+Esc default decided by its test.
- A scripted edit → diff → tweak → accept scenario passes.
- Package smoke: `--selftest` from each freshly extracted package under a clean profile on REF-W1 and in WSL; B27 met; archived symbols symbolize a `--selftest=crash` dump.
- Dogfood end-to-end: clone the FrostIDE repository, edit, build, run a test, commit and push.

**Depends on** M3, M4, D8, D9 and D26.

### M15: Editor and shell completion (6–7 units)
The deferred half of M1, and the TODO window from M2 (ADR 0029).

**Scope.**
- **Editor:** the display map's fold and inlay layers, filling the identity layers that M1 built; folding with persistence; multi-caret and column mode, with the column-mode status widget; whitespace and indent guides; Ctrl+wheel zoom; extend selection; EditorConfig; large-file mode (mapped originals of 128 MB and more, summaries computed in parallel, a first screen before the full line index, the background release of big trees), which replaces M1's large-file guard; the Structure view; rulers, clipboard history, and drag and drop in the editor.
- **Shell:** editor splits; the frameless window with snap layouts (`HTMAXBUTTON`); ToolWindowManager drag-docking and floating; the Settings dialog (§3.6), with the Keymap page of ADR 0016 and the pages for every key M1–M5 added; keyboard navigation of stripes, toolbar, status bar and gutter (§2.2); the UIA TextPattern smoke; the TODO window.

**Exit.**
- Display-map property test against the brute-force oracle across all four layers, with folds and inlays in use.
- B15 met; F2 editable; B5 and B6 still met with folds, inlays and several carets in use.
- Focus-order tests; the UIA TextPattern smoke; screenshot goldens for the new UI (both themes, 100/125/150/175%, Windows and Linux); a registry test that every key M1–M5 added has a Settings page.
- TODO-window, ruler, clipboard-history and drag-and-drop tests (rows 115, 252, 254 and 255).
- The matrix rows owned by M15, and the M15 parts of rows 35, 55, 239, 240 and 242, are green.

**Depends on** M1, M2 (the TODO window searches through M2's Find in Files) and M3 (EditorConfig maps onto M3's `.clang-format` handling); runs after M5, or alongside it (§7, Levers).

### M16: ProjectHost, solution writes, C# and CMake language (9–11 units)
The deferred halves of M2 and M4 (ADR 0029).

**Scope.**
- **MSBuild:** `FrostIDE.ProjectHost` (path (b): 2–4 workers, active configuration first, a shared EvaluationContext, EnvironmentVariableRead capture), launched before L1 in a trusted root as ADR 0011 B4 decides, becomes the source of truth; path (c) stays as fallback and consistency check. `FrostIDE.MSBuildLogger` streams build events into the Build window.
- **Solutions:** `.sln`↔`.slnx` conversion and New Project/Solution (CMake executable/library; .slnx + .vcxproj written via ProjectHost and SolutionPersistence); the Unload/Reload project and Properties actions; Show All Files and nesting.
- **C# solution citizenship:** C# projects in the tree; the dotnet build driver; build and run R5 (launchSettings.json); navigation to build errors.
- **First run and shell integration:** the first-run wizard (§3.4); `frost --wait`, `frost diff` and `frost merge`; file associations; the Jump List.
- **Run configurations:** importers for `launch.vs.json`, `.vcxproj.user`, `.idea` and `launchSettings.json`; before-launch tasks; gutter run icons for `main()`.
- **CMake:** the language service (completion, go-to-definition for functions, macros, targets and include(), Find Usages, Quick Doc, formatting) and the cache viewer.

**Exit.**
- Every extracted command line for R2a and R2b, now from ProjectHost, compiles with `cl.exe /c` (independent) and matches path (c) and msbuild-extractor-sample after normalization (consistency).
- B23's first project streamed met on ProjectHost under its preconditions (ProjectHost cold start included); B3 still met with zero .NET processes; the environment-relaunch test of M2 still passes with EnvironmentVariableRead capture.
- The scripted clean-profile first run passes on Windows and Linux, with screenshot goldens; `git -c core.editor="frost --wait" commit` round-trips.
- R5 builds from inside the IDE, and its build errors open their location.
- CMake completion and navigation goldens on R3's CMakeLists.
- The malicious-repository test also covers the importers: imported run configurations and before-launch tasks run nothing before trust.

**Depends on** M2, M4, M5 (the wizard's Claude page uses M5's CLI detection), M15 (the wizard and the Properties action open Settings pages), D3, D6, D27 and B23's first-project value from the S6 cold samples (ADR 0011, B1).

### M17: C++ intelligence completion (5–6 units)
The deferred half of M3 (ADR 0029).

**Scope.**
- Inspection framework v1: severities and suppression comments (row 99's first part).
- Inlay hints (on M15's inlay layer), Quick Definition, macro expansion, smart completion, intention preview.
- Navigate To; type and call hierarchy; Select In and the navigation bar, with its status-bar part (row 242); resolve context.
- include-cleaner: unused and missing includes, optimize includes.

**Exit.**
- The LSP replay suite covers every request added here; B8–B11 still met on R3 with inlay hints on.
- Severity and suppression tests: a suppressed diagnostic stays hidden across edits, saves and reopens.
- A resolve-context fixture: a header included from two projects with different defines shows each context's inactive regions and diagnostics.
- The matrix rows owned by M17 are green.

**Depends on** M3 and M15.

### M6: Debugger v1, test runner, installers, updater. Preview 0.1 (9–11 units)
**Thin path (ADR 0029).** M4 has U1's minimal debugger (the DAP client, lldb-dap on both operating systems, line breakpoints, stepping, the Debug window, Evaluate and watches); M6 completes debugger v1 on it.

**Scope.**
- **Debugger:** the GDB DAP profile; function and exception breakpoints and the Breakpoints dialog; conditions, hit counts and log messages on line breakpoints (row 169); Run to Cursor and the other stepping actions (row 173); Set Value, inline values, debugger console; attach; LLDB STL formatters beyond lldb-dap's defaults; CMake debugger; ptrace_scope detection.
- **Test runner:** GoogleTest and CTest run and debug, with a results tree; Run Unit Tests actions; the pre-commit run-tests check.
- **Distribution:** unsigned per-user MSI and AppImage; the updater and ComponentDownloader; `IHttpClient` on Linux.

**Exit.**
- DAP conformance suite of ≥60 scenarios, M4's included; STL value goldens; the CMake debugger stops at a breakpoint in R3's CMakeLists.
- Updater: a tampered manifest is rejected, a downgrade refused, an interrupted download resumed, a rollback restores the previous version. Uninstall and upgrade-in-place tests; install smoke on the floor-build VM (§1.3, stored on E:\vm) and in WSL; the tested Windows build is recorded.
- frost-featurecheck shows every P0 row owned by M0–M6 and M15–M17 green (rows 167, 169 and 173, which M4 starts, complete here); the only open P0 rows are 31, 99, 118, 227 and 242, each with its parts in those milestones green (§1.4).

**Moved out** (ADR 0029, Q4). To M4, for U1's minimal debugger: the DAP client and lldb-dap, with the Windows lldb-dap source (the open item S5 left; M4 builds it from the pinned LLVM sources, with the LLVM SDK artifacts and their pinning, which return to M4); line breakpoints, stepping, the Debug window, Evaluate and watches; FrostIDE debugging itself and B16.

**Depends on** M4, M5, M15, M16, M17 and D25 (certificate lead time).

### M7: frost-clangd and analysis tiers (10–12 units)
**Scope.**
- frost-clangd for Windows and Linux (win-x64 on REF-W1, linux-x64 in WSL), built against the official LLVM packages where S5 shows their libraries suffice, otherwise against a self-built LLVM; the engines' local `llvm-next` leg.
- T2 with PCH cache and caps; T3 SARIF ingestion; T4; headless `frost inspect`; the generated registry; frost and CFG checks; naming, class-design and header-guard checks; profiles and scopes (row 99); solution-wide analysis with the status-bar analysis monitor (row 242); include hierarchy; IWYU; Power Save Mode; libFormat in its DLL.
- Row 118, part 2: clangd modules behind the experimental flag, with a modules fixture, and downgrading of unsupported constructs.

**Exit.**
- The registry holds all configurable Clang warnings and primary tidy checks of the pinned LLVM and ≥100 CSA checkers. Its count is generated from LLVM's tables at build time and checked against them (S9 baseline, regenerated per LLVM major), and frost-featurecheck fails the exit on any shortfall; ≥30 frost and ≥2 CFG checks; ≥45% coverage of ReSharper C++'s own inspections.
- Each check adds ≤5% overhead; B11 and B12 hold; engine crashes stay isolated; SARIF is schema-valid; `frost inspect` equals the IDE's results on R1.
- The modules fixture shows no false errors with the flag on; the reflection fixture shows downgraded hints and zero errors.

**Depends on** M3, M4 and M17 (inspection framework v1).

### M8: Claude C2 (3–4 units)
**Scope.** `frostide-mcp` and its discovery file; `mcp_tool` hook handlers and the Stop-hook loop guard; the `frostide-tools` plugin; the result-size policy.

**Exit.**
- MCP conformance for every tool, including the 50,000-character and 25,000-token thresholds and the explicit `maxResultSizeChars` tools; mutating tools require approval.
- fake-claude: the Stop hook blocks until an introduced error is fixed, and with an unfixable error downgrades to a warning after 3 blocks.
- The real CLI lists `frostide` as connected from a FrostIDE-launched session and from an external `claude mcp add` session; the hook-merge test passes.
- Listener enumeration shows no HTTP hook listener by default.

**Depends on** M5, M7 and D11.

### M9: Refactoring, code generation and templates (11–13 units)
**Scope.**
- Multi-file refactorings: rename with files and #include updates, Change Signature, Extract Function, Introduce/Inline Variable, Safe Delete, Move; preview, conflict view, atomic undo; Refactor This.
- Generate, Create from Usage, Implement/Override, generate definitions; live templates (C++ and CMake), Surround With, file templates, Doxygen stubs.
- Add, remove and rename files, updating .vcxproj(.filters) and CMakeLists; Code Cleanup and Actions on Save; analysis growth.

**Exit.**
- ≥250 refactoring goldens, each compiling with clang-cl and cl.exe; undo byte-exact; a rename preview over ≥1,000 references in ≤10 s.
- Project-edit goldens: MSBuild (through ProjectHost) and a CMake reconfigure accept every edited project. Template goldens pass.
- ≥45 frost and ≥4 CFG checks; coverage of ReSharper C++'s own inspections ≥60%; frost-featurecheck fails the exit on any shortfall.

**Depends on** M7 and M16 (ProjectHost for the project edits).

### M10: Debugger v2 and full test runner (9–11 units)
**Scope.** frost-dbgeng-dap (natvis, minidumps, symbol servers, hardware breakpoints) via the ComponentDownloader; memory, disassembly and register views; Catch2, doctest and Boost.Test; test gutter icons and the Unit Tests window.

**Exit.** Natvis goldens, the large-PDB benchmark and per-framework test goldens pass; a symbol-server test against a local symstore passes with network consent honoured; DbgEng redistribution conditions are verified, or download-only delivery is kept.

**Depends on** M6 and D12.

### M11: Rider UX parity, importers and accessibility (9–11 units)
**Scope.** Sticky lines, breadcrumbs, bookmarks, the Local History UI; Git log, blame, file history and Show History in the Explorer context menu (row 31), the three-way merge tool; multiple project windows and detachable editors; the remaining keymaps (Visual Studio, VS 2022, ReSharper, VS Code); colour schemes, fonts, theme import; settings importers and export/import; solution editing; full UIA and AT-SPI support and the row 246 accessibility extras.

**Exit.**
- Keymap conformance: every default shortcut in the inventory, for each keymap, triggers its action.
- Importer goldens per source (Rider, CLion, VS, VS Code); the multi-window Claude routing test.
- Automated accessibility suites, including colour-deficiency and high-contrast goldens and terminal text via UIA and AT-SPI.
- Manual NVDA, Narrator, Orca and IME passes added to the user-owned release checklist (VoiceOver only if macOS is added).

**Depends on** M9, M15, D17 and D18.

### M12: Claude C3 native chat (5–6 units)
**Scope.** The auth gate, chat panel, permissions UI, checkpoints, sessions and usage widget.

**Exit.**
- fake-claude/cli-mimic: a subscription-auth fixture (`authMethod` claude.ai, no approval recorded) is refused before any session starts; API-key and third-party fixtures proceed.
- Launch-variant goldens for (a) and (b) (§5.1, §5.4) pass. The zero-token real-CLI handshake for variant (b) (`--bare --ide`, `apiKeyHelper` fixture) shows `ide_connected` and a connected `frostide` server, and the hooks from `--settings` fire under fake-claude.
- fake-claude goldens for every control subtype; interrupt in ≤200 ms; resume and fork round-trip.

**Depends on** M8, D10 and D22.

### M13: Scale and performance hardening (5–6 units)
**Scope.** Preamble-cache R&D (adopt if the patch is ≤300 lines and heavy-TU warm readiness reaches ≤2 s); trigram index; FrostMmapIndex R&D (navigation ≤1.2 s); re-evaluating the backend split; ResourceGovernor tuning on REF-L1; growth to ≥60 frost and 5 CFG checks.

**Exit.**
- All budgets green on their own corpora, plus B3, B4 and B7 on R4-XL; B24 at its final value; B26 under REF-L1; the REF-L1 suite within 1.5x.
- ≥60 frost and 5 CFG checks; coverage of ReSharper C++'s own inspections ≥75%, gated by frost-featurecheck; every R&D item closed by an adopt-or-drop ADR.
- The Power Save / battery scenario (§4 REF-L1) passes; real iGPU and battery behaviour is reported as untested unless a laptop leg exists.

**Depends on** M7.

### M14: v1.0 release (4–5 units)
**Scope.** Authenticode-signed WiX MSI and GPG-signed AppImage (D25; for personal use, signing may stay self-signed until D22 changes); the LGPL compliance pack and privacy statement; counsel's legal review and final name clearance only if FrostIDE goes public (D22, D28, L8).

**Exit.**
- Clean-VM installs succeed (VirtualBox on REF-W1 at the floor build from §1.3); the AppImage runs in WSL and, if the optional containers exist, in the four distro containers.
- 14 consecutive green local full-verification runs (§4 Enforcement); feature matrix green; licence audit and OSV scan clean; no open Qt advisory affects a shipped module.
- `signtool verify` and `gpg --verify` pass on the release artifacts.

**Depends on** all milestones, D21, D25 and D28.

---

## 8. Risk and legal register

| # | Risk | Sev. | Mitigation |
|---|---|---|---|
| L1 | JetBrains trademarks and trade dress | High | Own name, icons, themes, splash; "IntelliJ-compatible" wording; no JetBrains logos, icons, names or theme files (JetBrains Mono under OFL); counsel review (D28). |
| L2 | Qt LGPLv3 compliance and patch timing | High | Dynamic linking, licence-keyed module allowlist, source offer and relink notes, no app stores; tracking the latest minor keeps open-source patches timely (D14). |
| L3 | vsdbg/cpptools licence | High | Never used; local verification scans artifacts for them. |
| L4 | Anthropic terms and branding | High | §5.3; the auth gate in code with an M12 test (D10, D22). |
| L5 | DbgEng redistribution | Med | Download on first use; if bundled: unmodified files, significant added functionality, no Microsoft marks. |
| L6 | GPL/LGPL tools; MSVC and SDK not redistributable | Med | GDB, cppcheck, clazy as user-installed processes; discover existing VS/SDK installs only; app-local VC runtime under Microsoft's terms. |
| L7 | Notices; Inter licence | Low | SBOM and generated notices; verify Inter in M1. |
| L8 | Name clearance ("FrostIDE", "Glacier") | Med | Trademark search before the repository goes public or any build is downloadable (D4, D28); counsel review before v1.0. |
| L9 | AI-reproduced GPL or proprietary code | Med | Clean-room rule and independent review (primary controls), source citations; ScanCode licence-notice detection on diffs (not snippet matching, §6.7). |
| T1 | Claude CLI protocol drift | High | Version gating, weekly goldens, handshake test, minimum-version notice. |
| T2 | Cold preamble vs sub-second | High | Availability levels (B23–B25), persisted index, PCH cache, S7, M13 R&D. |
| T3 | GetClCommandLines is an internal target | High | Version-gated adapters, v143 fixtures in local verification (v145 once VS 2026 Build Tools are installed), Rule-XML fallback, CLI path, `cl.exe /c` oracle. |
| T4 | Stale caches give wrong flags | High | Fingerprint classes, mutation tests, Invalidate Caches. |
| T5 | Clang false errors on MSVC code; C++26 gaps | High | Curated suppressions, clean-TU oracle, published residuals, `/analyze` SARIF, downgrading, the LLVM cadence, EDG review in 2027. |
| T6 | Scope and a ~3-year schedule | High | Appendix A, §7 levers, sign-off per milestone, non-goals; a first usable build (U1) around month 10–12 (D29, ADR 0029). |
| T7 | Local listeners; untrusted project code | High | §5.2 rules and enumeration test; §3.5 restricted mode and the malicious-repository test. |
| T8 | Agent continuity across sessions | High | CLAUDE.md, progress log, ADRs, Issues, the matrix, verification footers. |
| T9 | Data loss | High | SaveManager, journal, fault injection. |
| T10 | Raster misses 144 Hz | Med | S1 with validated probes; contained QRhi module; raster fallback. |
| T11 | Custom shell, IME, accessibility | Med | Built with automated tests: IME and the shell core in M1, the frameless window, docking, keyboard navigation and the UIA smoke in M15 (ADR 0029); manual checklists. |
| T12 | Single-machine verification: LLVM builds and full runs compete with the user's own work on REF-W1; WSL shares its cores and memory (no CI, D24) | Med | Official binaries where sufficient (S5); sccache; `-Quick` per change and `-Full` per merge; long runs scheduled with the user; the bootstrap and verify scripts check free disk and commit memory before heavy work. |
| T13 | LLVM API churn; library gaps; C++26 churn | Med | No patches, a local llvm-next leg, ~1 unit per major; compat layer and allowlist. |
| T14 | .NET dependency | Med | Out of process, not started on an unchanged warm reopen; S6 measures cold cost. |
| T15 | libvterm frozen; heap mixing across Qt /MD | Med | `IVtEngine` seam and patches; no global allocator override. |
| T16 | Defender I/O; mapped files blocking tools | Med | Dev Drive advice; 128 MB mmap threshold. |
| T17 | No macOS test leg | Med | macOS is unsupported in v1 (D24); platform seams stay so a leg can be added later, and nothing macOS-specific is claimed until then. |
| T18 | Windows 11-only floor | Med | D14 states the alternative and its cost. |
| T19 | Disk and memory on REF-W1 | Low | C: had ~214 GB free on 2026-10-31; the WSL distro (already on E:\WSL), caches and the install-smoke VM (E:\vm) live on E:; prechecks stop bootstrap below the floors. Host commit memory can run out because of other programs (seen during bootstrap), which crashes WSL; heavy steps check it first. |
| T20 | Unverified: aqt for new Qt minors, LLVM package contents, Ctrl+Esc, ConPTY queries | Low | Spikes S3 (aqt fails for Windows Qt 6.12.0; `tools/qt-fetch.py` replaces it), S5 (Windows package lacks lldb-dap), S8 and the M5 test. |
| T21 | Unreal Engine scale | Low | Post-1.0; NMake path in M2. |
| T22 | U1's debugger on MSVC-built programs: lldb-dap's PDB and MSVC STL support is below Visual Studio's | Med | Spike S10 first in M4 on R2a's Notepad++; U1 promises only lldb-dap's default formatters; the DbgEng adapter (natvis, full PDB fidelity) in M10; if S10 fails, the user decides before M4 continues (ADR 0029, decision 12). |

---

## 9. Decisions

### Decided before M0 (user, 2026-10-31)
| # | Decision | User's answer | Consequence |
|---|---|---|---|
| D1 | Installs and disk layout | **Approved.** | Toolchain under E:\sdk (`tools/bootstrap.ps1`) and `~/sdk` in WSL; caches on E:. The WSL distro already lives on E:\WSL\Ubuntu-24.04, so no move is needed. C: had ~214 GB free; the precheck still stops bootstrap below 15 GB. VS 2026 Build Tools need elevation and are installed by the user when v145 coverage is wanted. Mirrored networking is written at M5; VirtualBox on E:\vm at M6. |
| D2 | Compiler for our own code | **Approved: Clang only.** LLVM 23.1.4 installed. | cl.exe stays a target for user projects and an oracle. |
| D3 | C# helpers: ProjectHost (~3k lines), MSBuildLogger | **Accepted** ("use C# if necessary"). | C# only where MSBuild or SolutionPersistence require it; S6 still measures ProjectHost cold start. |
| D4 | Repository | **Private GitHub repository https://github.com/Frowsty/FrostIDE.** | Pushes authenticate through Git Credential Manager; `gh` is installed under E:\sdk (PR creation needs `gh auth login` by the user). No CI of any kind (D24), so no runner or minutes cost. Making it public later requires the D28 name search first. |
| D5 | §1.1 redefinitions, scope and schedule | **Accepted.** | Sub-second = L1 warm; analysis below ReSharper parity; WSL, Docker and remote toolchains in 1.x; the row 118 and row 20 slices; P0 rows 31, 99, 118, 227 and 242 finishing after Preview 0.1; ~2.7–3.3 years sequential. |
| D7 | FrostIDE licence | **Apache-2.0.** | SPDX `Apache-2.0` headers from the first commit. |
| D22 | Distribution intent | **Personal use first; possibly public and free later.** | No public builds, signing spend or Anthropic approval request for now; D8, D25, D28 and D10 are revisited if it goes public. Qt LGPL obligations are honoured from the start so going public needs no rework. |
| D23 | Commit permission | **Standing permission granted.** | Recorded in CLAUDE.md; feature branch and PR per slice, merged after green `-Full` verification and review (§6.7). |

### Decided before M1 (user, 2026-10-31)
| # | Decision | User's answer | Consequence |
|---|---|---|---|
| D14 | Platform floors | **Windows 11 only.** | Tracks the latest Qt minor; the Windows floor is the lowest build Qt's current minor and the VM leg support (22631 today, §1.3). Linux glibc ≥2.39. No macOS in v1 (D24). |
| D20 | Upgrade cadence | **Keep updating (track, §2.3).** | ~9–12 units over the roadmap (9–11 before ADR 0029 lengthened it). |
| D24 | CI/CD and Mac access | **No CI/CD pipelines of any kind** (no GitHub Actions, hosted runners, runner services or cloud Mac). Build and verify only on REF-W1 and in WSL on it. | §6.3 is a local verification matrix; milestone exits require `verify.ps1 -Full` and `verify.sh -Full`; macOS is unsupported until a test leg exists; hosted legs (windows-2025, ubuntu-26.04, macos-26) are gone; GCC 16 and libstdc++ 15 legs are optional local containers. |

### Decided during M1 (user, 2026-11-01)
| # | Decision | User's answer | Consequence |
|---|---|---|---|
| D29 | A thin "usable for real C++ work sooner" path (basic editor, CMake, clangd, build and run) ahead of the full M1 polish | **"Yes I want something usable before a fully polished version"** | ADR 0029: the first usable build U1 is the M4 exit (§7, "First usable build"); M1–M4 shrink to their thin cores; their deferred halves move to M15, M16 and M17, before M6; no item or exit criterion is dropped and no target changes; B15's gate moves with large-file mode to M15, B16's with the minimal debugger to M4, and B23's first project is gated at M2 on path (c) and at M16 on ProjectHost; 129–156 milestone units instead of 120–145. The sub-decisions beyond the answer were ADR 0029's questions Q1–Q5, answered the same evening: **"I agree with everything except 4 and 5. I don't care for those savings"**. So path (c) is U1's evaluator (Q1), B23's first project is gated at M2 on path (c) (Q2) and M5 runs before M15–M17 (Q3), as recommended; U1 has a minimal debugger, moved from M6 to M4 (Q4, the recommendation to keep it in M6 reversed); and U1 is not thinned (Q5, the option declined). ADR 0029 is accepted. |

### Later
- **D6 C# scope (M16, ADR 0029):** solution citizenship in v1, frost-dotnet in 1.1.
- **D8 Distribution hosting (M5):** packages stay local (E:\FrostBuilds) or in the private repository while FrostIDE is for personal use (D22); public hosting only after the D28 name search.
- **D9 Real-CLI E2E (M5):** API key, $0.50 cap per run, run locally in `verify -Nightly`.
- **D10 Subscription auth in native chat (M12):** gate on; ask Anthropic only if FrostIDE goes public (D22).
- **D11 Mutating MCP tools (M8):** per-call approval, brave mode off.
- **D12 DbgEng (M10):** download on first use; bundle after legal review.
- **D13 Icons (M1):** own set, Lucide as stopgap. **D15 GPL/LGPL analyzers (M7):** optional, user-installed. **D16 Approximate .vcxproj mode off Windows (M2):** on, labelled. **D17 Solution editing:** conversion in M16, editing in M11. **D18 Accessibility bar (M11):** automated gate plus user-owned manual checklists. **D19 Dev Drive (M16, with the first-run wizard):** optional recommendation. **D21 Unreal:** post-1.0.
- **D25 Signing identities and costs (decide before M6 for lead time; used in M14):** Azure Trusted Signing or an OV certificate; a GPG key for Linux; for personal use (D22) self-signed may suffice. No Apple Developer Program while macOS is unsupported.
- **D26 Crash-report upload (M5):** none (local store plus Collect Logs); alternatives are self-hosted Sentry or a GitHub issue template.
- **D27 ProjectHost runtime (M16, with ProjectHost):** net472 in-box; net10 framework-dependent (needs the .NET 10 runtime) or self-contained (larger install).
- **D28 Legal counsel and name clearance:** the trademark search for "FrostIDE" and "Glacier" comes before the repository goes public or any build is downloadable (D4); with the private repository and personal use (D22) it is not needed yet. Counsel's review of the JetBrains look-alike, LGPL and DbgEng questions comes before any public release.

**Next step (M0 complete; M1 under way).** M0's exit is met (§7 M0): every M0 branch is merged on `m0/foundation` under D23, verified only locally (D24), S1–S7 and S9 are reported as ADRs with numbers (ADRs 0001 and 0005–0008), B28 is met (ADR 0010), the M0 decisions are recorded in ADR 0011, the launch from a path over 260 characters is met (ADR 0012), and `verify -Full` is green on both platforms. The sign-off follows ADR 0011's G1: the final commit is tagged `m0` and `main` is created from it, both pushed to the private repository; the demo pack is in `E:\FrostBuilds\demo\m0\`. Next is M1, already under way on `m1/*` branches, on the user's instruction of 2026-11-01 to go on with M1 and the milestones after it (`docs/progress.md`). Since D29 (2026-11-01, evening) M1 is its thin core (ADR 0029), and the milestones run in §7's order towards the first usable build U1 at the M4 exit: W1 first (ADR 0032), in the feature-first phase (ADR 0031).

---

## Appendix A: Feature matrix → milestone

Every P0 and P1 row of `tests/features/matrix.json` is listed under the milestone that delivers it. Row numbers are the 1-based lines of `features.features.tsv`; rows 250–257 (marked *) are new. "(+Mx)" marks the milestone where the last sub-part lands and the row's acceptance test turns fully green. "(P2 in)" marks a P2 row pulled into v1.0. Milestones are listed in the order they run (§7): M15–M17 hold the deferred halves of M1–M4 and run between M5 and M6 (ADR 0029).

| Milestone | Tier | Rows |
|---|---|---|
| M1 | P0 (22) | 28 File System view; 29 Explorer speed search; 35 text buffer, large files, undo (+M15); 44 smart keys; 45 line operations; 55 editor tabs, splits (+M15); 57 gutter (+M6); 60 encodings, BOM, EOL; 78 Find Action; 85 Find/Replace in file; 91 Recent Files/Locations; 92 Back/Forward; 94 Go to Line, next error/member (+M3); 219 Notifications, background tasks; 227 keymaps (+M11); 239 main window, toolbar (+M15); 240 tool-window docking (+M15); 241 popup framework; 242 status bar widgets (+M7); 243 themes; 244 compact mode, IDE zoom, HiDPI; 250 soft wrap* |
| M1 | P1 (1) | 218 Local History (+M11) |
| M2 | P0 (15) | 1 .sln reader; 2 .slnx reader; 4 .vcxproj model (+M16); 5 .filters folders; 8 CMake File API; 9 CMake profiles; 11 CMake auto-reload; 15 compile_commands; 20 toolchain detection, v1 slice (WSL/Docker/remote: 1.x); 25 trusted project, restricted mode; 26 Welcome, first run, New Project (+M16); 27 Solution view (+M16); 31 Explorer context actions (+M11); 86 Find/Replace in Files; 154 configuration switcher |
| M2 | P1 (2) | 10 CMake presets; 12 CMake tool window (+M6) |
| M3 | P0 (23) | 22 persistent index, Invalidate Caches; 23 system headers, exclude/mark dirs; 36 semantic highlighting; 37 inactive regions; 50 Parameter Info; 51 Quick Documentation; 58 error stripe, inspection widget; 68 completion; 69 #include completion, auto-import; 76 Search Everywhere (+M5); 77 Go to Class/File/Symbol; 80 Go to Declaration; 81 Go to Base/Implementations; 82 Switch Header/Source; 84 Find Usages; 98 on-the-fly diagnostics; 100 Alt+Enter actions; 104 clang-tidy integration; 114 Problems window; 118 C++23/26 language, v1 slice (+M7); 119 MSVC dialect; 122 ClangFormat; 131 Rename |
| M3 | P1 (1) | 120 PCH, forced includes |
| M4 | P0 (14) | 150 Build/Rebuild/Clean; 151 MSBuild orchestration; 152 CMake build; 153 Build window; 159 run/debug configuration model (+M6); 160 Run widget (+M6); 161 run options; 163 Run console; 167 debugger engine (+M6); 169 line breakpoints (+M6); 173 stepping (+M6); 175 Debug window; 178 Evaluate, watches; 217 terminal |
| M5 | P0 (5) | 203 git clone/fetch/pull/push; 204 Commit window (+M6); 205 diff viewer; 208 Branches popup; 232 Claude IDE bridge |
| M5 | P1 (3) | 32 VCS/error colours in tree (+M7); 96 compare files/folders; 210 stash, shelve |
| M15 | P0 (6) | 38 multi-caret, column mode; 40 folding; 87 File Structure, Structure window; 226 Settings dialog; 251 whitespace, indent guides*; 253 editor font zoom* |
| M15 | P1 (8) | 39 extend selection; 59 EditorConfig; 115 TODO window; 246 accessibility (+M11); 252 rulers*; 254 clipboard history*; 255 drag and drop*; 256 fold persistence* |
| M16 | P1 (6) | 3 .sln/.slnx conversion (P2 in); 13 CMake cache viewer (P2 in); 30 Show All Files, nesting; 66 CMakeLists language service; 162 before-launch tasks; 164 gutter run icons (+M6) |
| M17 | P0 (1) | 99 inspection framework (+M7) |
| M17 | P1 (12) | 21 resolve context; 42 inlay hints; 52 Quick Definition; 53 macro expansion; 70 smart completion; 83 Navigate To; 88 type hierarchy; 89 call hierarchy; 95 Select In, nav bar; 101 intention preview; 106 unused/missing includes (+M7); 125 optimize includes (+M7) |
| M6 | P1 (9) | 170 function/exception breakpoints; 172 Breakpoints dialog; 176 data renderers, v1 slice (+M10); 177 inline values; 179 Set Value; 184 debugger console; 186 attach; 193 CMake debugger (P2 in); 196 test runner (+M10) |
| M7 | P1 (8) | 90 include hierarchy; 102 inspection catalogue, v1 slice (+M13); 103 data-flow checks, v1 slice (+M13); 107 naming conventions; 109 class-design checks; 111 solution-wide analysis; 112 Inspect Code, `frost inspect` CLI; 124 header-guard style |
| M8 | P1 (2) | 233 frostide MCP server, v1 slice; 234 quality-gate hooks (P2 in) |
| M9 | P1 (19) | 6 add/remove/rename files in project; 46 live templates; 47 Surround With; 49 file templates; 54 Doxygen stub; 126 Actions on Save; 127 Code Cleanup; 130 Refactor This; 132 rename file + includes; 133 Change Signature; 134 Extract Function; 135 Introduce/Inline Variable; 140 Safe Delete; 141 Move (P2 in); 144 refactoring preview, atomic undo; 145 Generate; 146 Create from Usage; 147 Implement/Override; 148 generate definitions |
| M10 | P1 (5) | 171 hardware watchpoints; 180 Memory view; 181 Disassembly; 189 symbol servers; 197 test gutter icons, Unit Tests window |
| M11 | P1 (8) | 41 sticky lines; 56 breadcrumbs; 93 bookmarks; 206 three-way merge tool; 207 Git log; 209 blame, history; 229 colour schemes, theme import; 245 multiple windows |
| M12 | P1 (1) | 257 native Claude chat panel (C3)* |

**Totals.** P0: 86 (M1 22 + M2 15 + M3 23 + M4 14 + M5 5 + M15 6 + M17 1). P1: 85. M0, M13 and M14 own no feature rows; they own infrastructure rows and the budget, analysis-count and release gates. M4 owns no P1 row, and M16 and M6 no P0 row; M16 completes the P0 rows 4, 26 and 27, and M6 the P0 rows 167, 169 and 173 of U1's minimal debugger (ADR 0029, decision 12).

**P0 rows deferred past Preview 0.1:** 31 (Show History, M11), 99 (profiles and scopes, M7), 118 (modules flag and reflection downgrading, M7), 227 (VS, VS 2022, ReSharper and VS Code keymaps, M11), 242 (analysis monitor, M7). Their parts in M0–M6 and M15–M17 are green at the M6 exit (§1.4).

**Infrastructure rows** (tracked in `matrix.json` with an I prefix; P0 unless marked P1):
- **M0:** I7 Windows long paths, UTF-8 code-page manifest.
- **M1:** I1 settings registry, scopes, `.frost/`, migration; I2 SaveManager, autosave, hot-exit journal, external-change merge; I3 crash and freeze reporting (out-of-process handler); I4 privacy and network policy, no-network-before-L1 test; I5 i18n-ready strings, pseudo-localization; I6 AltGr and layout-aware shortcuts, grapheme navigation.
- **M2:** I8 trust model, restricted mode, absolute tool paths; I9 listener hardening and enumeration test (each later listener joins in its milestone); I10 single instance, `frost` CLI, file associations (+M16).
- **M3:** I11 ResourceGovernor, process-tree memory budget, cache disk budget.
- **M4:** I12 localized MSBuild/cl output decoding; I13 terminal IME.
- **M5:** I14 `IHttpClient` (+M6 for Linux); I15 portable packages, symbol archival, Collect Logs.
- **M6:** I16 unsigned installers (MSI, AppImage); I17 updater and ComponentDownloader.
- **P1:** I18 Power Save Mode (M7); I19 settings importers, export/import (M11); I20 signed installers (M14).

## Appendix B: Expanded product acceptance tracks (2026-11-02)

ADR 0054 records the user's expanded scope. These additional tracks are required product work, not
optional suggestions or completed features. Appendix A remains the original C/C++ milestone inventory;
its generated `tests/features/matrix.json` does not yet represent these additions. The table below is an
initial acceptance breakdown, not a claim that every Rider feature has been inventoried. The parity
inventory must cover the public Rider 2026.2 feature set and record each unsupported workflow explicitly.
Existing platform, data-safety and trust rules apply throughout.

| Track | Required behavior and acceptance evidence | Dependency / initial status |
|---|---|---|
| PARITY-01 | Inventory Rider's documented features, menus, settings, keyboard and mouse interactions; map each to a FrostIDE scenario and evidence, including managed/web/framework workflows absent from Appendix A. Record reference version and later deltas. | Required; inventory incomplete |
| PARITY-02 | Reproduce the intended layout, navigation, focus behavior, editor interactions and tool-window workflows with original artwork, theme values and branding. Exercise common tasks through real input on supported Windows/Linux displays and keyboard layouts. | Existing shell is partial; application and GPU integration under way |
| MANAGED-01 | Load and reevaluate managed projects; accurate C# completion, diagnostics, navigation, refactoring and build context, including Unity-generated projects and assembly definitions. No regex substitutes for semantic analysis. | Reviewed csharp-ls/Roslyn service and installer integrated; real C# editor semantics and corrected mixed-language shutdown pass combined Windows/Linux gates; project model/build/Unity context incomplete |
| MANAGED-02 | Debug, test and profile managed applications and supported framework projects, with explicit capability coverage against the parity inventory. | Depends on MANAGED-01 and debugger infrastructure; planned |
| LUA-01 | Lua service lifecycle, document synchronization, completion, signatures, hover, diagnostics, symbols, references and rename; select the intended runtime and honor workspace configuration. Test edits, Unicode positions, server failure and restart. | Backend and application routing reviewed; real LuaLS editor workflow tested on Windows/Linux; remaining capability coverage tracked separately |
| LUA-02 | Lua syntax, indentation, formatting, snippets, annotations/generics and module navigation; preserve unsaved edits and support mixed C++/Lua workspaces without sending Lua documents to clangd. | Reviewed syntax, effective-settings formatting, snippets/parameter hints, generics and module navigation pass real LuaLS and combined Windows/Linux checks. Broader snippet transforms and complete semantic coverage remain |
| LUA-03 | Lua debugging: breakpoints, stepping, stacks, locals, watches and evaluation in supported standalone and FrostEngine runtimes. A configured adapter alone is not acceptance. | Runtime/debug-protocol choice and engine support outstanding |
| LUA-04 | Verify real LuaLS behavior on the target engine's generated definitions and a synthetic fixture: correct completions and signatures, no false undefined-engine-global diagnostics, genuine errors still reported. Measure interactive latency on the reference machine. | Real engine API completion/hover/navigation/rename and diagnostics tested on Windows/Linux; full coverage and latency measurements remain |
| UNITY-01 | Recognize Unity projects and packages; open scripts at the requested location through a Unity external-editor bridge; refresh generated projects while preserving asset identities. | Managed-language foundation integrated; original bridge under development in an isolated worktree, not accepted or integrated |
| UNITY-02 | Connect the correct editor instance, control play/pause/step, navigate logs and run/debug Unity tests; handle disconnect and domain reload without stale state. | Depends on UNITY-01; planned |
| UNITY-03 | Debug editor and player scripts with appropriate value inspection; identify runtime/backend support and execute real sessions before claiming it. | Depends on managed debugger and Unity transport; planned |
| UNITY-04 | Navigate serialized scene/prefab references, understand Unity callbacks and assembly definitions, supply Unity-aware analysis and shader workflows. Track profiling and remaining documented Unity capabilities in PARITY-01. | Depends on project model, C# semantics and Unity context; planned |
| FROST-01 | Open `.frostproj` roots and scripts at 1-based locations through the engine's custom-editor argument template. Read the engine-owned `.luarc.json` and `.frost/lua-api/frost.lua` without rewriting them. | Target `D:\Dev\FrostEngine-AI-Claude`; project identity, configuration, CLI and native window reuse reviewed/tested on Windows/Linux, including native symlink regressions; actual engine opened/reused the staged IDE tab at the requested location |
| FROST-02 | Discover the matching project through the engine's versioned per-project discovery files, authenticate locally, verify project identity, and expose play, validation, tests and navigable log output without blocking the UI. | Reviewed v1/v1.1 helper/panel integrated; combined-main real-engine validation, play/state, test failures and log navigation pass, along with full Windows/Linux functional gates |
| FROST-03 | Handle unavailable/stale engines, project switching, paths with spaces/Unicode, cancellation, token privacy and trust refusal; verify against the real target engine and an isolated test project. Preserve users' projects. | Focused helper lifetime, trust revocation, project switching and alias tests pass; real target tested with isolated scratch projects/configuration. Protocol cannot atomically bind an action to the preceding project-identity check; see ADR 0063 |
| DELIVERY-01 | Complete and execute W1/U1 integrated scenarios; then execute the new Lua, Unity and FrostEngine scenarios. Packages include their required engines, plugins and notices. Separate component, application, platform and release evidence. | W1 integration under way; U1 and expansion scenarios outstanding |

Implementation agents use GPT-6.1 Terra; GPT-6 Astra reviews the changes and verification evidence before
integration. The inherited feature-first policy still postpones expensive hardening, but not missing
features, ordinary functional tests or review requested by the user. Scope is not reduced to whichever
parts happen to pass tests first. Current work and remaining gaps are recorded in `docs/progress.md`.

### B.1 Public-reference coverage ledger (2026-11-02)

This family-level inventory makes omissions visible; it does not complete PARITY-01's individual action,
setting and interaction inventory. The versioned documentation reference is Rider 2026.2. Only public
behavioral documentation was consulted, not IDE implementation sources or proprietary artwork. Status
below describes FrostIDE's current integrated application, not the existence of a standalone component.

The general families follow the public [Rider feature overview](https://www.jetbrains.com/rider/features/).
Acceptance scenarios are FrostIDE's own implementation targets.

| Family | Acceptance scenario | Current gap / owning track |
|---|---|---|
| Managed languages | Open C#, F# and VB projects; edit with semantic completion and diagnostics. | C# service/editor workflow and mixed-language shutdown reviewed and pass combined gates; F#/VB services absent. MANAGED-01. |
| Frameworks | Build and launch representative desktop, service and web projects with correct references. | Native build support does not establish managed/framework support. MANAGED-01/02. |
| Analysis | Detect project-wide errors and apply semantic fixes with preview and undo. | Language-specific coverage incomplete. MANAGED-01, M17. |
| Navigation | Find symbols/usages and navigate inheritance across project references. | Shared LSP UI and real C# definition navigation tested; complete managed navigation/inheritance coverage absent. MANAGED-01. |
| Refactoring | Rename across files, extract methods and move types without breaking references. | Lua rename and real C# two-project unsaved UTF-16 rename tested; extraction/move and broader coverage remain. MANAGED-01, LUA-01. |
| Library sources | Navigate dependency symbols to source or readable decompiled code. | Managed dependency navigation/decompilation absent. MANAGED-01. |
| Tests | Discover, run, debug and navigate NUnit, xUnit and MSTest results. | Integrated managed runner absent. MANAGED-02. |
| Debugging | Launch/attach, inspect threads and values, evaluate expressions and manage breakpoints. | Native application launch/step/inspection acceptance and combined gates pass; attach and broader coverage remain, managed runtimes absent. M4/M6, MANAGED-02. |
| Version control | Review changes, resolve conflicts, commit and navigate history through the application. | Existing Git components need current-workbench acceptance. M5. |
| Web | Edit, debug and test JavaScript/TypeScript and framework projects. | Dedicated semantic/runtime integration absent. PARITY-01. |
| Databases | Connect, inspect schemas, edit data and execute SQL with diagnostics. | Database application workflow absent. PARITY-01. |
| Extensions | Discover, configure and manage independently implemented extensions. | Extension API, compatibility scope and delivery remain unimplemented. PARITY-01. |

Unity behavior is grounded in the versioned [Unity feature documentation](https://www.jetbrains.com/help/rider/Features_Unity.html).
Every row below remains unimplemented in the integrated application; installing Unity locally is not evidence
of IDE integration. The semantic service audit precedes an original Unity bridge.

| Family | Acceptance scenario | Track |
|---|---|---|
| Project view | Show assets and resolved packages alongside generated script projects. | UNITY-01 |
| Editor control | Connect the matching editor and operate play, pause and step. | UNITY-02 |
| Console | Stream filtered logs and navigate their source locations. | UNITY-02 |
| Serialized references | Find scene/prefab usages and navigate back to matching editor objects. | UNITY-04 |
| Engine semantics | Recognize callbacks, serialized members and performance-sensitive code. | UNITY-04 |
| Assembly definitions | Validate and navigate assembly references; safely rename definitions. | UNITY-01/04 |
| Shaders | Analyze shader sources and exercise supported frame inspection/debugging workflows. | UNITY-04 |
| Authoring helpers | Provide original callback, script, test and shader templates. | UNITY-04 |
| Merge workflow | Invoke UnityYAMLMerge and report unresolved conflicts. | UNITY-04, M5 |

Remote execution is a distinct product workflow in the [remote development documentation](https://www.jetbrains.com/help/rider/Remote_development_overview.html).
FrostIDE's WSL verification mirror only tests the Linux build; it is not remote development support.

| Family | Acceptance scenario | Current gap |
|---|---|---|
| Remote backend | Connect securely to a remote workspace; edit, analyze, build, run and debug there. | Backend/client protocol and reconnection absent. |
| Containers | Open a development container with its own tools and project environment. | Container lifecycle and toolchain integration absent. |
| WSL workspace | Operate a native Linux project from the Windows application. | Product-side WSL workspace integration absent. |

The next inventory pass must expand individual menus, settings, keymaps, editor interactions, accessibility,
profiling, deployment, remaining frameworks and other game-engine workflows. These are outstanding scope,
not excluded features or deferred verification. No percentage of Rider parity is claimed from test counts.

### B.2 Editor and navigation acceptance inventory (2026-11-02)

These scenarios expand PARITY-01/02; they do not replace the wider product tracks. The behavioral references
are Rider 2026.2's [search](https://www.jetbrains.com/help/rider/Searching_Everywhere.html),
[recent navigation](https://www.jetbrains.com/help/rider/Navigation_and_Search__Navigating_to_Recent_Locations.html),
[editing](https://www.jetbrains.com/help/rider/Creating_and_Editing_Code.html),
[multiple carets](https://www.jetbrains.com/help/rider/Multicursor.html),
[folding](https://www.jetbrains.com/help/rider/Code_Folding.html),
[tool windows](https://www.jetbrains.com/help/rider/Tool_Windows.html) and
[accessibility](https://www.jetbrains.com/help/rider/Accessibility.html) documentation.
Only public behavior is used; FrostIDE keeps original implementation, assets and branding.

The source audit began at checkpoint `12fb20f`; later evidence is identified in the rows. A component or registered action alone is not
proof of the full scenario. Add real Windows/Linux input and application evidence before closing a row.

| ID | Required application scenario | Evidence and remaining gap |
|---|---|---|
| NAV-01 | One search surface finds files, types, symbols, text, settings/actions, run configurations and Git objects, with category filtering and useful empty-query history. | `ui/workbench` currently combines up to eight files and eight actions. Other categories, filtering and cross-language aggregation remain. |
| NAV-02 | Search types and symbols across the appropriate projects, including referenced code; cancel obsolete searches and navigate exact locations without overwriting dirty tabs. | `LanguageController` has a separate server-backed symbol popup. Type-specific filtering, complete backend coverage and mixed-language aggregation need application acceptance. |
| NAV-03 | Persist navigation and edit history per project; filter viewed versus edited locations, inspect useful context and remove entries consistently. | Recent files, session and back/forward models exist. Full changed-location/context/removal behavior is not established by those models. |
| EDIT-01 | Multiple carets and rectangular selection support typing, clipboard operations, completion and snippets, including virtual columns and predictable primary-caret behavior. | Editor selection models and Alt+Shift click exist. The full input/clipboard/snippet matrix and column-mode UI remain unverified. |
| EDIT-02 | Fold/unfold nested code and selections through gutter and keyboard; persist state, preview hidden code and keep navigation/editing correct across collapsed regions. | Lua syntax fold ranges exist, but the text display-fold layer is an identity placeholder. Interactive mapping, C++ ranges and editor integration are under development in an isolated worktree; no folding workflow is accepted yet. |
| EDIT-03 | Show inlay hints without corrupting caret movement, selection, wrapping, hit testing, diagnostics or either renderer. | Existing text/display infrastructure does not prove an interactive inlay layer; M15/M17 still own implementation and acceptance. |
| EDIT-04 | Reformat documents/selections using resolved file settings; accept snippets and parameter hints without losing edits or applying stale replies. | Advanced Lua formatting/settings, real LuaLS responses, parameter hints, Unicode positions and engine-API editing pass independent review and combined Windows/Linux gates. Broader snippet transforms and other language coverage remain. |
| EDIT-05 | Edit and navigate within split/detached editors while preserving document identity, undo, focus and recovery across project windows. | Tab reordering/pinning and project-specific process routing exist. They do not establish split/detached-editor behavior. |
| SHELL-01 | Dock, float, resize, hide, restore and keyboard-focus tool windows, preserving layout and returning focus to the correct editor. | Tool-window manager and application contents exist. A complete interaction/layout acceptance matrix remains, including both native displays. |
| SHELL-02 | Navigate and act through a persistent structure view, breadcrumbs, bookmarks and sticky context without stale locations after edits. | File-structure popup exists; it is not evidence for the remaining persistent navigation surfaces. M15/M11 own the gaps. |
| INPUT-01 | Exercise supported keymaps/layouts, IME composition and keyboard-only workflows without shortcut collisions or text corruption. | IntelliJ bindings and editor input tests exist. Additional keymaps and the complete real-layout/IME matrix remain. |
| ACCESS-01 | Screen readers expose editable text, selections, diagnostics and terminal content; keyboard navigation and high-contrast modes work through actual Windows/Linux accessibility interfaces. | Themes and ordinary Qt widgets alone do not establish custom-editor/terminal accessibility. Full application evidence and explicit accessible-text integration remain. |

The existing `tests/features/matrix.json` still tracks the original Appendix A, and many acceptance fields
are unfilled. Its successful consistency check is not a completion check for these scenarios or for the
expanded product scope. Coverage must be reconciled with actual behavior, rather than inferred from total tests.
