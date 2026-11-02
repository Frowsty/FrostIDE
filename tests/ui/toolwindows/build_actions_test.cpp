// SPDX-License-Identifier: Apache-2.0
//
// Tests for the build toolbar pieces (frost/ui/toolwindows/build_actions.hpp): the default-shortcut
// table, the actions' enabled state through a build, their registry entries reached through the
// keymap service's matcher, and the configuration combo. Offscreen; no window is shown.
#include "frost/input/keymap_service.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QAction>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using build::BuildAction;
using build::BuildState;
using test::FakeBuildBackend;
using test::loadSdk;
using test::runPostedEvents;

input::Sequence sequence(std::string_view text) {
    auto parsed = input::parseSequence(text, input::SyntaxMode::Strict);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(input::Sequence{});
}

input::KeyEvent keyPress(input::Code code, input::Modifiers modifiers) {
    input::KeyEvent event;
    event.type = input::KeyEventType::Press;
    event.code = code;
    event.modifiers = modifiers;
    event.osTimeMs = 1000;
    return event;
}

// Puts the fake service into `state` the way the real one reports it.
void setState(BuildController& controller, FakeBuildBackend& backend, BuildState state) {
    backend.currentState = state;
    controller.serviceCallbacks().onStateChanged(state);
    runPostedEvents();
}

TEST(BuildShortcuts, TheTableHasOneBuildSolutionEntryAndParses) {
    EXPECT_EQ(
        std::ranges::count(kBuildShortcutDefaults, build_actions::kBuildSolution, &BuildShortcutDefault::actionId), 1);
    auto bindings = buildShortcutBindings();
    ASSERT_TRUE(bindings.has_value()) << bindings.error().message();
    ASSERT_EQ(bindings->size(), 2U); // Rebuild and Clean have no default.
    EXPECT_EQ((*bindings)[0].actionId, build_actions::kBuildSolution);
    EXPECT_EQ((*bindings)[0].sequences, std::vector<input::Sequence>{sequence("Ctrl+Shift+B")});
    EXPECT_EQ((*bindings)[1].actionId, build_actions::kCancelBuild);
    EXPECT_EQ((*bindings)[1].sequences, std::vector<input::Sequence>{sequence("Ctrl+Pause")});
}

TEST(BuildActions, EnableAndDisableWithTheBuildState) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    BuildActions actions(controller);
    // No solution: nothing to build or cancel.
    EXPECT_FALSE(actions.buildSolution()->isEnabled());
    EXPECT_FALSE(actions.rebuildSolution()->isEnabled());
    EXPECT_FALSE(actions.cleanSolution()->isEnabled());
    EXPECT_FALSE(actions.cancelBuild()->isEnabled());
    EXPECT_FALSE(actions.run(build_actions::kBuildSolution));

    ASSERT_TRUE(controller.openSolution(loadSdk()).has_value());
    EXPECT_TRUE(actions.buildSolution()->isEnabled());
    EXPECT_TRUE(actions.rebuildSolution()->isEnabled());
    EXPECT_TRUE(actions.cleanSolution()->isEnabled());
    EXPECT_FALSE(actions.cancelBuild()->isEnabled());

    actions.rebuildSolution()->trigger();
    EXPECT_EQ(backend.builds, (std::vector<FakeBuildBackend::BuildCall>{{BuildAction::Rebuild, std::nullopt}}));

    // While a build runs (or waits for trust) only Cancel is enabled.
    for (const BuildState state : {BuildState::NeedsTrust, BuildState::SavingDocuments, BuildState::Running}) {
        setState(controller, backend, state);
        EXPECT_FALSE(actions.buildSolution()->isEnabled());
        EXPECT_FALSE(actions.cleanSolution()->isEnabled());
        EXPECT_TRUE(actions.cancelBuild()->isEnabled());
    }
    actions.cancelBuild()->trigger();
    EXPECT_EQ(backend.cancels, 1);
    setState(controller, backend, BuildState::Idle);
    EXPECT_TRUE(actions.buildSolution()->isEnabled());
    EXPECT_FALSE(actions.cancelBuild()->isEnabled());

    // A refused build is reported, not thrown away.
    test::SignalLog failures(&controller, &BuildController::requestFailed);
    backend.buildError = Error(ErrorCode::Busy, "a build is already running");
    actions.buildSolution()->trigger();
    ASSERT_EQ(failures.count(), 1);
    EXPECT_TRUE(failures.at(0).at(0).toString().contains(QStringLiteral("already running")));

    ASSERT_TRUE(controller.closeSolution().has_value());
    EXPECT_FALSE(actions.buildSolution()->isEnabled());
}

