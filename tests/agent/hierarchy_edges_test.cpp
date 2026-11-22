// SPDX-License-Identifier: Apache-2.0
// Offline provider callbacks verify actual edge method routing, scope filtering and failure propagation.
#include "frost/agent/ide_queries.hpp"
#include "frost/agent/protocol.hpp"

#include <gtest/gtest.h>

#include <optional>
namespace {
using frost::rpc::Json;
Json item(std::string name, std::string uri = "file:///workspace/source.cpp") {
    auto range = Json::object()
                     .set("start", Json::object().set("line", 1).set("character", 0))
                     .set("end", Json::object().set("line", 1).set("character", 4));
    return Json::object()
        .set("name", std::move(name))
        .set("kind", 12)
        .set("uri", std::move(uri))
        .set("range", range)
        .set("selectionRange", range)
        .set("data", Json::object().set("providerHandle", "opaque-root"));
}
bool admitted(std::string_view uri) { return uri.starts_with("file:///workspace/"); }
TEST(AgentHierarchy, TypeQueriesBothEdgeDirectionsAndRetainsPrivateProviderHandle) {
    std::vector<std::string> calls;
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        true, Json::object(),
        [&](const std::string& method, Json params, const frost::agent::IdeQueryDone& done) {
            calls.push_back(method);
            if (method == "textDocument/prepareTypeHierarchy") {
                done(Json::array().push(item("Root")));
                return;
            }
            ASSERT_NE(params.find("item"), nullptr);
            ASSERT_NE(params.find("item")->find("data"), nullptr);
            EXPECT_EQ(params.find("item")->find("data")->stringAt("providerHandle"), "opaque-root");
            done(Json::array().push(item(method == "typeHierarchy/supertypes" ? "ActualBase" : "ActualDerived")));
        },
        admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    EXPECT_EQ(calls, (std::vector<std::string>{"textDocument/prepareTypeHierarchy", "typeHierarchy/supertypes",
                                               "typeHierarchy/subtypes"}));
    ASSERT_EQ(result->value().size(), 3);
    EXPECT_EQ(result->value().items()[1].stringAt("relation"), "supertype");
    EXPECT_EQ(result->value().items()[1].find("item")->stringAt("name"), "ActualBase");
    EXPECT_EQ(result->value().items()[2].stringAt("relation"), "subtype");
    EXPECT_EQ(result->value().dump().find("providerHandle"), std::string::npos);
}
TEST(AgentHierarchy, CallEdgesKeepActualRangesAndExcludeEscapingCallers) {
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        false, Json::object(),
        [](const std::string& method, const Json&, const frost::agent::IdeQueryDone& done) {
            if (method == "textDocument/prepareCallHierarchy") {
                done(Json::array().push(item("callee")));
                return;
            }
            const auto key = method == "callHierarchy/incomingCalls" ? "from" : "to";
            auto range = *item("fixture").find("range");
            range.set("fixture", "actual-range");
            auto edges =
                Json::array()
                    .push(Json::object().set(key, item("ActualEdge")).set("fromRanges", Json::array().push(range)))
                    .push(Json::object()
                              .set(key, item("Outside", "file:///outside/private.cpp"))
                              .set("fromRanges", Json::array()));
            done(std::move(edges));
        },
        admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    ASSERT_EQ(result->value().size(), 3);
    EXPECT_EQ(result->value().items()[1].stringAt("relation"), "incomingCall");
    EXPECT_EQ(result->value().items()[2].stringAt("relation"), "outgoingCall");
    EXPECT_NE(result->value().dump().find("actual-range"), std::string::npos);
    EXPECT_EQ(result->value().dump().find("Outside"), std::string::npos);
}
TEST(AgentHierarchy, NestedProviderLocationsStayWithinWorkspace) {
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        false, Json::object(),
        [](const std::string& method, const Json&, const frost::agent::IdeQueryDone& done) {
            const auto metadata =
                Json::object()
                    .set("outsideLocation",
                         Json::object().set("location", Json::object().set("uri", "file:///outside/private.cpp")))
                    .set("outsideTarget", Json::object().set("targetUri", "file:///outside/target.cpp"))
                    .set("insideLocation",
                         Json::object().set("location", Json::object().set("uri", "file:///workspace/local.cpp")));
            if (method == "textDocument/prepareCallHierarchy") {
                done(Json::array().push(item("Root").set("metadata", metadata)));
                return;
            }
            const auto key = method == "callHierarchy/incomingCalls" ? "from" : "to";
            auto range = *item("fixture").find("range");
            range.set("metadata", metadata);
            done(
                Json::array().push(Json::object().set(key, item("Edge")).set("fromRanges", Json::array().push(range))));
        },
        admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });

    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    ASSERT_EQ(result->value().size(), 3);
    const auto output = result->value().dump();
    EXPECT_EQ(output.find("file:///outside/"), std::string::npos);
    EXPECT_NE(output.find("file:///workspace/local.cpp"), std::string::npos);
    EXPECT_EQ(result->value().items()[1].stringAt("relation"), "incomingCall");
    EXPECT_EQ(result->value().items()[2].stringAt("relation"), "outgoingCall");
}
TEST(AgentHierarchy, ProviderErrorsDoNotBecomeEmptyOrInventedEdges) {
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        true, Json::object(),
        [](const std::string& method, const Json&, const frost::agent::IdeQueryDone& done) {
            if (method == "textDocument/prepareTypeHierarchy")
                done(Json::array().push(item("Root")));
            else
                done(frost::fail(frost::ErrorCode::Unsupported, "actual provider does not support this edge request"));
        },
        admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), frost::ErrorCode::Unsupported);
}
TEST(AgentHierarchy, EdgeCapsFailExplicitlyAndOrdinaryLargeSetsRemainPageable) {
    for (const int count : {100, 129}) {
        std::optional<frost::Result<Json>> result;
        frost::agent::hierarchyEdges(
            true, Json::object(),
            [count](const std::string& method, const Json&, const frost::agent::IdeQueryDone& done) {
                if (method == "textDocument/prepareTypeHierarchy") {
                    done(Json::array().push(item("Root")));
                    return;
                }
                auto edges = Json::array();
                for (int i = 0; i < count; ++i)
                    edges.push(item("ActualEdge" + std::to_string(i)));
                done(std::move(edges));
            },
            admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
        ASSERT_TRUE(result);
        if (count == 129) {
            ASSERT_FALSE(*result);
            EXPECT_EQ(result->error().code(), frost::ErrorCode::OutOfRange);
        } else {
            ASSERT_TRUE(*result);
            auto pages = frost::agent::paginate(result->value());
            ASSERT_TRUE(pages);
            EXPECT_GT(pages->size(), 1);
        }
    }
}
TEST(AgentHierarchy, EscapingRootIsNeverUsedInAnotherProviderRequest) {
    int requests = 0;
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        false, Json::object(),
        [&](const std::string&, const Json&, const frost::agent::IdeQueryDone& done) {
            ++requests;
            done(Json::array().push(item("Outside", "file:///outside/source.cpp")));
        },
        admitted, [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    EXPECT_TRUE(result->value().items().empty());
    EXPECT_EQ(requests, 1);
}
TEST(AgentHierarchy, RevocationBetweenProviderRepliesPreventsFurtherExpansion) {
    bool live = true;
    int requests = 0;
    std::optional<frost::Result<Json>> result;
    frost::agent::hierarchyEdges(
        true, Json::object(),
        [&](const std::string& method, const Json&, const frost::agent::IdeQueryDone& done) {
            ++requests;
            if (method == "textDocument/prepareTypeHierarchy")
                done(Json::array().push(item("Root")));
            else {
                live = false;
                done(Json::array().push(item("ActualEdge")));
            }
        },
        [&](std::string_view uri) { return live && admitted(uri); },
        [&](frost::Result<Json> value) { result.emplace(std::move(value)); });
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), frost::ErrorCode::PermissionDenied);
    EXPECT_EQ(requests, 2);
}
} // namespace
