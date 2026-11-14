// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/semantic_tokens.hpp"

#include "lang/feature_server.hpp"
#include <gtest/gtest.h>
namespace {
using namespace frost;
using namespace frost::lang;
TEST(InlayProtocol, StandardProviderWinsWhenBothProvidersAreAnnounced) {
    auto capabilities = test::FeatureServer::defaultCapabilities();
    capabilities.set("inlayHintProvider", true).set("clangdInlayHintsProvider", true);
    test::FeatureServer server(capabilities);
    server.on("textDocument/inlayHint", [](const Json&) {
        return Json::array({Json::object()
                                .set("position", Json::object().set("line", 0).set("character", 4))
                                .set("kind", 2)
                                .set("label", Json::array({Json::object().set("value", "count:")}))});
    });
    auto hints = server.run(requestInlayHints(server.client(), test::workPath("real.cpp"), {{0, 0}, {0, 7}}));
    ASSERT_TRUE(hints);
    ASSERT_EQ(hints->size(), 1U);
    EXPECT_EQ(hints->front().label, "count:");
    EXPECT_EQ(server.paramsOf("textDocument/inlayHint").size(), 1U);
    EXPECT_TRUE(server.paramsOf("clangd/inlayHints").empty());
}
TEST(InlayProtocol, LegacyProviderUsesOnlyItsAdvertisedMethod) {
    auto capabilities = test::FeatureServer::defaultCapabilities();
    capabilities.set("clangdInlayHintsProvider", true);
    test::FeatureServer server(capabilities);
    server.on("clangd/inlayHints", [](const Json&) {
        return Json::array({Json::object()
                                .set("range", Json::object()
                                                  .set("start", Json::object().set("line", 0).set("character", 4))
                                                  .set("end", Json::object().set("line", 0).set("character", 4)))
                                .set("kind", "type")
                                .set("label", ": int")});
    });
    auto hints = server.run(requestInlayHints(server.client(), test::workPath("real.cpp"), {{0, 0}, {0, 7}}));
    ASSERT_TRUE(hints);
    ASSERT_EQ(hints->size(), 1U);
    EXPECT_EQ(hints->front().kind, LspInlayHintKind::Type);
    EXPECT_TRUE(server.paramsOf("textDocument/inlayHint").empty());
    EXPECT_EQ(server.paramsOf("clangd/inlayHints").size(), 1U);
}
TEST(InlayProtocol, FailureAndCancellationNeverSendAnUnadvertisedFallback) {
    auto capabilities = test::FeatureServer::defaultCapabilities();
    capabilities.set("inlayHintProvider", true);
    test::FeatureServer server(capabilities);
    server.on("textDocument/inlayHint", [](const Json&) { return Json::object().set("__error", "server failed"); });
    auto failed = server.run(requestInlayHints(server.client(), test::workPath("real.cpp"), {{0, 0}, {0, 7}}));
    EXPECT_FALSE(failed);
    EXPECT_TRUE(server.paramsOf("clangd/inlayHints").empty());
    std::stop_source stop;
    stop.request_stop();
    auto cancelled =
        server.run(requestInlayHints(server.client(), test::workPath("real.cpp"), {{0, 0}, {0, 7}}, stop.get_token()));
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(server.paramsOf("textDocument/inlayHint").size(), 1U);
}
} // namespace
