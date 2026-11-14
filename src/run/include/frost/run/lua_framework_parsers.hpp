// SPDX-License-Identifier: Apache-2.0
// LUA-12: explicit upstream framework protocols, using the selected LuaScript interpreter.
#pragma once
#include "frost/run/framework_parsers.hpp"
#include "frost/run/run_configuration.hpp"
namespace frost::run {
enum class LuaTestProfile : std::uint8_t { Busted220, LuaUnit35 };
// Pure planner. Preserves interpreter/script/environment identity, refuses conflicting report
// controls. An empty report on a run plans Debug with its normal console reporter. Discovery bridge is an absolute
// installed FrostIDE asset, not a project module.
[[nodiscard]] Result<RunConfiguration> luaFrameworkConfiguration(LuaTestProfile profile, RunConfiguration configuration,
                                                                 std::span<const std::string> cases,
                                                                 std::string_view report, bool listing,
                                                                 std::string_view discoveryBridge);
[[nodiscard]] Result<std::vector<FrameworkCase>> parseLuaFrameworkList(LuaTestProfile profile, std::string_view text);
// Complete genuine upstream JUnit only. Exact selected set and counters are mandatory; no exit
// code, diagnostic prose or discovery catalogue is interpreted as a passing result.
[[nodiscard]] Result<TestResult> parseLuaFrameworkRun(LuaTestProfile profile, std::string_view name,
                                                      std::string_view xml, std::span<const std::string> selected,
                                                      int exitCode, std::string_view output);
[[nodiscard]] std::string luaExactPattern(std::string_view name);
} // namespace frost::run
