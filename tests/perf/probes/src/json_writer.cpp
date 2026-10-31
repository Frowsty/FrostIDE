// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the report JSON writer (see frost/perf/json_writer.hpp).

#include "frost/perf/json_writer.hpp"

#include "frost/core/assert.hpp"

#include <cmath>
#include <format>

namespace frost::perf {

void JsonWriter::separate() {
    if (m_afterKey) {
        m_afterKey = false;
        return;
    }
    if (!m_first.empty()) {
        if (!m_first.back()) {
            m_out += ',';
        }
        m_first.back() = false;
    }
}

JsonWriter& JsonWriter::beginObject() {
    separate();
    m_out += '{';
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    FROST_PRE(!m_first.empty() && !m_afterKey);
    m_first.pop_back();
    m_out += '}';
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    separate();
    m_out += '[';
    m_first.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    FROST_PRE(!m_first.empty() && !m_afterKey);
    m_first.pop_back();
    m_out += ']';
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    FROST_PRE(!m_first.empty() && !m_afterKey);
    value(name);
    m_out += ':';
    m_afterKey = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    separate();
    m_out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': m_out += "\\\""; break;
        case '\\': m_out += "\\\\"; break;
        case '\n': m_out += "\\n"; break;
        case '\r': m_out += "\\r"; break;
        case '\t': m_out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                m_out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                m_out += c; // UTF-8 bytes pass through unchanged.
            }
        }
    }
    m_out += '"';
    return *this;
}

JsonWriter& JsonWriter::value(double number) {
    if (!std::isfinite(number)) {
        return null();
    }
    separate();
    m_out += std::format("{}", number);
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
    separate();
    m_out += std::format("{}", number);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number) {
    separate();
    m_out += std::format("{}", number);
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    separate();
    m_out += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    separate();
    m_out += "null";
    return *this;
}

JsonWriter& JsonWriter::value(const Summary& summary) {
    beginObject();
    field("count", static_cast<std::uint64_t>(summary.count));
    field("min", summary.min);
    field("mean", summary.mean);
    field("p50", summary.p50);
    field("p90", summary.p90);
    field("p99", summary.p99);
    field("max", summary.max);
    return endObject();
}

} // namespace frost::perf
