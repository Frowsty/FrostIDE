// SPDX-License-Identifier: Apache-2.0
// Only complete actual TestRunnerApi snapshots confer selection authority.
#include "unity_tests_window.hpp"

#include <algorithm>
#include <cmath>
#include <set>
namespace frost::app {
namespace {
bool token(std::string_view value) {
    return value.size() == 32 &&
           std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool text(const lang::Json& value, std::string_view key, size_t limit) {
    const auto* field = value.find(key);
    return field && field->isString() && field->asString()->size() <= limit &&
           field->asString()->find('\0') == std::string::npos;
}
} // namespace
Result<UnityTestObservation> parseUnityTestState(const lang::Json& response) {
    if (!response.isObject() || response.integerAt("version") != 1 || response.boolAt("available") != true ||
        response.boolAt("ok") != true)
        return fail(ErrorCode::Corrupt, "Unity returned no negotiated available test state");
    for (const auto key : {"owner", "job", "catalog", "mode", "state", "message", "execution"})
        if (!text(response, key, key == std::string_view("message") ? 2048 : 128))
            return fail(ErrorCode::Corrupt, "Unity test state field is missing or oversized");
    UnityTestObservation result;
    result.owner = response.stringAt("owner");
    result.job = response.stringAt("job");
    result.catalog = response.stringAt("catalog");
    result.mode = response.stringAt("mode");
    result.state = response.stringAt("state");
    result.message = response.stringAt("message");
    result.execution = response.stringAt("execution");
    if (!token(result.owner) || (!result.job.empty() && !token(result.job)) ||
        (!result.catalog.empty() && !token(result.catalog)) ||
        (!result.mode.empty() && result.mode != "EditMode" && result.mode != "PlayMode"))
        return fail(ErrorCode::Corrupt, "Unity test ownership or mode is invalid");
    const std::set<std::string> states{"idle",       "discovering", "discovered", "queued",    "running",
                                       "cancelling", "finished",    "cancelled",  "uncertain", "error"};
    if (!states.contains(result.state) || (result.state != "idle" && result.job.empty()))
        return fail(ErrorCode::Corrupt, "Unity test state has no actual job identity");
    if (!result.execution.empty()) {
        if (result.execution.size() != 36 || result.execution == "00000000-0000-0000-0000-000000000000")
            return fail(ErrorCode::Corrupt, "Unity returned an invalid actual execution GUID");
        for (size_t i = 0; i < result.execution.size(); ++i) {
            const char c = result.execution[i];
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (c != '-')
                    return fail(ErrorCode::Corrupt, "Unity returned an invalid actual execution GUID");
            } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return fail(ErrorCode::Corrupt, "Unity returned an invalid actual execution GUID");
        }
    }
    if ((result.state == "queued" || result.state == "running" || result.state == "cancelling" ||
         result.state == "finished") &&
        result.execution.empty())
        return fail(ErrorCode::Corrupt, "Unity run state has no actual framework execution identity");
    return result;
}
Result<UnityTestObservation> parseUnityTestPage(const lang::Json& response, std::string_view snapshot, int offset) {
    auto result = parseUnityTestState(response);
    if (!result)
        return result;
    const auto total = response.integerAt("total"), at = response.integerAt("offset"),
               next = response.integerAt("next");
    const auto* cases = response.find("cases");
    if (!text(response, "snapshot", 32) || !token(response.stringAt("snapshot")) ||
        (!snapshot.empty() && response.stringAt("snapshot") != snapshot) || !total || !at || !next || *total < 0 ||
        *total > 2048 || *at != offset || *at < 0 || *at > *total || *next < *at || *next > *total || !cases ||
        !cases->isArray() || cases->size() > 8 || *next - *at != static_cast<std::int64_t>(cases->size()) ||
        (*next < *total && cases->size() == 0))
        return fail(ErrorCode::Corrupt, "Unity test snapshot bounds or continuation changed");
    result->snapshot = response.stringAt("snapshot");
    result->total = static_cast<int>(*total);
    result->offset = offset;
    result->next = static_cast<int>(*next);
    std::set<std::string> ids;
    for (const auto& value : cases->items()) {
        for (const auto key : {"id", "name", "fullName", "outcome", "message", "stack"}) {
            const auto limit = key == std::string_view("fullName") ? 1024
                               : key == std::string_view("stack")  ? 2048
                               : key == std::string_view("name")   ? 256
                                                                   : 512;
            if (!text(value, key, static_cast<size_t>(limit)))
                return fail(ErrorCode::Corrupt, "Unity test row fields exceed the observation contract");
        }
        const auto* duration = value.find("duration");
        const auto seconds = duration ? duration->asDouble() : std::nullopt;
        const auto truncated = value.boolAt("truncated");
        UnityTestCase row;
        row.id = value.stringAt("id");
        row.name = value.stringAt("name");
        row.fullName = value.stringAt("fullName");
        row.outcome = value.stringAt("outcome");
        row.message = value.stringAt("message");
        row.stack = value.stringAt("stack");
        const std::set<std::string> outcomes{"Discovered", "Pending", "Running",     "Passed",
                                             "Failed",     "Skipped", "Inconclusive"};
        if (row.id.empty() || row.name.empty() || row.fullName.empty() || !ids.insert(row.id).second || !seconds ||
            !std::isfinite(*seconds) || *seconds < 0 || *seconds > 31536000 || !truncated ||
            !outcomes.contains(row.outcome))
            return fail(ErrorCode::Corrupt, "Unity test row has no unambiguous framework identity/outcome");
        row.duration = *seconds;
        row.truncated = *truncated;
        result->cases.push_back(std::move(row));
    }
    return result;
}
} // namespace frost::app
