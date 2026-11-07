<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0079: Bundle LuaLS 3.19.1 as the Lua language server

- Status: accepted (user decision D37, PLAN.md); implemented by LUA-1 (R0).
- Date: 2026-11-07
- Plan reference: D37, LUA-1 (R0), LUA-7 (R4, formatter).

## Context

FrostIDE's Lua support (`frost::lang::LuaService`) speaks LSP to LuaLS
(lua-language-server). Until now FrostIDE only looked for a LuaLS on PATH, in
`<exe dir>` or in a configured setting, and the tests skipped when there was
none. A Lua feature that works only on a developer's machine breaks the truth
rule (ADR 0078): the editor showed no Lua diagnostics and gave no reason why.
The user decided (D37) to bundle LuaLS 3.19.1 (MIT) and to use its built-in
EmmyLuaCodeStyle formatter (MIT). StyLua (MPL-2.0) stays optional,
configured by the user and not bundled.

## Decision

Pin the upstream release archives of LuaLS 3.19.1 in `sdk/sdk.lock.json`:

| Platform    | Asset                                       | SHA-256 |
|-------------|---------------------------------------------|---------|
| Windows x64 | `lua-language-server-3.19.1-win32-x64.zip`   | `fdb9a59108cf62517813c97fa5549b0e16d1ef0688306bac728b08434db7e4cd` |
| Linux x64   | `lua-language-server-3.19.1-linux-x64.tar.gz` | `e9235d2d72ef55bc41cf8c99cda2ed64777682024b4bb81f5dea425060c5cbb8` |

Both are the release assets of tag `3.19.1` (tag object
`817f32d437985115ad56d0b896d100d9500276e7`, published 2026-08-13), and each
digest equals the one GitHub publishes for the asset.

- **Install.** `tools/bootstrap.ps1` (Windows, `-Only luals`) and
  `tools/bootstrap-wsl.sh` (Linux, `--only luals`) download the archive,
  verify the pinned SHA-256 and extract it to `<sdk>/luals-3.19.1`.
- **Locate.** `lang::defaultLuaSearch` looks in this order: the setting
  `tools.luaLanguageServerPath`; `<exe dir>/luals-3.19.1/bin`, the staged
  copy; then `<exe dir>/lua-language-server/bin` and `<exe dir>`;
  `$FROST_SDK/luals-3.19.1/bin`; and finally PATH. There is no built-in
  SDK root (FND-10, R0-close): the product never guesses a developer's
  drive, and development and test runs name the SDK with `FROST_SDK`
  (`tools/env.ps1`, `~/sdk/env.sh`). The version comes from the lock file
  at configure time (`FROST_PINNED_LUALS_VERSION`).
- **Launch.** LuaLS is started only through `IProcessLauncher`, from its
  absolute path, and only for a trusted workspace. By default it writes its
  log and its generated meta definitions into its own installation
  directory. FrostIDE therefore passes `--logpath=<dir>/log` and
  `--metapath=<dir>/meta`, with `<dir>` under the user profile (next to the
  trust store, `.../luals`). This keeps the staged folder read-only while
  FrostIDE runs. A `.frostproj` in the workspace root selects the
  FrostEngine defaults (`luaDefaults(runtime, true)`).
- **Missing server.** If no LuaLS is found, FrostIDE shows a visible warning
  that names the reason and LUA-1. It appears once per project: on open for
  a FrostEngine project or a configured path, otherwise when the first Lua
  file opens. No Lua process runs and no Lua result appears.
- **Stage.** `tools/stage.ps1` copies `<sdk>/luals-3.19.1` to
  `<stage>/luals-3.19.1`, leaving out the install marker and any `log`. It
  also copies the LICENSE to `licenses/luals-LICENSE.txt`. The stage fails if
  LuaLS is missing, as it does for every other bundled tool (FND-13).
  `tools/stage.sh` (R0-close) does the same for a Linux build: frost, the
  crash handler, the resources, the licences, clangd, lldb-dap and
  `<stage>/luals-3.19.1` in the layout the locator expects. A Linux stage
  still loads Qt from the pinned SDK through frost's RUNPATH; deploying Qt
  into it belongs to Linux packaging.
- **Tests.** The LuaLS tests (`tests/lang/lua_e2e_test.cpp`,
  `tests/app/workbench_lua_test.cpp`) require the pinned server. They fail
  rather than skip when it is missing.
- **Exit test (R0-close).** `tests/tools/test_staged_luals.py` stages the
  Debug build of the ctest tree on each leg and runs the staged frost
  offscreen on a fresh profile with `FROST_SDK` removed. It checks that the
  SDK's archive matches the pinned SHA-256, that the staged executable is
  that archive's `bin/lua-language-server`, that frost started exactly that
  image through `IProcessLauncher` (the spawn log of
  `frost/io/spawn_log.hpp`, in Debug builds with `FROST_ENABLE_SPAWN_LOG`,
  records each launch with its SHA-256), and that frost shows LuaLS's
  `undefined-global` diagnostic.

## Update (R0-close): staging never leaves a partial folder

Both stage scripts run every check on their inputs before they create the
destination: the build tree (on Windows also the ProjectHost, the HTTPS
backend and the libsodium notice), the resources, the bundled tools, and on
Windows the PDB identities of frost.exe and frost_app.dll
(`tools/symbolize.py archive --check` on the build tree). A failure after
that point removes what the run created. That covers the checks of the
staged folder (a failed windeployqt, a staged file that differs from its
source, a failed symbol archive) and unexpected errors: a failed copy, a
locked or unreadable file, a full disk. `stage.ps1` routes every terminating
error through a script-scope `trap`, and `stage.sh` uses an `ERR` trap. A
destination folder that already existed empty is emptied again, not
removed. Paths are taken literally, so a destination whose name holds `[`
or `]` is cleaned up like any other. The Windows and Linux script tests
assert that no partial folder is left, including after a copy fails on an
input held open without sharing.

## Licence

LuaLS is MIT: `Copyright (c) 2018 最萌小汐`. MIT is on the
`tools/licensecheck.py` allowlist. `check_sdk_luals` checks both pins (an
allowed licence, a SHA-256, this ADR, and the copyright and MIT permission
text in NOTICE) and lists them in the SBOM. The archive also contains:

- The LuaCATS definition libraries under `meta/3rd`, each with its own
  licence file (for example `meta/3rd/bee/LICENSE`, MIT). They are staged
  unmodified with the rest of the folder.
- On Windows, the Microsoft Visual C++ runtime DLLs in `bin` (vcruntime140,
  msvcp140, concrt140). These are distributed as in the upstream archive,
  under Microsoft's redistributable terms for the Visual C++ runtime.

The formatter is LuaLS's built-in EmmyLuaCodeStyle (MIT, part of the same
binary). No GPL or proprietary code is involved.

## Consequences

- Every developer machine and staged folder has the same LuaLS, and Lua
  diagnostics no longer depend on what happens to be on PATH.
- The bootstrap needs about 15 MB more on each platform.
- Updating LuaLS means changing the pins and hashes here and in the lock
  file; tests show the version through `--version`.
