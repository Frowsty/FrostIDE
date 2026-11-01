// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the shared report fragments (see frost/perf/report.hpp).

#include "frost/perf/report.hpp"

namespace frost::perf {

void writeStrings(JsonWriter& json, std::span<const std::string> strings) {
    json.beginArray();
    for (const std::string& text : strings) {
        json.value(std::string_view(text));
    }
    json.endArray();
}

void writeVerdict(JsonWriter& json, const SessionVerdict& verdict) {
    json.beginObject();
    json.field("valid", verdict.valid);
    json.key("problems");
    writeStrings(json, verdict.problems);
    json.endObject();
}

void writeContent(JsonWriter& json, const ContentStats& stats) {
    json.beginObject();
    json.field("updates", static_cast<std::uint64_t>(stats.updates));
    json.field("refreshes", static_cast<std::uint64_t>(stats.refreshes));
    json.field("missedRefreshes", static_cast<std::uint64_t>(stats.missedRefreshes));
    json.field("missedFraction", stats.missedFraction);
    json.field("maxIntervalMs", stats.maxIntervalMs);
    json.key("intervalsMs").value(stats.intervalsMs);
    json.field("mergedCaptures", static_cast<std::uint64_t>(stats.mergedCaptures));
    json.endObject();
}

void writeDwm(JsonWriter& json, const DwmDelta& delta) {
    json.beginObject();
    json.field("refreshes", delta.refreshes);
    json.field("compositions", delta.compositions);
    json.field("missed", delta.missed);
    json.field("dropped", delta.dropped);
    json.field("late", delta.late);
    json.field("missedFraction", delta.missedFraction);
    json.field("compositionFraction", delta.compositionFraction);
    json.endObject();
}

void writeLatency(JsonWriter& json, const LatencyStats& stats) {
    json.beginObject();
    json.field("shown", static_cast<std::uint64_t>(stats.shown));
    json.field("neverShown", static_cast<std::uint64_t>(stats.neverShown));
    json.key("readyToScreenMs").value(stats.readyToScreenMs);
    json.key("inputToScreenMs").value(stats.inputToScreenMs);
    json.endObject();
}

void writeDelay(JsonWriter& json, const DelayResult& result) {
    json.beginObject();
    json.field("injectedMs", result.injectedMs);
    json.field("trials", static_cast<std::uint64_t>(result.trials));
    json.field("lost", static_cast<std::uint64_t>(result.lost));
    json.key("latencyMs").value(result.latencyMs);
    json.key("residualMs").value(result.residualMs);
    json.field("meanShiftMs", result.meanShiftMs);
    json.field("shiftErrorMs", result.shiftErrorMs);
    json.endObject();
}

} // namespace frost::perf
