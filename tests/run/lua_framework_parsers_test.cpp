// SPDX-License-Identifier: Apache-2.0
// LUA-12 protocol contracts. Authored sources; no framework execution is implied.
#include "frost/run/lua_framework_parsers.hpp"
#include "frost/run/run_manager.hpp"

#include <gtest/gtest.h>

#include <algorithm>
using namespace frost;
using namespace frost::run;
namespace {
RunConfiguration config() {
    RunConfiguration c;
    c.id = "lua-tests";
    c.name = "Lua tests";
    c.type = RunType::LuaScript;
#ifdef _WIN32
    c.program = "C:/sdk/lua.exe";
    c.target = "C:/project/test.lua";
#else
    c.program = "/sdk/lua";
    c.target = "/project/test.lua";
#endif
    c.interpreterArguments = {"-E", "--"};
    c.environment = {{"LUA_PATH", "custom/?.lua"}};
    return c;
}
std::string absolute(std::string leaf) {
#ifdef _WIN32
    return "C:/scratch/" + leaf;
#else
    return "/scratch/" + leaf;
#endif
}
std::string junit(std::string cases = R"(<testcase classname="[TestFunctions]" name="testOne" time="0.001"/>)") {
    return std::string(
               R"(<testsuites><testsuite tests="1" errors="0" failures="0" skipped="0"><properties><property name="LuaUnit Version" value="3.5"/></properties>)") +
           cases + "</testsuite></testsuites>";
}
TEST(LuaFramework, PlannerKeepsActualInterpreterArgumentsAndModuleEnvironment) {
    auto c = config();
    auto planned =
        luaFrameworkConfiguration(LuaTestProfile::LuaUnit35, c, {}, absolute("list.xml"), true, absolute("bridge.lua"));
    ASSERT_TRUE(planned);
    EXPECT_EQ(planned->program, c.program);
    EXPECT_EQ(planned->target, c.target);
    EXPECT_EQ(planned->environment, c.environment);
    EXPECT_EQ(planned->interpreterArguments.back(), "--");
    auto launch = launchPlanFor(*planned);
    ASSERT_TRUE(launch);
    EXPECT_EQ(launch->program, c.program);
    EXPECT_NE(std::ranges::find(launch->arguments, c.target), launch->arguments.end());
}
TEST(LuaFramework, BustedSelectionEscapesLuaPatternsAndDisablesRuntimeSwitch) {
    auto planned =
        luaFrameworkConfiguration(LuaTestProfile::Busted220, config(), std::vector<std::string>{"suite a.[x]%-?"},
                                  absolute("run.xml"), false, {});
    ASSERT_TRUE(planned);
    EXPECT_NE(std::ranges::find(planned->arguments, "--ignore-lua"), planned->arguments.end());
    EXPECT_NE(std::ranges::find(planned->arguments, "--filter=^suite a%.%[x%]%%%-%?$"), planned->arguments.end());
    auto c = config();
    c.arguments = {"--lua=other"};
    EXPECT_FALSE(luaFrameworkConfiguration(LuaTestProfile::Busted220, c, {}, absolute("run.xml"), false, {}));
    EXPECT_FALSE(
        luaFrameworkConfiguration(LuaTestProfile::Busted220, config(), {}, absolute("has,comma.xml"), false, {}));
}
TEST(LuaFramework, CatalogueMustProveSupportedHookAndNeverCarriesVerdicts) {
    EXPECT_FALSE(parseLuaFrameworkList(LuaTestProfile::LuaUnit35, "<LuaUnitDiscovery/>"));
    auto list = parseLuaFrameworkList(
        LuaTestProfile::LuaUnit35,
        R"(<LuaUnitDiscovery version="3.5" hook="internalRunSuiteByInstances"><TestCase name="TestClass.testOne" file="tests.lua" line="12"/></LuaUnitDiscovery>)");
    ASSERT_TRUE(list);
    ASSERT_EQ(list->size(), 1u);
    EXPECT_EQ(list->front().name, "TestClass.testOne");
    ASSERT_TRUE(list->front().location);
    EXPECT_EQ(list->front().location->line, 12);
    EXPECT_FALSE(parseLuaFrameworkList(LuaTestProfile::Busted220, "some diagnostic text"));
    auto busted = parseLuaFrameworkList(LuaTestProfile::Busted220, "spec/a.lua:7: group name: with colon\n");
    ASSERT_TRUE(busted);
    EXPECT_EQ(busted->front().name, "group name: with colon");
}
TEST(LuaFramework, ResultsRequireCompleteCounterSelectionAndExitAgreement) {
    auto report = parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", junit(),
                                       std::vector<std::string>{"testOne"}, 0, "real output");
    ASSERT_TRUE(report);
    ASSERT_EQ(report->cases.size(), 1u);
    EXPECT_EQ(report->cases[0].outcome, TestOutcome::Passed);
    EXPECT_FALSE(parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", junit(),
                                      std::vector<std::string>{"testOther"}, 0, {}));
    EXPECT_FALSE(parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", junit(), {}, 1, {}));
    EXPECT_FALSE(parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", junit().substr(0, 60), {}, 0, {}));
    auto bad = junit();
    bad.replace(bad.find("tests=\"1\""), 9, "tests=\"2\"");
    EXPECT_FALSE(parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", bad, {}, 0, {}));
}
TEST(LuaFramework, GenuineSkippedAndFailureNodesArePreserved) {
    auto xml = junit(
        R"(<testcase classname="[TestFunctions]" name="testOne" time="0"><failure type="assertion">tests.lua:9: expected value</failure></testcase>)");
    xml.replace(xml.find("failures=\"0\""), 12, "failures=\"1\"");
    auto report = parseLuaFrameworkRun(LuaTestProfile::LuaUnit35, "driver", xml, {}, 1, {});
    ASSERT_TRUE(report);
    EXPECT_EQ(report->cases[0].outcome, TestOutcome::Failed);
    EXPECT_FALSE(report->cases[0].failures.empty());
}
} // namespace
