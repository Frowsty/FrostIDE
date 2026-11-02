// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the live diagnostics model (diagnostics.hpp): decoding publishDiagnostics
// (severity, code, source, related information, version), mapping to a frost::text snapshot, and
// DiagnosticsStore (replace, clear, Problems order, counts, listeners).

#include "frost/lang/diagnostics.hpp"
#include "frost/lang/uri.hpp"
#include "frost/text/document.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::lang::DiagnosticSeverity;
using frost::lang::DiagnosticsStore;
using frost::lang::FileDiagnostics;
using frost::lang::Json;
using frost::lang::LiveDiagnostic;
using frost::lang::PositionEncoding;

#ifdef _WIN32
const std::string kFile = "C:\\src\\main.cpp";
const std::string kHeader = "C:\\src\\main.h";
#else
const std::string kFile = "/src/main.cpp";
const std::string kHeader = "/src/main.h";
#endif

Json range(int l1, int c1, int l2, int c2) {
    return Json::object()
        .set("start", Json::object().set("line", l1).set("character", c1))
        .set("end", Json::object().set("line", l2).set("character", c2));
}

Json publish() {
    return Json::object()
        .set("uri", frost::lang::pathToUri(kFile).value())
        .set("version", 3)
        .set("diagnostics",
             Json::array({
                 Json::object()
                     .set("range", range(1, 4, 1, 7))
                     .set("severity", 1)
                     .set("code", "undeclared_var_use")
                     .set("source", "clang")
                     .set("message", "Use of undeclared identifier 'foo'")
                     .set("relatedInformation",
                          Json::array({Json::object()
                                           .set("location", Json::object()
                                                                .set("uri", frost::lang::pathToUri(kHeader).value())
                                                                .set("range", range(9, 0, 9, 3)))
                                           .set("message", "declared here")})),
                 Json::object().set("range", range(0, 0, 0, 1)).set("severity", 2).set("code", 42).set("message", "w"),
                 Json::object().set("message", "no range: skipped"),
                 Json::object().set("range", range(0, 2, 0, 3)).set("message", "no severity: error"),
             }));
}

TEST(Diagnostics, DecodesPublishDiagnostics) {
    auto decoded = frost::lang::decodePublishDiagnostics(publish());
    ASSERT_TRUE(decoded.has_value()) << decoded.error().describe();
    EXPECT_EQ(decoded->file, kFile);
    EXPECT_EQ(decoded->lspVersion, 3);
    ASSERT_EQ(decoded->diagnostics.size(), 3U);
    const LiveDiagnostic& first = decoded->diagnostics[0];
    EXPECT_EQ(first.severity, DiagnosticSeverity::Error);
    EXPECT_EQ(first.code, "undeclared_var_use");
    EXPECT_EQ(first.source, "clang");
    EXPECT_EQ(first.file, kFile);
    EXPECT_EQ(first.line, 2);
    EXPECT_EQ(first.column, 5);
    ASSERT_EQ(first.related.size(), 1U);
    EXPECT_EQ(first.related[0].file, kHeader);
    EXPECT_EQ(first.related[0].line, 10);
    EXPECT_EQ(first.related[0].message, "declared here");
    EXPECT_EQ(decoded->diagnostics[1].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(decoded->diagnostics[1].code, "42");
    EXPECT_EQ(decoded->diagnostics[2].severity, DiagnosticSeverity::Error);
}

TEST(Diagnostics, RejectsParamsWithoutUriOrList) {
    EXPECT_FALSE(frost::lang::decodePublishDiagnostics(Json::object().set("diagnostics", Json::array())).has_value());
    EXPECT_FALSE(frost::lang::decodePublishDiagnostics(Json::object().set("uri", "file:///x")).has_value());
    EXPECT_FALSE(
        frost::lang::decodePublishDiagnostics(Json::object().set("uri", "untitled:1").set("diagnostics", Json::array()))
            .has_value());
}

TEST(Diagnostics, MapsToTheSnapshotTheServerSaw) {
    auto document = frost::text::Document::fromUtf8("int x;\nint foo();\n");
    ASSERT_TRUE(document.has_value());
    auto decoded = frost::lang::decodePublishDiagnostics(publish());
    ASSERT_TRUE(decoded.has_value());
    frost::lang::mapToSnapshot(*decoded, document->text(), PositionEncoding::Utf8);
    EXPECT_EQ(decoded->textVersion, document->text().version());
    ASSERT_TRUE(decoded->diagnostics[0].range.has_value());
    EXPECT_EQ(decoded->diagnostics[0].range->begin, frost::ByteOffset{11});
    EXPECT_EQ(decoded->diagnostics[0].range->end, frost::ByteOffset{14});
    EXPECT_EQ(document->text().text(*decoded->diagnostics[0].range), "foo");
}

TEST(DiagnosticsStoreTest, PublishReplaceClearAndOrder) {
    DiagnosticsStore store;
    std::vector<std::size_t> heard;
    const auto listener =
        store.addListener([&](const FileDiagnostics& set) { heard.push_back(set.diagnostics.size()); });
    auto decoded = frost::lang::decodePublishDiagnostics(publish());
    ASSERT_TRUE(decoded.has_value());
    store.publish(*decoded);
    EXPECT_EQ(store.count(DiagnosticSeverity::Error), 2U);
    EXPECT_EQ(store.count(DiagnosticSeverity::Warning), 1U);
    const auto problems = store.problems();
    ASSERT_EQ(problems.size(), 3U);
    EXPECT_EQ(problems[0].line, 1); // Ordered by line, then column.
    EXPECT_EQ(problems[0].column, 1);
    EXPECT_EQ(problems[1].column, 3);
    EXPECT_EQ(problems[2].line, 2);
    ASSERT_TRUE(store.forFile(kFile).has_value());
    const std::uint64_t generation = store.generation();

    FileDiagnostics empty;
    empty.file = kFile;
    empty.uri = decoded->uri;
    store.publish(empty); // An empty publish clears the file.
    EXPECT_GT(store.generation(), generation);
    EXPECT_TRUE(store.files().empty());
    EXPECT_TRUE(store.problems().empty());

    store.publish(*decoded);
    store.clear();
    EXPECT_TRUE(store.files().empty());
    store.removeListener(listener);
    store.publish(*decoded);
    EXPECT_EQ(heard, (std::vector<std::size_t>{3, 0, 3, 0}));
}

} // namespace
