// SPDX-License-Identifier: Apache-2.0
// Unit coverage of pushed engine-state projection; real-session acceptance lives in tests/app.
#include "frost/ui/shell/engine_state.hpp"
#include "frost/ui/shell/main_toolbar.hpp"

#include "shell/shell_rig.hpp"
#include <QAction>
#include <QMenu>
#include <QToolButton>
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>

namespace frost::ui {
namespace {
constexpr std::array kEngineActions{MainToolbar::kEnginePlay, MainToolbar::kEnginePause, MainToolbar::kEngineStep,
                                    MainToolbar::kEngineStop, MainToolbar::kEngineSimulate};

TEST(EngineToolbar, ProjectsOnlyDeliveredStateAndDoesNotPredictCommandResults) {
    test::ShellRig rig;
    int unused = 0;
    for (const auto id : kEngineActions)
        rig.addCounted(std::string(id), std::string(id), unused);
    std::string invoked;
    MainToolbar toolbar([&](std::string_view id) { invoked = id; });
    toolbar.refresh(rig.registry, *rig.keymap, {});
    for (const auto id : kEngineActions) {
        ASSERT_NE(toolbar.button(id), nullptr);
        EXPECT_TRUE(toolbar.button(id)->isHidden());
    }
    EngineControlState state;
    state.connected = true;
    state.canPlay = state.canPause = state.canStep = state.canStop = state.canSimulate = true;
    toolbar.setEngineState(state);
    EXPECT_TRUE(toolbar.button(MainToolbar::kEnginePlay)->isEnabled());
    EXPECT_TRUE(toolbar.button(MainToolbar::kEngineSimulate)->isEnabled());
    EXPECT_FALSE(toolbar.button(MainToolbar::kEnginePause)->isEnabled());
    EXPECT_FALSE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
    toolbar.button(MainToolbar::kEnginePlay)->click();
    EXPECT_EQ(invoked, MainToolbar::kEnginePlay);
    EXPECT_FALSE(toolbar.button(MainToolbar::kEnginePlay)->isChecked());
    EXPECT_FALSE(toolbar.engineState()->playing);
    // Only delivery of the play event changes the display, even when engine-side controls made it.
    state.playing = true;
    toolbar.setEngineState(state);
    EXPECT_TRUE(toolbar.button(MainToolbar::kEnginePlay)->isChecked());
    EXPECT_FALSE(toolbar.button(MainToolbar::kEnginePlay)->isEnabled());
    EXPECT_TRUE(toolbar.button(MainToolbar::kEnginePause)->isEnabled());
    EXPECT_TRUE(toolbar.button(MainToolbar::kEngineStop)->isEnabled());
    toolbar.button(MainToolbar::kEnginePause)->click();
    EXPECT_FALSE(toolbar.button(MainToolbar::kEnginePause)->isChecked());
    for (int cycle = 0; cycle < 20; ++cycle) {
        state.paused = true;
        toolbar.setEngineState(state);
        EXPECT_TRUE(toolbar.button(MainToolbar::kEnginePause)->isChecked());
        EXPECT_TRUE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
        EXPECT_TRUE(toolbar.button(MainToolbar::kEnginePlay)->isEnabled());
        state.paused = false;
        toolbar.setEngineState(state);
        EXPECT_FALSE(toolbar.button(MainToolbar::kEnginePause)->isChecked());
        EXPECT_FALSE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
    }
    state.paused = true;
    toolbar.setEngineState(state);
    ASSERT_NE(toolbar.button(MainToolbar::kEngineStep)->menu(), nullptr);
    EXPECT_EQ(toolbar.engineStepFrames(), 1);
    toolbar.button(MainToolbar::kEngineStep)->menu()->actions().at(1)->trigger();
    EXPECT_EQ(toolbar.engineStepFrames(), 5);
    EXPECT_EQ(invoked, MainToolbar::kEngineStep);
    EXPECT_TRUE(toolbar.engineState()->paused);
    state.connected = false;
    toolbar.setEngineState(state);
    for (const auto id : kEngineActions)
        EXPECT_FALSE(toolbar.button(id)->isEnabled());
    toolbar.setEngineState(std::nullopt);
    for (const auto id : kEngineActions)
        EXPECT_TRUE(toolbar.button(id)->isHidden());
}

TEST(EngineToolbar, IntersectsStateSchemaPolicyAndRegisteredActionAvailability) {
    test::ShellRig rig;
    int unused = 0;
    for (const auto id : kEngineActions)
        rig.addCounted(std::string(id), std::string(id), unused);
    MainToolbar toolbar(MainToolbar::Invoker{});
    toolbar.refresh(rig.registry, *rig.keymap, {});
    EngineControlState state;
    state.connected = state.playing = state.paused = true;
    toolbar.setEngineState(state);
    EXPECT_FALSE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
    state.canStep = true;
    toolbar.setEngineState(state);
    EXPECT_TRUE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
    state.debuggerPaused = true;
    state.canPlay = state.canPause = state.canSimulate = true;
    toolbar.setEngineState(state);
    for (const auto id : kEngineActions)
        EXPECT_FALSE(toolbar.button(id)->isEnabled());
    // DAP terminate availability is supplied by the app; normal stop permission alone is absent.
    state.canStop = true;
    toolbar.setEngineState(state);
    EXPECT_TRUE(toolbar.button(MainToolbar::kEngineStop)->isEnabled());
    EXPECT_FALSE(toolbar.button(MainToolbar::kEngineStep)->isEnabled());
    EXPECT_TRUE(toolbar.button(MainToolbar::kEngineStep)->accessibleDescription().contains(QStringLiteral("debugger")));

    // A confirmed native all-stop can suspend the engine socket's heartbeat. The app retains
    // only its exact authenticated DAP binding; Stop still routes through that debugger.
    state.connected = false;
    toolbar.setEngineState(state);
    for (const auto id : kEngineActions)
        EXPECT_EQ(toolbar.button(id)->isEnabled(), id == MainToolbar::kEngineStop);
    EXPECT_FALSE(toolbar.engineState()->connected);
    state.canStop = false;
    toolbar.setEngineState(state);
    EXPECT_FALSE(toolbar.button(MainToolbar::kEngineStop)->isEnabled());
    state.canStop = true;
    state.debuggerPaused = false;
    toolbar.setEngineState(state);
    for (const auto id : kEngineActions)
        EXPECT_FALSE(toolbar.button(id)->isEnabled());
    state.debuggerPaused = true;
    toolbar.setEngineState(state);

    test::ShellRig emptyRig;
    toolbar.refresh(emptyRig.registry, *emptyRig.keymap, {});
    for (const auto id : kEngineActions)
        EXPECT_FALSE(toolbar.button(id)->isEnabled());
}

TEST(EngineConnectionIndicator, ShowsSessionReasonsAsEscapedText) {
    EngineConnectionIndicator indicator;
    EXPECT_EQ(indicator.state(), EngineConnectionState::Disconnected);
    EXPECT_TRUE(indicator.text().contains(QStringLiteral("Disconnected")));
    for (const auto state :
         {EngineConnectionState::Connecting, EngineConnectionState::Connected, EngineConnectionState::StaleDiscovery,
          EngineConnectionState::WrongProject, EngineConnectionState::IncompatibleContract,
          EngineConnectionState::DebuggerPaused, EngineConnectionState::Paused}) {
        indicator.setState(state, QStringLiteral("<img src='secret'>"));
        EXPECT_EQ(indicator.state(), state);
        EXPECT_FALSE(indicator.toolTip().contains(QStringLiteral("<img")));
        EXPECT_TRUE(indicator.accessibleDescription().contains(QStringLiteral("<img")));
    }
}
} // namespace
} // namespace frost::ui
