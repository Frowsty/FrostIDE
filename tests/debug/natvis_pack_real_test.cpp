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
    auto instances = build::listVsInstances(io::realProcessLauncher(), io::realFileSystem());
    ASSERT_TRUE(instances) << instances.error().describe();
    const std::string pinned = FROST_PINNED_VC_TOOLS;
    const auto install = std::ranges::find_if(*instances, [&](const build::VsInstance& instance) {
        return io::realFileSystem()
            .exists(instance.installationPath + "\\VC\\Tools\\MSVC\\" + pinned + "\\include")
            .value_or(false);
    });
    ASSERT_NE(install, instances->end()) << "no Visual Studio instance has the pinned MSVC toolset " << pinned;
    const auto stl = io::joinPath(install->installationPath, "Common7/Packages/Debugger/Visualizers/stl.natvis");
    ASSERT_TRUE(stl) << stl.error().describe();
    ASSERT_TRUE(io::realFileSystem().exists(*stl).value_or(false)) << *stl;
    const auto forwarded = scratch.join("forwarded.natvis");
    ASSERT_TRUE(io::realFileSystem().writeFileAtomic(forwarded, R"natvis(
<AutoVisualizer xmlns="http://schemas.microsoft.com/vstudio/debugger/natvis/2010">
  <Type Name="ForwardedArray"><Expand><ExpandedItem>values</ExpandedItem></Expand></Type>
</AutoVisualizer>)natvis"));
    target.natvisFiles = {FROST_VISUALIZER_PACK, *stl, forwarded};
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
    const auto evaluate = [&](std::string_view expression) {
        return peer.request("evaluate", Json::object().set("expression", expression).set("frameId", *frame));
    };
    for (const auto* expression : {"numbers", "forwardedArray"}) {
        SCOPED_TRACE(expression);
        const auto value = evaluate(expression);
        ASSERT_TRUE(value);
        const auto reference = value->integerAt("variablesReference");
        ASSERT_TRUE(reference);
        const auto page = peer.request("variables", Json::object()
                                                        .set("variablesReference", *reference)
                                                        .set("filter", "indexed")
                                                        .set("start", 1)
                                                        .set("count", 1));
        ASSERT_TRUE(page);
        ASSERT_NE(page->find("variables"), nullptr);
        ASSERT_EQ(page->find("variables")->items().size(), 1U);
        EXPECT_EQ(page->find("variables")->items().front().stringAt("name"), "[1]");
        EXPECT_EQ(page->find("variables")->items().front().stringAt("value"), "5");
    }

    const auto duration = evaluate("duration");
    ASSERT_TRUE(duration);
    const auto durationReference = duration->integerAt("variablesReference");
    ASSERT_TRUE(durationReference);
    const auto durationChildren =
        peer.request("variables", Json::object().set("variablesReference", *durationReference));
    ASSERT_TRUE(durationChildren);
    ASSERT_NE(durationChildren->find("variables"), nullptr);
    EXPECT_TRUE(durationChildren->find("variables")->items().empty());
    const auto rawDuration = peer.request("variables", Json::object()
                                                           .set("variablesReference", *durationReference)
                                                           .set("format", Json::object().set("frostRaw", true)));
    ASSERT_TRUE(rawDuration);
    ASSERT_NE(rawDuration->find("variables"), nullptr);
    EXPECT_FALSE(rawDuration->find("variables")->items().empty());

    const auto tuple = evaluate("mixedTuple");
    ASSERT_TRUE(tuple);
    const auto tupleReference = tuple->integerAt("variablesReference");
    ASSERT_TRUE(tupleReference);
    const auto tupleChildren = peer.request("variables", Json::object().set("variablesReference", *tupleReference));
    ASSERT_TRUE(tupleChildren);
    ASSERT_NE(tupleChildren->find("variables"), nullptr);
    const auto& elements = tupleChildren->find("variables")->items();
    ASSERT_EQ(elements.size(), 2U);
    EXPECT_EQ(elements[0].stringAt("name"), "[0]");
    EXPECT_EQ(elements[0].stringAt("value"), "7");
    EXPECT_EQ(elements[1].stringAt("name"), "[1]");
    const auto nativePointer = evaluate("tuplePointer");
    ASSERT_TRUE(nativePointer);
    EXPECT_EQ(elements[1].stringAt("value"), nativePointer->stringAt("result"));

    const auto plain = evaluate("plain");
    ASSERT_TRUE(plain);
    const auto plainReference = plain->integerAt("variablesReference");
    ASSERT_TRUE(plainReference);
    const auto plainChildren = peer.request("variables", Json::object().set("variablesReference", *plainReference));
    ASSERT_TRUE(plainChildren);
    ASSERT_NE(plainChildren->find("variables"), nullptr);
    ASSERT_EQ(plainChildren->find("variables")->items().size(), 1U);
    EXPECT_EQ(plainChildren->find("variables")->items().front().stringAt("name"), "field");
    EXPECT_EQ(plainChildren->find("variables")->items().front().stringAt("value"), "11");

    ASSERT_TRUE(peer.request("disconnect", Json::object().set("terminateDebuggee", true)));
}
} // namespace
} // namespace frost::debug
