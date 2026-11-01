// SPDX-License-Identifier: Apache-2.0
//
// Freeze reports: the JSON document the stall watchdog writes for a UI-thread stall, schema
// "frost-freeze-report/1" (ADR 0022, D-I), and the minimal JSON writer that produces it.
//
/*
 * {
 *   "schema": "frost-freeze-report/1", "version": "<frost version>", "pid": 1234,
 *   "thread": {"id": 5678, "name": "main"},
 *   "started_utc": "2026-11-01T18:59:12.123Z", "duration_ms": 312, "ongoing": false,
 *   "unreliable": false, "threshold_ms": 50, "sample_period_ms": 10,
 *   "modules": [{"index": 0, "path": "...", "base": "0x7ff6...", "size": 123,
 *                "id": {"kind": "pdb70", "guid": "...", "age": 1, "pdb": "..."}
 *                   | {"kind": "gnu-build-id", "hex": "..."} | {"kind": "none"}}],
 *   "samples": [{"t_ms": 25, "frames": [[0, "0x1a2b"], [-1, "0x7ff612345678"]]}]
 * }
 * A frame is [module index, "0x<offset from the module base>"], or [-1, "0x<absolute address>"]
 * when no module contains it. The first frame of a sample is the instruction pointer, the
 * others are return addresses. t_ms is measured from the stall's last progress point.
 */
#pragma once

#include "frost/diag/module_identity.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::diag {

struct FreezeSample {
    std::int64_t tMs = 0;
    std::vector<std::pair<int, std::uint64_t>> frames; // (module index or -1, offset or address)
};

struct FreezeReport {
    std::string version;
    std::uint32_t pid = 0;
    std::uint64_t threadId = 0;
    std::string threadName;
    std::string startedUtc;
    std::int64_t durationMs = 0;
    bool ongoing = false;
    bool unreliable = false;
    std::int64_t thresholdMs = 50;
    std::int64_t samplePeriodMs = 10;
    std::vector<ModuleIdentity> modules;
    std::vector<FreezeSample> samples;
};

// The report as JSON (one object, two-space indentation, a final newline). Pure.
[[nodiscard]] std::string toJson(const FreezeReport& report);

// Appends `text` to `out` as a JSON string literal: quotes, backslashes and control characters
// escaped (\n, \t, ... or \u00XX); bytes that are not valid UTF-8 become �. Pure.
void appendJsonString(std::string& out, std::string_view text);

} // namespace frost::diag
