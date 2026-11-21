# ADR 0094: Standalone Lua interpreter and DAP

Status: accepted. The opt-in standalone coroutine profile is verified on Windows and Linux.

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

Use an explicit local coroutine profile on Windows and Linux x64. It combines the official Lua
5.4.9 sources with a small VM lifecycle tracker, and the pinned Lua Debug 2.3.0 VSIX with checked
changes to its Lua scripts. The adapter executable and native libraries are copied byte for byte
from the verified package. Both recipes work offline from local archives. The package digests
apply to the ZIP payload inside any Marketplace HTTP gzip wrapper. Each prefix retains the
upstream notices and the license and change summary for FrostIDE's additions.

The VM observes `lua_newthread`, `lua_resume`, `lua_closethread`, and garbage collection. It assigns
IDs to actual Lua threads, keeps metadata outside the Lua garbage collector, and anchors a
coroutine only during an individual inspection request. The adapter captures the native
inspection function before user code starts. DAP exposes creation and exit events, selects each
live coroutine's own stack, and resolves frame and variable handles against their originating
coroutine for the current stop. Continue resumes the VM. Step operates on the stopped coroutine.
The adapter reports a capability error for a mismatched runtime or an exhausted tracker budget.
The policy for exceeding the 4,096 tracked objects or 16,384 unconsumed events is to end the
debug session with an explanatory error. Restart-frame and individual-thread termination are
unavailable in this profile.

Build into fresh absolute work and installation directories with the existing recipe:

```text
python tools/build_lua_runtime.py --archive PATH_TO_LUA_5_4_9_TAR_GZ --work-dir NEW_WORK --prefix NEW_RUNTIME --clang PINNED_CLANG --cmake PINNED_CMAKE --ninja PINNED_NINJA --coroutine-debugger
python tools/build_lua_debug_coroutines.py --vsix PATH_TO_LUA_DEBUG_2_3_0_VSIX --prefix NEW_PROVIDER
```

Set `tools.luaInterpreter.path` for new configurations, or select `NEW_RUNTIME/bin/lua[.exe]`
in a standalone Run configuration. Set `tools.luaDebugAdapter.path` to
`NEW_PROVIDER/extension/bin/lua-debug[.exe]`. Debug uses the interpreter selected in the Run
configuration. Existing installations and other Run configurations retain their paths.
For native acceptance, configure `FROST_LUA54_EXECUTABLE` and `FROST_LUA_DEBUG_ADAPTER` with
those paths. Build `debug_standalone_lua_real_tests` and run `ctest -R '^StandaloneLuaReal[.]'`
for the selected configuration. The fixture confirms a C-created coroutine after launch,
an independently selectable Lua-created suspended coroutine, both real frames and locals,
completion and garbage-collection removal, and ordinary script, REPL, stepping, and exit behavior.

Primary sources: [Lua archive](https://www.lua.org/ftp/), [Lua license](https://www.lua.org/license.html),
[Lua5.4.9 source](https://www.lua.org/source/5.4/),
[Lua Debug 2.3.0 source](https://github.com/actboy168/lua-debug/tree/60aad0259502f354d921b08313fce74facb6a497),
[Lua Debug package listing](https://marketplace.visualstudio.com/items?itemName=actboy168.lua-debug),
[lua-debug descriptor](https://github.com/actboy168/lua-debug/blob/master/extension/js/descriptorFactory.js),
[lua-debug schema](https://github.com/actboy168/lua-debug/blob/master/compile/common/package_json.lua),
[lua-debug bootstrap](https://github.com/actboy168/lua-debug/blob/master/extension/script/frontend/debuger_factory.lua).
