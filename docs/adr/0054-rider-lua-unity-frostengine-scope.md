<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0054: Expanded product scope and reviewed parallel implementation

- Status: accepted (explicit user instruction, 2026-11-02)
- Plan reference: PLAN.md scope update and Appendix B; ADR 0031 (feature-first), ADR 0032 (W1)

## Context

The user asked to continue Claude's work toward a "1:1 copy of Jetbrains Rider excluding any copyright
infringing parts", with CMake, "Top notch Lua support", Unity integration like Rider and their FrostEngine.
They explicitly requested GPT-6.1 Terra implementation agents verified by GPT-6 Astra. The existing plan
targets a C/C++ IDE and explicitly excludes Unity; leaving that restriction in force would contradict the
new instruction. The unfinished workbench merge and existing worktrees contain substantial user work.

## Decision

1. The product target is observable feature, interaction and workflow parity with Rider, implemented
   independently with FrostIDE's own assets, wording and branding. Public product documentation and user
   workflows define acceptance. Do not consult or copy proprietary IDE source, packaged assets or theme
   files. Compatibility does not imply binary plugin compatibility or permission to redistribute third-party
   components. Existing dependency licence checks remain in force.
2. W1 and U1 stay useful intermediate builds. Complete their features and preserve their tests while adding
   the Lua, Unity, FrostEngine and managed-language tracks in Appendix B. No old estimate or feature count
   is presented as an estimate or complete inventory of the expanded scope. A capability inventory based
   on the public Rider 2026.2 documentation is required before claiming parity; new upstream releases need
   a recorded delta rather than silently changing the reference version.
3. The intended FrostEngine is `D:\Dev\FrostEngine-AI-Claude` (the verified directory matching the user's
   `D:\Dev\FrostEngin-AI-Claude` spelling). Other local FrostEngine repositories are not the target.
   Read its `Docs/ScriptEditor.md` version-1 contract with v1.1 clarifications and `Docs/Automation.md` as
   integration specifications. Do not modify the engine or users' projects merely to inspect them.
4. Reuse the engine's generated `.frost/lua-api/frost.lua` and `.luarc.json`; the engine owns its API
   definitions. Its Lua 5.4 sandbox, `Assets/Scripts/?.lua` module resolution and disabled builtins must be
   reflected in language assistance. FrostIDE must not silently replace or loosen that configuration.
5. FrostEngine integration covers opening scripts at locations, matching the correct running project,
   play controls, script validation, Lua test results and navigable logs. Discovery and authenticated
   loopback RPC follow the existing engine contract; tokens must never reach logs. Project identity and
   trust are checked before operations. Lua debugging requires a compatible runtime/debug protocol and
   remains outstanding until implemented and exercised; ordinary engine automation is not a debugger.
6. Unity integration includes the managed-language foundation, editor/player debugging, editor controls,
   assets and serialized references, tests, logs and shader workflows. Opening a generated solution alone
   does not establish Unity support. Use documented Unity APIs and a separately implemented editor bridge.
7. Use separate worktrees or exclusive file ownership for Terra implementation. Astra reviews correctness,
   lifetime, concurrency, trust, integration and test evidence. Address blocking findings and verify the
   combined tree before completing its merge. Preserve Claude's uncommitted and staged work. No CI.
8. Feature-first deferral applies to the expensive hardening checks listed in ADR 0031, not missing product
   behavior. In particular, terminal input required for Claude Code and U1 crash-stop behavior remain
   required even if a branch's notes called them deferred. Track implemented, integrated and verified
   separately; passing component tests cannot establish application-level acceptance.

## Initial sequence and evidence

Restore and verify the W1 composition; finish GPU diagnostic rendering; integrate CMake and project-wide
search; finish and integrate the terminal and debugger; connect the Lua service and FrostEngine contract;
implement the managed-language/Unity vertical slices. Independent backend work can run alongside integration.
Git, settings and the Claude bridge retain their planned scope. This is a dependency order, not a scope cut.

The initial audit found nine unmerged index entries in the W1 merge, unintegrated CMake/search/Git components,
unfinished GPU rendering, an uncommitted debugger and an unmerged terminal. The LLDB spike did not run the
planned Notepad++ scenario and records a Windows abort case without a stop. Preserve those limitations in
reports until new tests establish otherwise.

## Public references

- [Rider Unity features](https://www.jetbrains.com/help/rider/Features_Unity.html): workflow reference,
  inspected 2026-11-02; no implementation source or assets reused.
- [LuaLS](https://luals.github.io/): candidate language service; the service slice records its actual
  configuration, supported protocol and tests separately.
- FrostEngine `Docs/ScriptEditor.md`: local integration contract, including generated Lua definitions,
  custom editor arguments, per-project discovery and available automation commands.
