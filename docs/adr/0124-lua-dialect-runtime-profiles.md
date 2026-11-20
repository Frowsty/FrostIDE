<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0124: Explicit LuaJIT and Luau profiles

- Status: source checkpoint; independent source review and runtime acceptance pending
- Date: 2026-11-20
- Plan: R19 LUA-16; builds on ADRs 0094 and 0117

## Decision and product boundary

An explicitly selected script/REPL run configuration now records `luaRuntime` as `lua54`, `luajit`
or `luau`, separately from its absolute interpreter executable and separately from analysis settings.
The normal Run/Debug Configurations dialog exposes that selector. Non-default profiles use store
version 6; old Lua 5.4 stores remain version 5 and ordinary stores keep their existing version.
Unknown profiles and dialect fields on non-Lua configurations refuse; no runtime is inferred from
an executable name. The existing trusted IProcessLauncher/contained-process/input path owns Run.

LuaJIT accepts its explicit supported Lua 5.1 CLI subset and -jon/-joff, refusing the Lua 5.4 -W flag.
Luau accepts the reviewed -O0..2/-g0..2 subset, passes script arguments after its actual -a delimiter,
and enters its own REPL with no synthetic Lua 5.4 -i. These are CLI contracts, not measured runtime
compatibility. The pinned [Luau CLI source](https://github.com/luau-lang/luau/blob/fb63edcceadafc7144d3de7a6ccb0fee83978cf0/CLI/src/Repl.cpp)
is the argv oracle; no interpreter was executed to obtain these facts.

LuaJIT analysis uses the existing explicit LuaLS `lua.runtimeVersion = LuaJIT` selection; .luarc.json
retains its documented per-workspace precedence. Choosing a Run executable does not silently rewrite
analysis configuration. Luau uses the independent MIT luau-lsp provider with languageId `luau`,
arguments `lsp`, and the standard non-Roblox platform configuration. `tools/lua_dialects.py manifest`
produces an exact local executable/license-pinned registry manifest, imported through the normal
Language Server Registry dialog. It selects `.luau` only. Registry admission, trust revisions,
physical workspace boundaries and actual LSP results remain authoritative; `.lua` is not stolen
from the specialized Lua/LuaJIT service and no LuaLS fallback is used for Luau.

The current native syntax grammar remains the pinned ordinary Lua grammar. No Luau grammar is
installed or falsely mapped to Lua. Luau semantic completion/navigation/diagnostics can come from
the actual registry LSP; native Luau syntax highlighting, syntax-based edits/SSR and complete LuaJIT
FFI/cdata syntax coverage are not established. A dedicated native Luau grammar requires a separate
pinned source/license integration. The typed and FFI fixtures make those boundaries visible.

The source-built standalone debugger profile in ADR 0094 explicitly selects the Lua 5.4 C API.
Luau/LuaJIT Run or analysis does not prove debugger ABI compatibility. Debug refuses non-lua54
profiles before selecting/starting an adapter; it never switches to native or engine debugging.
Busted/LuaUnit does not acquire Luau compatibility by sharing a script run type. Engine Lua and
paired engine debugging retain their existing separate protocol and runtime identity.

## Source, licences and deferred recipes

`sdk/lua-dialects/sources.json` pins immutable upstream source commits and exact original license
text hashes for Luau 0.690 (MIT), LuaJIT 2.1 at 96f7b297 (MIT), and luau-lsp 1.70.1 (MIT).
Primary notices are [Luau](https://github.com/luau-lang/luau/blob/fb63edcceadafc7144d3de7a6ccb0fee83978cf0/LICENSE.txt),
[LuaJIT](https://github.com/LuaJIT/LuaJIT/blob/96f7b297326e9fd454b7186c6858b2773f94a0dc/COPYRIGHT), and
[luau-lsp](https://github.com/JohnnyMorganz/luau-lsp/blob/0382dc76ca9b73df3c8e7c9cd85a8dd493cbd0ae/LICENSE.md).
The recipe checks the repository SPDX allowlist. Source commit signatures were inspected through
public GitHub metadata: the selected Luau commit reports verified; the other two report unverified.
The latter are immutable source/content pins, not signed release authenticity claims.

The opt-in offline builder requires a pre-existing clean exact source checkout, initialized exact
gitlinks, matching notice, fresh separate work directory and explicit Clang/tool paths. It never
fetches source/dependencies or installs a runtime during project open. CMake FetchContent networking
is disabled; missing offline sources fail. LuaJIT Windows builds patch only a private source copy
from upstream cl/link/lib names to the selected clang-cl/lld-link/llvm-lib tools and retain the
upstream assembly/build steps. Linux uses explicit Clang and make. A build receipt records the
resulting actual binary digest, source and recipe identity without pretending tests or runtime ran.
No built binary digest is invented in the source pin.

These recipes and source pins are not distribution approval. Transitive source/notice graph review,
Windows native assembler/toolchain compatibility, relocatable payload/system dependencies, built
artifact verification, actual Luau-LSP/FFI behavior and debugger expansion still need their explicit
verification legs. No binary or source archive was downloaded, no licence accepted and no compiler,
interpreter, test runner, build/configuration or WSL command ran during this checkpoint. Public
metadata/license/CLI source was read only for provenance and contract inspection.

## Authored acceptance

Regression sources cover version-6 round trips and old-store refusal, exact Luau -a/REPL argv,
LuaJIT-specific flags and incompatible flag/profile refusal, independent registry .luau routing,
normal selected Run through scripted process ownership and non-Lua54 Debug starting zero adapters.
Typed Luau and LuaJIT FFI fixture sources are committed for later real-tool acceptance. The source
recipe/manifest unit tests never launch a provider. None of these authored tests was executed;
this bounded checkpoint does not establish full LUA-16 runtime or native grammar completion.