TEST(BuildActions, RegisterUnderBuildAndFireFromTheirShortcuts) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    BuildActions actions(controller);

    input::ActionRegistry registry;
    // IntelliJ's Ctrl+Shift+B action, as the editor would register it.
    int typeDeclarations = 0;
    input::ActionSpec typeDeclaration;
    typeDeclaration.id = "navigate.typeDeclaration";
    typeDeclaration.name = "Type Declaration";
    typeDeclaration.handler = [&typeDeclarations](const input::ActionContext&, const input::InvokeInfo&) {
        ++typeDeclarations;
    };
    ASSERT_TRUE(registry.add(std::move(typeDeclaration)).has_value());
    ASSERT_TRUE(actions.registerActions(registry).has_value());
    EXPECT_FALSE(actions.registerActions(registry).has_value()); // Ids are taken now.

    for (const std::string_view id : {build_actions::kBuildSolution, build_actions::kRebuildSolution,
                                      build_actions::kCleanSolution, build_actions::kCancelBuild}) {
        const auto action = registry.find(id);
        ASSERT_TRUE(action.has_value()) << id;
        EXPECT_EQ(registry.info(*action).group, "Build");
        EXPECT_FALSE(registry.isEnabled(*action, input::ActionContext{})) << id; // No solution yet.
    }

    input::MemoryKeymapSettings settings;
    auto service = input::KeymapService::create(registry, settings, input::KeymapPlatform::Windows,
                                                input::simulatedLayout(input::SimulatedLayoutId::EnUs));
    ASSERT_TRUE(service.has_value());
    auto defaults = buildShortcutBindings();
    ASSERT_TRUE(defaults.has_value());
    service->setModuleDefaults(std::move(*defaults));
    const input::ActionContext context{
        .keys = input::ContextKeys::all(), .level = input::AvailabilityLevel::L4, .trusted = true};
    const auto fire = [&](input::Code code, input::Modifiers modifiers) {
        const input::MatchResult result = service->handle(keyPress(code, modifiers), context);
        if (result.outcome == input::MatchOutcome::Fire) {
            static_cast<void>(registry.tryInvoke(result.action, context));
            return registry.info(result.action).id;
        }
        return std::string();
    };
    const input::Modifiers ctrlShift = input::Modifier::Ctrl | input::Modifier::Shift;

    // Without a solution Ctrl+Shift+B falls through to Go to Type Declaration...
    EXPECT_EQ(fire(input::Code::KeyB, ctrlShift), "navigate.typeDeclaration");
    EXPECT_EQ(typeDeclarations, 1);
    // ...and with one it builds (priority 1 wins the shared shortcut).
    ASSERT_TRUE(controller.openSolution(loadSdk()).has_value());
    EXPECT_EQ(fire(input::Code::KeyB, ctrlShift), build_actions::kBuildSolution);
    EXPECT_EQ(backend.builds, (std::vector<FakeBuildBackend::BuildCall>{{BuildAction::Build, std::nullopt}}));
    setState(controller, backend, BuildState::Running);
    EXPECT_EQ(fire(input::Code::Pause, input::Modifier::Ctrl), build_actions::kCancelBuild); // Ctrl+Break.
    EXPECT_EQ(backend.cancels, 1);
}

TEST(ConfigurationCombo, ListsAndSwitchesConfigurations) {
    FakeBuildBackend backend;
    BuildController controller;
    controller.setBackend(&backend);
    ConfigurationCombo combo(controller);
    EXPECT_EQ(combo.count(), 0);
    EXPECT_FALSE(combo.isEnabled());

    ASSERT_TRUE(controller.openSolution(loadSdk()).has_value());
    ASSERT_GE(combo.count(), 2);
    ASSERT_EQ(static_cast<std::size_t>(combo.count()), controller.configurations().size());
    for (int i = 0; i < combo.count(); ++i) {
        EXPECT_EQ(combo.itemText(i), configurationLabel(controller.configurations()[static_cast<std::size_t>(i)]));
        EXPECT_TRUE(combo.itemText(i).contains(QStringLiteral(" | ")));
    }
    EXPECT_EQ(combo.currentIndex(), 0);
    EXPECT_TRUE(combo.isEnabled());

    combo.setCurrentIndex(1); // As a pick from the list does.
    EXPECT_EQ(backend.active, std::optional<std::size_t>(1));
    EXPECT_EQ(controller.activeConfigurationIndex(), std::optional<std::size_t>(1));

    // The service persisting another choice (or another window switching) moves the combo.
    backend.active = 0;
    controller.serviceCallbacks().onActiveConfigurationChanged(controller.configurations()[0]);
    runPostedEvents();
    EXPECT_EQ(combo.currentIndex(), 0);

    // A refused switch snaps back.
    backend.switchError = Error(ErrorCode::Busy, "a build is running");
    combo.setCurrentIndex(1);
    EXPECT_EQ(combo.currentIndex(), 0);
    EXPECT_EQ(backend.active, std::optional<std::size_t>(0));
    backend.switchError.reset();

    setState(controller, backend, BuildState::Running);
    EXPECT_FALSE(combo.isEnabled());
    setState(controller, backend, BuildState::Idle);
    EXPECT_TRUE(combo.isEnabled());
}

} // namespace
} // namespace frost::ui
