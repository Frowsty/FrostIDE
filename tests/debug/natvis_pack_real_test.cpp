// SPDX-License-Identifier: Apache-2.0
// Real standalone MSVC image: Natvis projected children must equal independently read native fields.
#include "dbgeng_native_peer.hpp"
#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <map>

namespace frost::debug {
namespace {
using native_test::Peer;
using Json = rpc::Json;
using namespace std::chrono_literals;
TEST(NatvisPackReal, OriginalPackProjectsNativeFieldsAndRawViewDoesNotUseTheProjection) {
    frost::test::ScratchDir scratch("natvis-pack-oracle");
    auto program = native_test::buildNativeFixture(FROST_VISUALIZER_SOURCE, scratch);
    ASSERT_TRUE(program) << program.error().describe();
    Peer peer;
    ASSERT_TRUE(peer.start(FROST_DBGENG_ADAPTER_PATH));
    DbgEngTarget target;
    target.native.program = *program;
    target.natvisFiles = {FROST_VISUALIZER_PACK};
    auto arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments);
    ASSERT_TRUE(peer.request("launch", *arguments)) << peer.text();
    auto points = peer.request(
        "setFunctionBreakpoints",
        Json::object().set("breakpoints", Json::array().push(Json::object().set("name", "visualizer_breakpoint"))));
    ASSERT_TRUE(points) << peer.text();
    ASSERT_NE(points->find("breakpoints"), nullptr);
    ASSERT_EQ(points->find("breakpoints")->items().front().boolAt("verified"), true);
    ASSERT_TRUE(peer.request("configurationDone"));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("stopped") > 0; }, 60s)) << peer.text();
    const auto capabilities = peer.last("capabilities");
    ASSERT_TRUE(capabilities);
    ASSERT_NE(capabilities->find("capabilities"), nullptr);
    // Raw fallback cannot satisfy this feature oracle: an unavailable ModelStore is a real failure.
    ASSERT_EQ(capabilities->find("capabilities")->boolAt("frostStructuredNatvis"), true) << peer.text();
    ASSERT_GT(peer.count("frost.visualizersLoaded"), 0U);
    auto trace =
        peer.request("stackTrace", Json::object().set("threadId", *peer.last("stopped")->integerAt("threadId")));
    ASSERT_TRUE(trace);
    ASSERT_NE(trace->find("stackFrames"), nullptr);
    const auto frame = trace->find("stackFrames")->items().front().integerAt("id");
    ASSERT_TRUE(frame);
    for (const auto& name : {"point", "color", "vector", "rotation"}) {
        auto value = peer.request("evaluate", Json::object().set("expression", name).set("frameId", *frame));
        ASSERT_TRUE(value) << peer.text();
        const auto reference = value->integerAt("variablesReference");
        ASSERT_TRUE(reference);
        ASSERT_GT(*reference, 0);
        auto projected = peer.request("variables", Json::object().set("variablesReference", *reference));
        ASSERT_TRUE(projected);
        ASSERT_NE(projected->find("variables"), nullptr);
        std::map<std::string, std::string> fields;
        for (const auto& child : projected->find("variables")->items())
            fields.emplace(child.stringAt("name"), child.stringAt("value"));
        ASSERT_TRUE(fields.contains("X")) << projected->dump();
        for (const auto& component : {"x", "y"}) {
            auto native = peer.request(
                "evaluate",
                Json::object().set("expression", std::string(name) + "." + component).set("frameId", *frame));
            ASSERT_TRUE(native) << peer.text();
            const std::string label = component[0] == 'x' ? "X" : "Y";
            ASSERT_TRUE(fields.contains(label));
            EXPECT_EQ(fields.at(label), native->stringAt("result"));
        }
        auto raw = peer.request(
            "variables",
            Json::object().set("variablesReference", *reference).set("format", Json::object().set("frostRaw", true)));
        ASSERT_TRUE(raw);
        ASSERT_NE(raw->find("variables"), nullptr);
        EXPECT_TRUE(std::ranges::any_of(raw->find("variables")->items(), [](const auto& child) {
            return child.stringAt("name") == "x";
        })) << raw->dump();
        EXPECT_FALSE(std::ranges::any_of(raw->find("variables")->items(), [](const auto& child) {
            return child.stringAt("name") == "X";
        })) << raw->dump();
    }
    ASSERT_TRUE(peer.request("disconnect", Json::object().set("terminateDebuggee", true)));
}
} // namespace
} // namespace frost::debug
