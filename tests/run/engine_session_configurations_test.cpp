// SPDX-License-Identifier: Apache-2.0
// FER-11 generated CLI and ordinary compound membership preserve selected tools and argument tokens.
#include "frost/io/path.hpp"
#include "frost/run/engine_session_configurations.hpp"

#include <gtest/gtest.h>
namespace frost::run {
namespace {
std::string absolute(std::string_view leaf) {
    return std::string(io::kNativePathStyle == io::PathStyle::Windows ? "C:/installed engine/" : "/installed engine/") +
           std::string(leaf);
}
TEST(EngineSessionConfigurations, ExportAndServerTwoClientCompoundUseActualCliContracts) {
    auto configs = engineSessionRunConfigurations("selected", "Debug", absolute("FrostEditor"),
                                                  absolute("FrostRuntime"), absolute("game/My Game.frostproj"), 7778);
    ASSERT_TRUE(configs) << configs.error().describe();
    ASSERT_EQ(configs->size(), 5);
    EXPECT_EQ((*configs)[0].arguments,
              (std::vector<std::string>{"build.export", "--project", absolute("game/My Game.frostproj")}));
    EXPECT_EQ((*configs)[1].arguments, (std::vector<std::string>{"--project", absolute("game/My Game.frostproj"),
                                                                 "--server", "--port", "7778"}));
    for (unsigned i = 2; i <= 3; ++i) {
        EXPECT_EQ((*configs)[i].program, absolute("FrostRuntime"));
        EXPECT_EQ((*configs)[i].arguments[2], "--connect");
        EXPECT_EQ((*configs)[i].arguments[3], "127.0.0.1:7778");
        EXPECT_EQ((*configs)[i].arguments[5], "FrostIDE client " + std::to_string(i - 1));
    }
    EXPECT_EQ(configs->back().type, RunType::Compound);
    EXPECT_EQ(configs->back().configurations,
              (std::vector<std::string>{(*configs)[1].id, (*configs)[2].id, (*configs)[3].id}));
    auto stored = parseRunConfigurations(serializeRunConfigurations(*configs));
    ASSERT_TRUE(stored);
    EXPECT_EQ(*stored, *configs);
}
TEST(EngineSessionConfigurations, MissingRuntimeKeepsExportAndRelativePathsOrZeroPortRefuse) {
    auto configs = engineSessionRunConfigurations("selected", "Debug", absolute("FrostEditor"), "", absolute("Game"));
    ASSERT_TRUE(configs);
    ASSERT_EQ(configs->size(), 1);
    EXPECT_FALSE(engineSessionRunConfigurations("selected", "Debug", "FrostEditor", "", absolute("Game")));
    EXPECT_FALSE(engineSessionRunConfigurations("selected", "Debug", absolute("FrostEditor"), "", "Game"));
    EXPECT_FALSE(engineSessionRunConfigurations("selected", "Debug", absolute("FrostEditor"), absolute("Runtime"),
                                                absolute("Game"), 0));
}
} // namespace
} // namespace frost::run
