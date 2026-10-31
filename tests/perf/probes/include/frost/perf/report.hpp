// SPDX-License-Identifier: Apache-2.0
//
// JSON report fragments shared by the perf harnesses, so every S1/B1/B5/B6
// report spells the session verdict, pacing statistics and DWM deltas the same
// way. Each function writes one JSON value (an object) at the writer's position.
#pragma once

#include "frost/perf/frame_analysis.hpp"
#include "frost/perf/json_writer.hpp"
#include "frost/perf/probe_validation.hpp"
#include "frost/perf/session_state.hpp"

#include <span>
#include <string>

namespace frost::perf {

// {"valid":..,"problems":[..]}
void writeVerdict(JsonWriter& json, const SessionVerdict& verdict);

// {"updates":..,"refreshes":..,"missedRefreshes":..,"missedFraction":..,"maxIntervalMs":..,
//  "intervalsMs":{summary},"mergedCaptures":..}
void writeContent(JsonWriter& json, const ContentStats& stats);

// {"refreshes":..,"compositions":..,"missed":..,"dropped":..,"late":..,"missedFraction":..,
//  "compositionFraction":..}
void writeDwm(JsonWriter& json, const DwmDelta& delta);

// {"shown":..,"neverShown":..,"readyToScreenMs":{summary},"inputToScreenMs":{summary}}
void writeLatency(JsonWriter& json, const LatencyStats& stats);

// {"injectedMs":..,"trials":..,"lost":..,"latencyMs":{..},"residualMs":{..},"meanShiftMs":..,"shiftErrorMs":..}
void writeDelay(JsonWriter& json, const DelayResult& result);

// ["...", ...]
void writeStrings(JsonWriter& json, std::span<const std::string> strings);

} // namespace frost::perf
