// SPDX-License-Identifier: Apache-2.0
// Measured report admission: exact source receipts, unavailable values and malformed bounds.
#include "frost/engine/performance.hpp"

#include <gtest/gtest.h>

#include <limits>
namespace frost::engine {
namespace {
using rpc::Json;
EngineIdentity identity() {
    EngineIdentity value;
    value.projectPath = "selected-project";
    value.instanceId = "instance";
    value.generation = "host-generation";
    value.connectionGeneration = 7;
    return value;
}
Json report() {
    return Json::object()
        .set("frame", Json::object().set("frames", 13).set("fps", 60.0))
        .set("rendering", Json())
        .set("profiler", Json::object()
                             .set("sourceProvenance", "macro-source-location/v1")
                             .set("sourceScopes", Json::array().push(Json::object()
                                                                         .set("name", "duplicate label")
                                                                         .set("file", "compiled-source.cpp")
                                                                         .set("function", "actualFunction")
                                                                         .set("sourceSha256", std::string(64, 'a'))
                                                                         .set("line", 37)
                                                                         .set("calls", 3)
                                                                         .set("msPerFrame", 2.75)
                                                                         .set("callsPerFrame", 1.5)
                                                                         .set("averageMs", 1.0)
                                                                         .set("maxMs", 2.0))));
}
TEST(EnginePerformance, PreservesMeasuredReportIdentityAndExplicitSourceReceipt) {
    const auto wire = report();
    auto sample = parseEnginePerformance(identity(), wire);
    ASSERT_TRUE(sample) << sample.error().describe();
    EXPECT_EQ(sample->measured.dump(), wire.dump());
    EXPECT_TRUE(sample->measured.find("rendering")->isNull());
    EXPECT_EQ(sample->identity.connectionGeneration, 7);
    ASSERT_EQ(sample->sourceScopes.size(), 1);
    EXPECT_EQ(sample->sourceScopes[0].file, "compiled-source.cpp");
    EXPECT_EQ(sample->sourceScopes[0].line, 37);
    EXPECT_EQ(sample->sourceScopes[0].sourceSha256, std::string(64, 'a'));
    EXPECT_DOUBLE_EQ(sample->sourceScopes[0].msPerFrame, 2.75);
}
TEST(EnginePerformance, OlderOrUnknownSourceContractNeverGuessesFromScopeNames) {
    for (const auto provenance : {"", "future-contract/v2"}) {
        auto wire = report();
        wire.set("profiler", Json::object()
                                 .set("sourceProvenance", provenance)
                                 .set("scopes", Json::array().push(Json::object().set("name", "actualFunction"))));
        auto sample = parseEnginePerformance(identity(), wire);
        ASSERT_TRUE(sample);
        EXPECT_FALSE(sample->sourceProvenanceAvailable);
        EXPECT_TRUE(sample->sourceScopes.empty());
    }
}
TEST(EnginePerformance, SourceAttributionRequiresTheBuildBytesAndRefusesEditedOrMissingReceipts) {
    EngineSourcePerformance scope;
    // FIPS 180-4 SHA256("abc") known-answer oracle, independent of the producer's implementation.
    scope.sourceSha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    EXPECT_TRUE(matchesEnginePerformanceSource(scope, "abc"));
    EXPECT_FALSE(matchesEnginePerformanceSource(scope, "abc\n"));
    EXPECT_FALSE(matchesEnginePerformanceSource(scope, "edited source"));
    scope.sourceSha256.clear();
    EXPECT_FALSE(matchesEnginePerformanceSource(scope, "abc"));
}
TEST(EnginePerformance, RefusesUnauthenticatedMalformedAndUnboundedReports) {
    auto missingIdentity = identity();
    missingIdentity.connectionGeneration = 0;
    EXPECT_FALSE(parseEnginePerformance(missingIdentity, report()));
    EXPECT_FALSE(parseEnginePerformance(identity(), Json::object()));
    auto wire = report();
    wire.set("frame", Json::object().set("frames", 1).set("fps", -1.0));
    EXPECT_FALSE(parseEnginePerformance(identity(), wire));
    wire = report();
    wire.set("oversized", std::string(2U * 1024U * 1024U, 'x'));
    EXPECT_FALSE(parseEnginePerformance(identity(), wire));
    for (const auto metric : {"msPerFrame", "callsPerFrame", "averageMs", "maxMs"}) {
        wire = report();
        auto entry = wire.find("profiler")->find("sourceScopes")->items()[0];
        entry.set(metric, -0.1);
        wire.set("profiler", Json::object()
                                 .set("sourceProvenance", "macro-source-location/v1")
                                 .set("sourceScopes", Json::array().push(entry)));
        EXPECT_FALSE(parseEnginePerformance(identity(), wire)) << metric;
    }
}
} // namespace
} // namespace frost::engine
