# ADR 0096: Standalone Lua test framework protocols

Status: source implementation and future acceptance tests; runtime verification deferred.

## Decision

LUA-12 supports explicitly selected Busted 2.2.0 and LuaUnit 3.5 profiles. The Unit Tests
Lua tab consumes the selected Lua Script run configuration: program is the same explicit
interpreter used by Run and Debug, target is the installed Busted entry script or user LuaUnit
driver, and interpreter arguments, cwd and environment retain their existing semantics.
No shebang, shell, system Lua, bundled adapter runtime or FrostEngine service substitutes for it.
Discovery and runs use TestRunner's existing contained RunSession, trust revision, cancellation
and generation ownership. Missing tools/modules, unknown versions, malformed/missing reports
and incomplete or unexpected case sets are errors, never skipped acceptance or assumed passes.

Busted discovery invokes the real --list protocol; run verdicts use its real junit reporter.
The same interpreter/entry script reports --version before each operation. --ignore-lua prevents
.busted interpreter reexecution; repeat=1 and explicit reporter/filter CLI values override those
configuration fields. Existing module paths, roots and other framework arguments remain active.
Conflicting explicit output/list/runtime/name controls are refused. Selected names use anchored
escaped Lua patterns, not Busted's substring --name implementation. Xoutput splits commas, so
report paths containing commas are unsupported. Listing lines require upstream source:line:
name records; truncated short_src paths do not become navigable absolute locations.

LuaUnit 3.5 has no list command. FrostIDE's original tools/luaunit-discovery.lua bridge is
installed through -e before the unchanged user script. It requires version3.5 and interposes
internalRunSuiteByInstances, the shared normal runSuite/runSuiteByInstances boundary. It calls
LuaUnit's own applyTestPrefixSuffixFilter, expandClasses and applyPatternFilter, obtains source
locations from the actual selected function objects, writes an explicit discovery catalogue,
and exits before suite setup or test bodies. No source scanner predicts cases. The driver must
reach this supported runner hook; a driver which bypasses it, changes it, errors or exits early
writes no catalogue and fails discovery. The bridge does not override arg[0], positional script
arguments, cwd, environment or package paths. Custom drivers that run bodies before invoking
LuaUnit remain ordinary trusted project code; such side effects are not claimed sandboxed.
LuaUnit runs use its genuine -o junit -n report CLI. Discovery catalogues cannot create verdicts.
Selected-case reports must exactly match the requested names. LuaUnit's actual JUnit Version
property and counters are checked; class-qualified node.testName is already the full identity.
Failure messages/traces and genuine skips are preserved. Unsupported upstream XML names which
produce malformed XML fail rather than being guessed from terminal text.

Debug selects the same literal framework case using the same Lua Script configuration and real
standalone Lua DAP. It leaves the framework's terminal reporter active; Debug is not a test result.
Gutter entries come only from actual discovered function/source records and are invalidated
when configuration, workspace lifetime, profile or trust changes.

## Dependencies and primary sources

No third-party package/payload is copied or bundled by this decision. Users provision supported
frameworks/modules for the selected interpreter (including Busted's actual runtime dependencies).
Future real acceptance requires those explicit installed inputs and fails if unavailable.
Busted 2.2.0 is MIT; LuaUnit 3.5 LICENSE.txt is BSD-2-Clause. Both are existing allowlisted
license families; any future redistribution must include original notices and dependency review.
The product bridge is original Apache-2.0 integration code, not a copy of framework implementation.

- https://github.com/lunarmodules/busted/tree/v2.2.0 (bin/busted, modules/cli.lua,
  modules/filter_loader.lua, modules/configuration_loader.lua, outputHandlers/junit.lua, LICENSE)
- https://github.com/bluebird75/luaunit/blob/LUAUNIT_V3_5/luaunit.lua (normal runner and JUnitOutput)
- https://github.com/bluebird75/luaunit/blob/LUAUNIT_V3_5/LICENSE.txt

Real future acceptance compares discovered selections against the actual framework selection and
JUnit outcomes, exercises selected Debug and running-child cancellation. Authored source is not
runtime conformance, benchmark or engine owned-Play acceptance.
