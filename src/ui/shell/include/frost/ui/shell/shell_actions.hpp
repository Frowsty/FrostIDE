// SPDX-License-Identifier: Apache-2.0
//
// The ids of the actions the shell registers itself (ADR 0038) and of the tool windows it docks.
// Ids that already have IntelliJ-compatible bindings keep the keymap's id (ADR 0016 decision 8);
// the others follow the same group.camelCase convention.
#pragma once

#include <string_view>

namespace frost::ui::actions {

// File.
inline constexpr std::string_view kOpenFile = "file.openFile";
inline constexpr std::string_view kExit = "file.exit";
// Editor tabs.
inline constexpr std::string_view kCloseTab = "window.closeTab";
inline constexpr std::string_view kCloseOtherTabs = "window.closeOtherTabs";
inline constexpr std::string_view kCloseAllTabs = "window.closeAllTabs";
inline constexpr std::string_view kCloseAllButPinned = "window.closeAllButPinned";
inline constexpr std::string_view kPinTab = "window.pinTab";
inline constexpr std::string_view kNextTab = "window.nextTab";
inline constexpr std::string_view kPreviousTab = "window.previousTab";
inline constexpr std::string_view kSwitcher = "window.switcher";
inline constexpr std::string_view kSwitcherBack = "window.switcherBack";
// Tool windows.
inline constexpr std::string_view kProjectWindow = "window.project";
inline constexpr std::string_view kCommitWindow = "window.commit";
inline constexpr std::string_view kRunWindow = "window.run";
inline constexpr std::string_view kProblemsWindow = "window.problems";
inline constexpr std::string_view kNotificationsWindow = "window.notifications";
inline constexpr std::string_view kHideActiveWindow = "window.hideActive";
inline constexpr std::string_view kHideAllWindows = "window.hideAll";
inline constexpr std::string_view kJumpToLastWindow = "window.jumpToLast";
inline constexpr std::string_view kMaximizeToolWindow = "window.maximizeToolWindow";
inline constexpr std::string_view kFocusEditor = "window.focusEditor";
// Keyboard navigation (PLAN.md section 2.2 "Accessibility").
inline constexpr std::string_view kFocusToolbar = "window.focusToolbar";
inline constexpr std::string_view kFocusStatusBar = "window.focusStatusBar";
// View.
inline constexpr std::string_view kToggleToolbar = "view.toggleToolbar";
inline constexpr std::string_view kToggleStatusBar = "view.toggleStatusBar";
inline constexpr std::string_view kCompactMode = "view.compactMode";
inline constexpr std::string_view kZoomIn = "view.zoomInIde";
inline constexpr std::string_view kZoomOut = "view.zoomOutIde";
inline constexpr std::string_view kResetZoom = "view.resetIdeZoom";
inline constexpr std::string_view kQuickSwitchTheme = "view.quickSwitchScheme";
// Navigate.
inline constexpr std::string_view kGoToLine = "editor.gotoLine";
inline constexpr std::string_view kFocusNavigationBar = "navigate.navigationBar";
inline constexpr std::string_view kGoToCppBinding = "navigate.cppBinding";
// Help.
inline constexpr std::string_view kAbout = "help.about";

} // namespace frost::ui::actions

namespace frost::ui::tool_windows {

inline constexpr std::string_view kProject = "project";
inline constexpr std::string_view kCommit = "commit";
inline constexpr std::string_view kRun = "run";
inline constexpr std::string_view kProblems = "problems";
inline constexpr std::string_view kNotifications = "notifications";
// Docked by frost::ui_terminal (ADR 0051), not by the shell; the shell sets TerminalFocus for it.
inline constexpr std::string_view kTerminal = "terminal";

} // namespace frost::ui::tool_windows
