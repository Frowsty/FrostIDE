// SPDX-License-Identifier: Apache-2.0
// Authored real-framework provider composition; no execution/acceptance claim.
#include "frost/ui/test_results_tree.hpp"

#include "lua_unit_tests_window.hpp"
#include "unit_tests_window.hpp"
#include "workbench_rig.hpp"
#include <QComboBox>
#include <QPushButton>
#include <QTabWidget>

#include <cstdlib>
namespace {
using namespace frost;
TEST(LuaUnitTestsWindow, ActualDiscoveryFeedsTreeGutterAndSameInterpreterSelectedDebug) {
    ASSERT_TRUE(std::getenv("FROST_LUA_TEST_PATH")) << "provision supported LuaUnit 3.5";
    ASSERT_TRUE(io::isAbsolutePath(FROST_TEST_LUA_INTERPRETER));
    app::test::WorkDir dir("lua-unit-tests-ui");
    auto& fs = io::realFileSystem();
    auto trust = project::TrustStore::open(fs, dir.join("trust.txt"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trustForSession(dir.path()));
    run::RunConfiguration config;
    config.id = "luaunit";
    config.name = "LuaUnit driver";
    config.type = run::RunType::LuaScript;
    config.program = FROST_TEST_LUA_INTERPRETER;
    config.target = std::string(FROST_LUA_FRAMEWORK_FIXTURES) + "/luaunit_driver.lua";
    config.workingDirectory = dir.path();
    config.environment = {{"LUA_PATH", std::getenv("FROST_LUA_TEST_PATH")}};
    std::stop_source lifetime;
    std::optional<run::RunConfiguration> debugged;
    app::LuaUnitTestsContext context;
    context.fs = &fs;
    context.launcher = &io::realProcessLauncher();
    context.trust = trust->get();
    context.scratchDirectory = dir.join("reports");
    context.discoveryBridge = FROST_LUA_DISCOVERY_BRIDGE;
    context.source = [&]() -> Result<app::LuaUnitTestsSource> {
        return app::LuaUnitTestsSource{config, dir.path(), (*trust)->snapshot(dir.path()).revision,
                                       lifetime.get_token()};
    };
    context.debug = [&](auto selected) -> Result<void> {
        debugged = std::move(selected);
        return {};
    };
    QTabWidget tabs;
    auto* widget = app::createLuaUnitTestsWidget(&tabs, context);
    tabs.addTab(widget, QStringLiteral("Lua"));
    auto* profile = widget->findChild<QComboBox*>(QStringLiteral("luaTestProfile"));
    ASSERT_NE(profile, nullptr);
    profile->setCurrentIndex(1);
    QPushButton* discover{};
    for (auto* b : widget->findChildren<QPushButton*>())
        if (b->text() == QStringLiteral("Discover"))
            discover = b;
    ASSERT_NE(discover, nullptr);
    discover->click();
    auto* tree = widget->findChild<ui::TestResultsTree*>();
    ASSERT_NE(tree, nullptr);
    ASSERT_TRUE(app::test::waitFor([&] { return tree->frameworkCases(config.name) != nullptr; }));
    const auto* listed = tree->frameworkCases(config.name);
    ASSERT_NE(listed, nullptr);
    ASSERT_FALSE(listed->empty());
    auto cases = app::unitTestsGutterCasesForFile(&tabs, config.target, {});
    ASSERT_FALSE(cases.empty());
    auto found = std::ranges::find(cases, std::string("TestSelection.testPass"), &run::GutterTestCase::caseName);
    ASSERT_NE(found, cases.end());
    ASSERT_TRUE(app::debugGutterTestCase(&tabs, *found));
    ASSERT_TRUE(debugged);
    EXPECT_EQ(debugged->program, config.program);
    EXPECT_EQ(debugged->target, config.target);
    EXPECT_EQ(debugged->environment, config.environment);
    EXPECT_NE(std::ranges::find(debugged->arguments, std::string("^TestSelection%.testPass$")),
              debugged->arguments.end());
    // No object-name impersonation; two actual providers are ambiguous, not arbitrarily routed.
    auto* duplicate = app::createLuaUnitTestsWidget(&tabs, context);
    tabs.addTab(duplicate, QStringLiteral("Lua duplicate"));
    EXPECT_FALSE(app::debugGutterTestCase(&tabs, *found));
    tabs.removeTab(1);
    delete duplicate;
    lifetime.request_stop();
    EXPECT_TRUE(app::unitTestsGutterCasesForFile(&tabs, config.target, {}).empty());
    EXPECT_FALSE(app::runGutterTestCase(&tabs, *found));
}
} // namespace
