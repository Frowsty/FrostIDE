// SPDX-License-Identifier: Apache-2.0
// Real Unity event frames are accepted only as complete, bounded, monotonic journal pages.
#include "unity_events.hpp"

#include <algorithm>
#include <limits>
namespace frost::app {
Result<UnityEventPage> parseUnityEvents(const lang::Json& response, std::int64_t cursor) {
    const auto first = response.integerAt("eventFirst"), next = response.integerAt("eventNext"),
               last = response.integerAt("eventLast"), lost = response.integerAt("eventLost"),
               version = response.integerAt("projectVersion");
    const auto compiling = response.boolAt("compiling");
    const auto* entries = response.find("events");
    if (cursor < 0 || !first || !next || !last || !lost || !version || !compiling || !entries || !entries->isArray() ||
        entries->items().size() > 8 || *first < 1 || *last < 0 || *last == std::numeric_limits<std::int64_t>::max() ||
        *first > *last + 1 || cursor > *last || *next < cursor || *next > *last || *version < 0 ||
        *lost != std::max<std::int64_t>(0, *first - 1 - cursor))
        return fail(ErrorCode::Corrupt, "Unity event journal bounds or continuation are invalid");
    UnityEventPage page{*first, *next, *last, *lost, *version, *compiling, {}};
    std::int64_t expected = std::max(cursor + 1, *first);
    for (const auto& row : entries->items()) {
        if (!row.isObject() || row.integerAt("sequence") != expected)
            return fail(ErrorCode::Corrupt, "Unity event sequence is not contiguous");
        for (const auto key : {"kind", "level", "message", "stack", "file", "assembly"}) {
            const auto* field = row.find(key);
            if (!field || !field->asString())
                return fail(ErrorCode::Corrupt, "Unity event text field has the wrong type");
        }
        const auto truncated = row.boolAt("truncated");
        if (!truncated)
            return fail(ErrorCode::Corrupt, "Unity event truncation flag is missing or invalid");
        UnityBridgeEvent event;
        event.sequence = expected++;
        event.kind = row.stringAt("kind");
        event.level = row.stringAt("level");
        event.message = row.stringAt("message");
        event.stack = row.stringAt("stack");
        event.file = row.stringAt("file");
        event.assembly = row.stringAt("assembly");
        const auto line = row.integerAt("line"), column = row.integerAt("column");
        if (!line || !column || *line < 0 || *line > INT32_MAX || *column < 0 || *column > INT32_MAX ||
            !(event.kind == "log" || event.kind == "compiler" || event.kind == "project" ||
              event.kind == "compilationStarted" || event.kind == "compilationFinished") ||
            !(event.level == "info" || event.level == "warning" || event.level == "error") ||
            event.message.size() > 2048 || event.stack.size() > 8192 || event.file.size() > 2048 ||
            event.assembly.size() > 2048)
            return fail(ErrorCode::Corrupt, "Unity event fields exceed the journal contract");
        event.line = static_cast<int>(*line);
        event.column = static_cast<int>(*column);
        event.truncated = *truncated;
        page.events.push_back(std::move(event));
    }
    if (*next != (page.events.empty() ? cursor : page.events.back().sequence) ||
        (page.events.empty() && *next < *last) || (!page.events.empty() && page.events.back().sequence > *last))
        return fail(ErrorCode::Corrupt, "Unity event continuation does not match its payload");
    return page;
}
} // namespace frost::app
