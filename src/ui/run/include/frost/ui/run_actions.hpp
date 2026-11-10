// SPDX-License-Identifier: Apache-2.0
//
// The Run actions (W1, ADR 0040) in frost::input's registry, with the ids the built-in keymap
// binds (keymap_intellij.cpp): run.run (Shift+F10), run.runWithoutDebugging (Ctrl+F5, Visual
// Studio's "Start Without Debugging"; with no debugger yet it is the same as Run), run.stop
// (Ctrl+F2), run.rerun, run.setStartupProject (the Solution Explorer's context menu, "Set as
// Startup Project") and run.toggleExternalConsole. The shell's own ids (window.*, view.*,
// file.*, help.*) and run.debug (registerDebugActions, frost/ui/debug_actions.hpp) are not
// registered here.
//
/*
 * Trust. None is marked executesProjectCode: a run builds first, and the build service asks
 * for trust with its own prompt; the registry's gate would only disable Run silently. A run
 * without a build checks the trust store itself (RunService).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/project/solution_model.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace frost::ui {

class RunController;

// The ids registerRunActions adds.
inline constexpr std::string_view kRunActionId = "run.run";
inline constexpr std::string_view kRunContextActionId = "run.runContext";
inline constexpr std::string_view kRunWithoutDebuggingActionId = "run.runWithoutDebugging";
inline constexpr std::string_view kStopActionId = "run.stop";
inline constexpr std::string_view kRerunActionId = "run.rerun";
inline constexpr std::string_view kSetStartupProjectActionId = "run.setStartupProject";
inline constexpr std::string_view kToggleExternalConsoleActionId = "run.toggleExternalConsole";

struct RunActionHooks {
    // The project selected in the Solution Explorer, for Set as Startup Project; empty: none.
    std::function<std::optional<project::ProjectId>()> selectedProject{};
    // Shows why an action failed (the run could not start); empty: ignored.
    std::function<void(const Error& error)> reportError{};
    // Stop also ends a debug session (Rider's one Stop, Ctrl+F2): whether one is live, and ending
    // it (frost::ui::DebugController). Empty: Stop ends runs only.
    std::function<bool()> debugging{};
    std::function<void()> stopDebugging{};
};

/*
 * Registers the actions above, driving `controller` (which must outlive the registry's use of
 * them). Errors: AlreadyExists when one of the ids is registered already (nothing after it is
 * added). GUI thread only.
 */
[[nodiscard]] Result<void> registerRunActions(input::ActionRegistry& registry, RunController& controller,
                                              RunActionHooks hooks = {});

} // namespace frost::ui
