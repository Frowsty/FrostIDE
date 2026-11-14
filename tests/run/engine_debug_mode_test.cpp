// SPDX-License-Identifier: Apache-2.0
// Explicit R7 native-and-Lua launch intent round-trips without changing legacy run-store formats.
#include "frost/io/path.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"

#include <gtest/gtest.h>
namespace frost::run {
namespace {
RunConfiguration config() {
    RunConfiguration c;
    c.id = "engine.mixed";
    c.name = "Engine mixed";
    c.type = RunType::FrostEditor;
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/mixed" : "/mixed";
    c.program = io::normalizePath(root + "/FrostEditor.exe").value_or("");
    c.engineProjectPath = io::normalizePath(root + "/Game.frostproj").value_or("");
    return c;
}
TEST(EngineDebugMode, ExplicitMixedIntentRequiresVersionFourWhileLuaKeepsVersionThree) {
    auto c = config();
    auto lua = rpc::parseJson(serializeRunConfigurations(std::vector<RunConfiguration>{c}));
    ASSERT_TRUE(lua);
    EXPECT_EQ(lua->integerAt("version"), 3);
    c.engineDebugMode = EngineDebugMode::NativeAndLua;
    auto text = serializeRunConfigurations(std::vector<RunConfiguration>{c});
    auto mixed = rpc::parseJson(text);
    ASSERT_TRUE(mixed);
    EXPECT_EQ(mixed->integerAt("version"), 4);
    auto downgraded = *mixed;
    downgraded.set("version", 3);
    EXPECT_EQ(parseRunConfigurations(downgraded.dump()).error().code(), ErrorCode::Corrupt);
    auto parsed = parseRunConfigurations(text);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ(parsed->front(), c);
    EXPECT_EQ(parsed->front().engineDebugMode, EngineDebugMode::NativeAndLua);
}
TEST(EngineDebugMode, InvalidOrNonEngineDebugModeIsRefusedAndFutureVersionStillUnsupported) {
    auto c = config();
    c.type = RunType::Native;
    c.engineProjectPath.clear();
    c.engineDebugMode = EngineDebugMode::NativeAndLua;
    EXPECT_FALSE(validateRunConfiguration(c));
    c = config();
    c.engineDebugMode = static_cast<EngineDebugMode>(255);
    EXPECT_FALSE(validateRunConfiguration(c));
    EXPECT_EQ(parseRunConfigurations(R"({"version":5,"configurations":[]})").error().code(), ErrorCode::Unsupported);
    auto mixed = rpc::parseJson(serializeRunConfigurations(std::vector<RunConfiguration>{config()}));
    ASSERT_TRUE(mixed);
    auto entries = mixed->find("configurations")->items();
    entries.front().set("engineDebugMode", "unknown");
    mixed->set("configurations", rpc::Json::array(entries));
    EXPECT_EQ(parseRunConfigurations(mixed->dump()).error().code(), ErrorCode::Corrupt);
}
} // namespace
} // namespace frost::run
