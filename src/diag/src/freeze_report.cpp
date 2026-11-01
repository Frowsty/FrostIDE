// SPDX-License-Identifier: Apache-2.0
//
// The freeze report's JSON (frost/diag/freeze_report.hpp): a minimal writer for exactly this
// document, so frost::diag needs no JSON library.

#include "frost/diag/freeze_report.hpp"

#include <format>

namespace frost::diag {
namespace {

// The length of the UTF-8 sequence starting at text[i], or 0 if it is not well-formed
// (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF).
std::size_t utf8SequenceLength(std::string_view text, std::size_t i) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    const auto continuation = [&](std::size_t k) { return k < text.size() && (byte(k) & 0xC0) == 0x80; };
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return continuation(i + 1) ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (!continuation(i + 1) || !continuation(i + 2)) {
            return 0;
        }
        const unsigned char second = byte(i + 1);
        if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F)) {
            return 0;
        }
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (!continuation(i + 1) || !continuation(i + 2) || !continuation(i + 3)) {
            return 0;
        }
        const unsigned char second = byte(i + 1);
        if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
            return 0;
        }
        return 4;
    }
    return 0;
}

const char* kindName(ModuleIdentity::Kind kind) noexcept {
    switch (kind) {
    case ModuleIdentity::Kind::Pdb70: return "pdb70";
    case ModuleIdentity::Kind::GnuBuildId: return "gnu-build-id";
    case ModuleIdentity::Kind::None: break;
    }
    return "none";
}

} // namespace

void appendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            ++i;
            continue;
        case '\\':
            out += "\\\\";
            ++i;
            continue;
        case '\b':
            out += "\\b";
            ++i;
            continue;
        case '\f':
            out += "\\f";
            ++i;
            continue;
        case '\n':
            out += "\\n";
            ++i;
            continue;
        case '\r':
            out += "\\r";
            ++i;
            continue;
        case '\t':
            out += "\\t";
            ++i;
            continue;
        default: break;
        }
        if (u < 0x20 || u == 0x7F) {
            out += std::format("\\u{:04x}", u);
            ++i;
            continue;
        }
        const std::size_t length = utf8SequenceLength(text, i);
        if (length == 0) {
            out += "\\ufffd";
            ++i;
            continue;
        }
        out.append(text.substr(i, length));
        i += length;
    }
    out.push_back('"');
}

std::string toJson(const FreezeReport& report) {
    std::string out;
    out.reserve(4096 + report.samples.size() * 64 * 24);
    const auto key = [&out](std::string_view name) {
        appendJsonString(out, name);
        out += ": ";
    };
    out += "{\n  ";
    key("schema");
    appendJsonString(out, "frost-freeze-report/1");
    out += ",\n  ";
    key("version");
    appendJsonString(out, report.version);
    out += std::format(",\n  \"pid\": {},\n  \"thread\": {{\"id\": {}, \"name\": ", report.pid, report.threadId);
    appendJsonString(out, report.threadName);
    out += "},\n  ";
    key("started_utc");
    appendJsonString(out, report.startedUtc);
    out += std::format(",\n  \"duration_ms\": {},\n  \"ongoing\": {},\n  \"unreliable\": {},\n"
                       "  \"threshold_ms\": {},\n  \"sample_period_ms\": {},\n  \"modules\": [",
                       report.durationMs, report.ongoing ? "true" : "false", report.unreliable ? "true" : "false",
                       report.thresholdMs, report.samplePeriodMs);
    for (std::size_t i = 0; i < report.modules.size(); ++i) {
        const ModuleIdentity& module = report.modules[i];
        out += i == 0 ? "\n    " : ",\n    ";
        out += std::format("{{\"index\": {}, \"path\": ", i);
        appendJsonString(out, module.path);
        out += std::format(", \"base\": \"0x{:x}\", \"size\": {}, \"id\": {{\"kind\": \"{}\"", module.base, module.size,
                           kindName(module.kind));
        if (module.kind == ModuleIdentity::Kind::Pdb70) {
            out += ", \"guid\": ";
            appendJsonString(out, module.guid);
            out += std::format(", \"age\": {}, \"pdb\": ", module.age);
            appendJsonString(out, module.pdb);
        } else if (module.kind == ModuleIdentity::Kind::GnuBuildId) {
            out += ", \"hex\": ";
            appendJsonString(out, module.buildId);
        }
        out += "}}";
    }
    out += report.modules.empty() ? "],\n  \"samples\": [" : "\n  ],\n  \"samples\": [";
    for (std::size_t i = 0; i < report.samples.size(); ++i) {
        const FreezeSample& sample = report.samples[i];
        out += i == 0 ? "\n    " : ",\n    ";
        out += std::format("{{\"t_ms\": {}, \"frames\": [", sample.tMs);
        for (std::size_t f = 0; f < sample.frames.size(); ++f) {
            if (f != 0) {
                out += ", ";
            }
            out += std::format("[{}, \"0x{:x}\"]", sample.frames[f].first, sample.frames[f].second);
        }
        out += "]}";
    }
    out += report.samples.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

} // namespace frost::diag
