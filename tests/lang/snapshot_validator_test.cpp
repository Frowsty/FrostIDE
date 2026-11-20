// SPDX-License-Identifier: Apache-2.0
// Drive the actual snapshot validator executable with in-memory source and invalid envelopes.
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/json_schema.hpp"
#include "frost/lang/snapshot_validator.hpp"
#include "frost/text/text_buffer.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace frost::lang {
namespace {
using namespace std::chrono_literals;
#ifdef _WIN32
const std::string kNeverSaved = "C:/NeverSaved/";
#else
const std::string kNeverSaved = "/NeverSaved/";
#endif

Result<io::ProcessResult> worker(std::string_view request) {
    auto executable = io::normalizePath(FROST_SNAPSHOT_VALIDATOR);
    if (!executable)
        return std::unexpected(executable.error());
    io::ProcessSpec spec;
    spec.executable = std::move(*executable);
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    return io::runToCompletion(io::realProcessLauncher(), spec, request, 5s);
}

text::Snapshot snapshot(std::string source) { return text::TextBuffer::fromUtf8(std::move(source)).value().snapshot(); }

Result<Json> issues(std::string_view path, const text::Snapshot& text, const std::optional<Json>& schema = {}) {
    auto request = makeSnapshotValidationRequest(path, text, schema);
    if (!request)
        return std::unexpected(request.error());
    auto output = worker(*request);
    if (!output)
        return std::unexpected(output.error());
    if (output->exitCode || output->signal || !output->stderrBytes.empty() ||
        output->stdoutBytes.size() > kSnapshotValidationMaxOutputBytes)
        return fail(ErrorCode::IoError, output->stderrBytes);
    auto reply = rpc::parseJsonStrict(output->stdoutBytes);
    if (!reply || reply->integerAt("version") != kSnapshotValidationVersion)
        return fail(ErrorCode::Corrupt, "Snapshot worker reply version differs");
    const auto* issues = reply->find("issues");
    if (!issues || !issues->isArray())
        return fail(ErrorCode::Corrupt, "Snapshot worker reply omitted issues");
    return *issues;
}

TEST(SnapshotValidatorCli, CMakeUsesUnsavedSyntaxAndFindsOnlyUnconditionalDuplicates) {
    const auto path = kNeverSaved + "CMakeLists.txt";
    const auto invalid = issues(path, snapshot("add_custom_target("));
    ASSERT_TRUE(invalid) << invalid.error().describe();
    ASSERT_EQ(invalid->size(), 1U);
    EXPECT_EQ(invalid->items().front().stringAt("code"), "cmake.syntax");
    EXPECT_EQ(invalid->items().front().unsignedAt("begin"), 0U);

    const std::string first = "add_custom_target(alpha)\r\n";
    const auto duplicate = issues(path, snapshot(first + "  add_custom_target(alpha)\r\n"));
    ASSERT_TRUE(duplicate) << duplicate.error().describe();
    ASSERT_EQ(duplicate->size(), 1U);
    EXPECT_EQ(duplicate->items().front().stringAt("code"), "cmake.duplicate-target");
    EXPECT_EQ(duplicate->items().front().unsignedAt("begin"), first.size());
    EXPECT_EQ(duplicate->items().front().unsignedAt("end"), first.size());

    const auto clean = issues(path, snapshot(first + "if(VAR)\r\nadd_custom_target(alpha)\r\nendif()"));
    ASSERT_TRUE(clean) << clean.error().describe();
    EXPECT_EQ(clean->size(), 0U);
}

TEST(SnapshotValidatorCli, BundledJsonAndJsoncUseTheirCapturedValueRanges) {
    const auto path = kNeverSaved + "CMakePresets.json";
    const auto invalid = issues(path, snapshot("{"));
    ASSERT_TRUE(invalid) << invalid.error().describe();
    ASSERT_EQ(invalid->size(), 1U);
    EXPECT_EQ(invalid->items().front().stringAt("code"), "syntax");

    const std::string presets = R"({"note":"劇","version":"bad"})";
    const auto schemaIssue = issues(path, snapshot(presets));
    ASSERT_TRUE(schemaIssue) << schemaIssue.error().describe();
    ASSERT_EQ(schemaIssue->size(), 1U);
    const auto& issue = schemaIssue->items().front();
    EXPECT_EQ(issue.stringAt("source"), "frost-json-schema");
    EXPECT_EQ(issue.stringAt("code"), "schema");
    EXPECT_EQ(issue.unsignedAt("begin"), presets.find("\"bad\""));
    EXPECT_EQ(issue.unsignedAt("end"), presets.find("\"bad\"") + 5);
    const auto clean = issues(path, snapshot(R"({"version":6})"));
    ASSERT_TRUE(clean) << clean.error().describe();
    EXPECT_EQ(clean->size(), 0U);

    const auto luaPath = kNeverSaved + ".luarc.jsonc";
    const auto wrongType = issues(luaPath, snapshot("{/* offline */\"diagnostics.enable\":3,}"));
    ASSERT_TRUE(wrongType) << wrongType.error().describe();
    ASSERT_EQ(wrongType->size(), 1U);
    EXPECT_EQ(wrongType->items().front().stringAt("code"), "schema");
    const auto cleanJsonc = issues(luaPath, snapshot("{/* offline */\"diagnostics.enable\":true,}"));
    ASSERT_TRUE(cleanJsonc) << cleanJsonc.error().describe();
    EXPECT_EQ(cleanJsonc->size(), 0U);
}

TEST(SnapshotValidatorCli, AppliesCapturedSelectedEngineShape) {
    const auto shape =
        Json::object().set("properties", Json::object().set("Enabled", Json::object().set("type", "boolean")));
    const auto path = kNeverSaved + "asset.fmat";
    const auto invalid = issues(path, snapshot(R"({"Enabled":3,"Future":true})"), shape);
    ASSERT_TRUE(invalid) << invalid.error().describe();
    ASSERT_EQ(invalid->size(), 1U);
    EXPECT_EQ(invalid->items().front().stringAt("source"), "frost-engine-schema-shape");
    EXPECT_EQ(invalid->items().front().stringAt("code"), "schema");
    const auto clean = issues(path, snapshot(R"({"Enabled":false,"Future":true})"), shape);
    ASSERT_TRUE(clean) << clean.error().describe();
    EXPECT_EQ(clean->size(), 0U);
}

TEST(SnapshotValidatorCli, RejectsBadRequestsWithoutDiagnosticOutput) {
    for (const std::string_view request : {
             "{",
             R"({"version":2,"path":"CMakeLists.txt","text":""})",
             R"({"version":1,"version":1,"path":"CMakeLists.txt","text":""})",
             R"({"version":1,"path":"CMakeLists.txt","text":null})",
             R"({"version":1,"path":"CMakeLists.txt","text":""}[])",
             R"({"version":1,"path":"unknown.json","text":"{}"})",
             R"({"version":1,"path":"asset.fmat","text":"{}","engineSchema":{"properties":{"bad":{"type":3}}}})",
         }) {
        SCOPED_TRACE(request);
        const auto output = worker(request);
        ASSERT_TRUE(output) << output.error().describe();
        EXPECT_NE(output->exitCode, 0);
        EXPECT_TRUE(output->stdoutBytes.empty());
        EXPECT_TRUE(output->stderrBytes.starts_with("frost-snapshot-validator: "));
    }
}

TEST(SnapshotValidatorCli, OversizedDocumentRefusesAtBothEnds) {
    const auto path = kNeverSaved + "CMakeLists.txt";
    const std::string text(256 * 1024 + 1, ' ');
    const auto rejected = makeSnapshotValidationRequest(path, snapshot(text));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
    const auto request = Json::object().set("version", kSnapshotValidationVersion).set("path", path).set("text", text);
    const auto output = worker(request.dump());
    ASSERT_TRUE(output) << output.error().describe();
    EXPECT_NE(output->exitCode, 0);
    EXPECT_TRUE(output->stdoutBytes.empty());
    EXPECT_NE(output->stderrBytes.find("budget"), std::string::npos);
}
} // namespace
} // namespace frost::lang
