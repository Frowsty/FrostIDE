// SPDX-License-Identifier: Apache-2.0
//
// The freeze report's JSON (frost/diag/freeze_report.hpp): a golden of schema
// "frost-freeze-report/1" for a fixed report (m1/automation-bench and tools/symbolize.py read
// it), and the string escaping golden: quotes, backslashes, control characters, non-ASCII
// UTF-8 kept as it is, and malformed UTF-8 replaced by U+FFFD.

#include "frost/diag/freeze_report.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using frost::diag::FreezeReport;
using frost::diag::ModuleIdentity;

TEST(FreezeReport, SchemaGolden) {
    FreezeReport report;
    report.version = "0.1.0";
    report.pid = 1234;
    report.threadId = 5678;
    report.threadName = "main";
    report.startedUtc = "2026-11-01T18:59:12.123Z";
    report.durationMs = 312;
    report.thresholdMs = 50;
    report.samplePeriodMs = 10;
    ModuleIdentity pe;
    pe.path = "C:\\Program Files\\FrostIDE\\frost_app.dll";
    pe.base = 0x7ff600000000;
    pe.size = 0x200000;
    pe.kind = ModuleIdentity::Kind::Pdb70;
    pe.guid = "01234567-89AB-CDEF-0123-456789ABCDEF";
    pe.age = 1;
    pe.pdb = "frost_app.pdb";
    ModuleIdentity elf;
    elf.path = "/opt/frost/frost";
    elf.base = 0x5555;
    elf.size = 4096;
    elf.kind = ModuleIdentity::Kind::GnuBuildId;
    elf.buildId = "abcdef";
    report.modules = {pe, elf};
    report.samples = {{25, {{0, 0x1a2b}, {-1, 0x7ff612345678}}}, {35, {{1, 0x10}}}};
    const std::string expected = R"({
  "schema": "frost-freeze-report/1",
  "version": "0.1.0",
  "pid": 1234,
  "thread": {"id": 5678, "name": "main"},
  "started_utc": "2026-11-01T18:59:12.123Z",
  "duration_ms": 312,
  "ongoing": false,
  "unreliable": false,
  "threshold_ms": 50,
  "sample_period_ms": 10,
  "modules": [
    {"index": 0, "path": "C:\\Program Files\\FrostIDE\\frost_app.dll", "base": "0x7ff600000000", "size": 2097152, "id": {"kind": "pdb70", "guid": "01234567-89AB-CDEF-0123-456789ABCDEF", "age": 1, "pdb": "frost_app.pdb"}},
    {"index": 1, "path": "/opt/frost/frost", "base": "0x5555", "size": 4096, "id": {"kind": "gnu-build-id", "hex": "abcdef"}}
  ],
  "samples": [
    {"t_ms": 25, "frames": [[0, "0x1a2b"], [-1, "0x7ff612345678"]]},
    {"t_ms": 35, "frames": [[1, "0x10"]]}
  ]
}
)";
    EXPECT_EQ(frost::diag::toJson(report), expected);
}

TEST(FreezeReport, EmptyListsStayValidJson) {
    FreezeReport report;
    report.ongoing = true;
    const std::string json = frost::diag::toJson(report);
    EXPECT_TRUE(json.contains("\"modules\": [],\n  \"samples\": []\n}\n")) << json;
    EXPECT_TRUE(json.contains("\"ongoing\": true")) << json;
}

TEST(FreezeReport, JsonStringEscapingGolden) {
    std::string out;
    frost::diag::appendJsonString(out, "quote \" backslash \\ tab \t newline \n cr \r bell \x07 del \x7f nul");
    EXPECT_EQ(out, R"("quote \" backslash \\ tab \t newline \n cr \r bell \u0007 del \u007f nul")");
    out.clear();
    frost::diag::appendJsonString(out, std::string("a\0b", 3));
    EXPECT_EQ(out, R"("a\u0000b")");
    out.clear();
    // Non-ASCII stays UTF-8: a, U+00E4, U+6D4B, U+1F600.
    frost::diag::appendJsonString(out, "a \xC3\xA4 \xE6\xB5\x8B \xF0\x9F\x98\x80");
    EXPECT_EQ(out, "\"a \xC3\xA4 \xE6\xB5\x8B \xF0\x9F\x98\x80\"");
    out.clear();
    // Malformed: a lone continuation byte, an overlong form, a surrogate, a truncated sequence.
    frost::diag::appendJsonString(out, "\x80|\xC0\xAF|\xED\xA0\x80|\xE6\xB5");
    EXPECT_EQ(out, R"("\ufffd|\ufffd\ufffd|\ufffd\ufffd\ufffd|\ufffd\ufffd")");
}

} // namespace
