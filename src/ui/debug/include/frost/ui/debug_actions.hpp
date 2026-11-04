// SPDX-License-Identifier: Apache-2.0
//
// The debugger's actions and its tool window registration (PLAN.md M4, ADR 0050). The ids are the
// IntelliJ ones the built-in keymap binds (keymap_intellij.cpp holds the one table of default
// shortcuts): run.debug (Shift+F9), debug.toggleBreakpoint (Ctrl+F8), debug.resume (F9),
// debug.pause (no default), debug.stepOver (F8), debug.stepInto (F7), debug.stepOut (Shift+F8),
// debug.runToCursor (Alt+F9), debug.evaluate (Alt+F8) and window.debug (Alt+5). Stop is run.stop,
// which registerRunActions owns (RunActionHooks::debugging and stopDebugging reach the session).
/*
 * Trust. None is marked executesProjectCode: Debug builds first through the build service (its own
 * trust prompt), and the session start refuses an untrusted root itself (startDebugSession).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"

#include <array>
#include <string_view>

namespace frost::ui {

class DebugController;
class ToolWindowManager;

// The tool window's id and its activation action.
inline constexpr std::string_view kDebugToolWindowId = "debug";
inline constexpr std::string_view kDebugWindowActionId = "window.debug";

// One debugger action: its id, name and what it does (registerDebugActions' table).
struct DebugActionInfo {
    std::string_view id;
    std::string_view name;
    std::string_view description;
};

// The table registerDebugActions registers, in menu order. No shortcuts here: the keymap binds them.
inline constexpr std::array kDebugActions{
    DebugActionInfo{"run.debug", "Debug", "Build the startup project and debug it"},
    DebugActionInfo{"debug.toggleBreakpoint", "Toggle Line Breakpoint", "Add or remove a breakpoint on the caret line"},
    DebugActionInfo{"debug.resume", "Resume Program", "Continue until the next breakpoint"},
    DebugActionInfo{"debug.pause", "Pause Program", "Suspend the program where it runs"},
    DebugActionInfo{"debug.stepOver", "Step Over", "Run to the next line in this function"},
    DebugActionInfo{"debug.stepInto", "Step Into", "Step into the function called on this line"},
    DebugActionInfo{"debug.stepOut", "Step Out", "Run until the current function returns"},
    DebugActionInfo{"debug.runToCursor", "Run to Cursor", "Continue to the caret line"},
    DebugActionInfo{"debug.setNextStatement", "Set Next Statement", "Move the execution point to the caret line"},
    DebugActionInfo{"debug.evaluate", "Evaluate Expression...", "Evaluate an expression in the selected frame"},
    DebugActionInfo{"debug.muteBreakpoints", "Mute Breakpoints", "Temporarily mute or unmute all breakpoints"},
    DebugActionInfo{"debug.editBreakpoint", "Edit Breakpoint...", "Edit the condition or properties of a breakpoint"},
};

/*
 * Registers kDebugActions, driving `controller` (which must outlive the registry's use of them).
 * Errors: AlreadyExists when one of the ids is registered already (nothing after it is added).
 * GUI thread only.
 */
[[nodiscard]] Result<void> registerDebugActions(input::ActionRegistry& registry, DebugController& controller);

/*
 * Adds the Debug tool window (bottom, DebugToolWindow as its content) to `windows` and registers
 * window.debug, which activates it. Errors: those of ToolWindowManager::add and the registry's.
 */
[[nodiscard]] Result<void> registerDebugToolWindow(ToolWindowManager& windows, input::ActionRegistry& registry,
                                                   DebugController& controller);

} // namespace frost::ui
