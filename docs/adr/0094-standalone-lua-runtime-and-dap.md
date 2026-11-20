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
sends explicit luaexe equal to Run's selected interpreter, luaVersion=lua54, UTF8, and useWSL=false.
The upstream integratedTerminal route allows FrostIDE to own the Lua child and observe its output
and exit. The standalone profile accepts one reverse request matching the admitted interpreter,
working directory, arguments and environment, and the installed extension bootstrap for the held
adapter PID. FrostIDE builds the bootstrap argument locally and launches the child directly.
The adapter can close without reporting a debuggee exit, so FrostIDE reads the held child's status.
No adapter-bundled runtime fallback is allowed. Environment removals cannot be
represented by the inspected launch schema and Debug refuses them instead of retaining variables
silently. REPL Debug is explicitly unsupported; use a script Debug configuration. Standalone Lua
build/tasks-before-launch are currently refused rather than skipped.

## Consequences

Missing tools produce actionable unavailable errors, not simulated success. DebugSession and the
ordinary controller own breakpoints, stepping, scopes, evaluation, cancellation and teardown.
Recipes, authored native tests and unit tests do not constitute runtime acceptance. LUA-11a
conformance must be exercised against the actual installed adapter and compiled interpreter;
engine-only owned-Play lifecycle checks cannot be reported as standalone passes.

### Coroutine thread identities

Standalone Lua Debug 2.3.0 does not satisfy Appendix B's coroutine-as-thread gate. In a Windows
session using the published adapter and the pinned external Lua 5.4.9, the main breakpoint and a
later breakpoint inside a newly created coroutine both reported DAP thread ID 1. At the second
stop, `threads` returned only ID 1 and `stackTrace(1)` exposed the active coroutine's frame. A
second live coroutine was visible through a bound Lua variable as `thread (suspended)` with
`variablesReference=0`. Requesting `stackTrace(2)` failed with `Not found thread [2]`. The active
coroutine's local value was readable, and the owned Lua process exited zero after continuing.

The packaged `script/backend/master/mgr.lua` allocates thread IDs for connected debugger workers.
`master/request.lua` routes `threads` and `stackTrace` through that worker table. `worker.lua`
can incorporate coroutine ancestry into a worker's stack and encodes the traversal index into its
frame IDs. That index belongs to a stack walk. It carries no lifetime or suspended-stack identity.
The thread value display in `worker/variables.lua` reports status for a reachable value. It
provides neither a registry of coroutines nor a handle for stack requests. The published launch
configuration and request handlers offer no switch or coroutine listing request.

The viable route is provider support for identities derived from actual Lua coroutine objects.
The provider must retain stable identities for live coroutines without keeping otherwise dead
objects alive, emit their creation and exit, and route stack and scope requests to the selected
coroutine at a current stop. The tracker must cover coroutine creation through the Lua C API as
well as the Lua standard library. An upstream change or a separately reviewed and pinned provider
build can expose these through ordinary DAP threads so FrostIDE can use its existing selector.
Acceptance must cover a coroutine created after launch, an independently selectable suspended
coroutine, correct per-coroutine stacks and locals, and identity removal on completion. The real
thread assertion remains a failing acceptance check with Lua Debug 2.3.0.

Primary sources: [Lua archive](https://www.lua.org/ftp/), [Lua license](https://www.lua.org/license.html),
[Lua5.4.9 source](https://www.lua.org/source/5.4/),
[lua-debug descriptor](https://github.com/actboy168/lua-debug/blob/master/extension/js/descriptorFactory.js),
[lua-debug schema](https://github.com/actboy168/lua-debug/blob/master/compile/common/package_json.lua),
[lua-debug bootstrap](https://github.com/actboy168/lua-debug/blob/master/extension/script/frontend/debuger_factory.lua).
