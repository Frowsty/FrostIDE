# ADR 0094: Standalone Lua interpreter and DAP

Status: accepted source design; recipes and acceptance sources authored, not executed.

## Context

PLAN R12 LUA-10/LUA-11b requires standalone Lua 5.4 Run, REPL and Debug using the same interpreter.
LuaLS's analysis runtime selection is not an executable. The FrostEngine endpoint is a separate
private engine service and cannot stand in for standalone execution.

## Decision

Pin the official Lua 5.4.9 source archive in sdk.lock.json (SHA256
`2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6`). This is a source digest,
not an invented digest for a built binary. The explicit offline recipe tools/build_lua_runtime.py
uses the selected pinned Clang, CMake and Ninja, builds a shared Lua API and standalone interpreter,
and installs the source copyright/MIT notice from lua.h plus upstream readme.html. Windows uses
lua54.dll and the external executable imports its exported API. Linux uses a shared library beside
the executable with an origin-relative runtime search path. No readline dependency is added.
Normal host C module ABI compatibility still requires the same architecture/default Lua 5.4 ABI;
no debugger conformance result is inferred from a successful compilation.

RunConfiguration version5 adds LuaScript/LuaRepl and interpreterArguments. Program is the explicit
absolute interpreter path; script target and script argv are separate. New configurations copy the
configured tools.luaInterpreter.path, or the app's staged lua54/lua[.exe] default. Existing configs
never silently change runtime when settings change. Old configurations retain their old store
versions. RunManager owns the interpreter process tree and pipes. REPL is actual -i with the Run
console input connected to the interpreter stdin; there is no synthetic eval implementation.
Save All follows the existing before-build/run setting. Trust revision and workspace cancellation
are checked again by the real launch writer.

Adopt an explicitly configured installed actboy168/lua-debug adapter over real stdio DAP. The
upstream descriptor starts bin/lua-debug[.exe] without arguments. The inspected schema declares
2.3.0; this is not a claim that any arbitrary installed release was tested. Keep its complete
extension/script/platform dependencies. Do not redistribute a VSIX, proprietary redistributable,
or unchecked adapter payload by default. Lua and upstream lua-debug are MIT; any later binary
packaging must preserve their licenses and audit all native/submodule dependencies separately.

The upstream bootstrap emits interpreter/-e/bootstrap, then arg0, program, arg. FrostIDE always
sends explicit luaexe equal to Run's selected interpreter, luaVersion=lua54, UTF8, internalConsole
and useWSL=false. No adapter-bundled runtime fallback is allowed. Environment removals cannot be
represented by the inspected launch schema and Debug refuses them instead of retaining variables
silently. REPL Debug is explicitly unsupported; use a script Debug configuration. Standalone Lua
build/tasks-before-launch are currently refused rather than skipped.

## Consequences

Missing tools produce actionable unavailable errors, not simulated success. DebugSession and the
ordinary controller own breakpoints, stepping, scopes, evaluation, cancellation and teardown.
Recipes, authored native tests and unit tests do not constitute runtime acceptance. LUA-11a
conformance must be exercised against the actual installed adapter and compiled interpreter;
engine-only owned-Play lifecycle checks cannot be reported as standalone passes.

Primary sources: [Lua archive](https://www.lua.org/ftp/), [Lua license](https://www.lua.org/license.html),
[Lua5.4.9 source](https://www.lua.org/source/5.4/),
[lua-debug descriptor](https://github.com/actboy168/lua-debug/blob/master/extension/js/descriptorFactory.js),
[lua-debug schema](https://github.com/actboy168/lua-debug/blob/master/compile/common/package_json.lua),
[lua-debug bootstrap](https://github.com/actboy168/lua-debug/blob/master/extension/script/frontend/debuger_factory.lua).
