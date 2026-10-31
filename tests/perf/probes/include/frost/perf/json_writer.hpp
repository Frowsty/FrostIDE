// SPDX-License-Identifier: Apache-2.0
//
// A minimal streaming JSON writer for perf reports (no dependency). The caller
// opens and closes objects and arrays in order; the writer inserts commas and
// escapes strings. Not thread-safe; one writer per report.
#pragma once

#include "frost/perf/stats.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::perf {

class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    // Writes an object key; the next call writes its value. Precondition: inside an object.
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(double number); // NaN and infinities are written as null.
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(std::uint64_t number);
    JsonWriter& value(int number) { return value(static_cast<std::int64_t>(number)); }
    JsonWriter& value(bool flag);
    JsonWriter& null();

    // key(name).value(v) in one call.
    template<class T>
    JsonWriter& field(std::string_view name, const T& v) {
        key(name);
        return value(v);
    }

    // Writes `summary` as {"count":..,"min":..,"mean":..,"p50":..,"p90":..,"p99":..,"max":..}.
    JsonWriter& value(const Summary& summary);

    // The document so far. Complete once every object and array is closed.
    [[nodiscard]] const std::string& str() const noexcept { return m_out; }

private:
    void separate();

    std::string m_out;
    // One entry per open container: true until its first element is written.
    std::vector<bool> m_first;
    bool m_afterKey = false;
};

} // namespace frost::perf
