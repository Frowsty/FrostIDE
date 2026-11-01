// SPDX-License-Identifier: Apache-2.0
//
// The icon ids of resources/icons/icons.json, as constants (ADR 0015 decision 7). Ids are
// lowercase, dot-separated, with kebab-case segments. Icons.IdsMatchManifest checks that this
// list and the manifest match both ways; add an icon to both.
#pragma once

#include <array>
#include <string_view>

namespace frost::ui::icons {

inline constexpr std::string_view kActionsAdd = "actions.add";
inline constexpr std::string_view kActionsClose = "actions.close";
inline constexpr std::string_view kActionsFilter = "actions.filter";
inline constexpr std::string_view kActionsMore = "actions.more";
inline constexpr std::string_view kActionsOk = "actions.ok";
inline constexpr std::string_view kActionsPin = "actions.pin";
inline constexpr std::string_view kActionsRefresh = "actions.refresh";
inline constexpr std::string_view kActionsRemove = "actions.remove";
inline constexpr std::string_view kActionsSearch = "actions.search";
inline constexpr std::string_view kActionsSettings = "actions.settings";
inline constexpr std::string_view kActionsShow = "actions.show";
inline constexpr std::string_view kBuildBuild = "build.build";
inline constexpr std::string_view kBuildPackage = "build.package";
inline constexpr std::string_view kEditCopy = "edit.copy";
inline constexpr std::string_view kEditCut = "edit.cut";
inline constexpr std::string_view kEditPaste = "edit.paste";
inline constexpr std::string_view kEditRedo = "edit.redo";
inline constexpr std::string_view kEditUndo = "edit.undo";
inline constexpr std::string_view kFileSave = "file.save";
inline constexpr std::string_view kLayoutPanelBottom = "layout.panel-bottom";
inline constexpr std::string_view kLayoutPanelLeft = "layout.panel-left";
inline constexpr std::string_view kNavBack = "nav.back";
inline constexpr std::string_view kNavChevronDown = "nav.chevron-down";
inline constexpr std::string_view kNavChevronLeft = "nav.chevron-left";
inline constexpr std::string_view kNavChevronRight = "nav.chevron-right";
inline constexpr std::string_view kNavChevronUp = "nav.chevron-up";
inline constexpr std::string_view kNavForward = "nav.forward";
inline constexpr std::string_view kNodesFile = "nodes.file";
inline constexpr std::string_view kNodesFileCode = "nodes.file-code";
inline constexpr std::string_view kNodesFolder = "nodes.folder";
inline constexpr std::string_view kNodesFolderOpen = "nodes.folder-open";
inline constexpr std::string_view kRunDebug = "run.debug";
inline constexpr std::string_view kRunRun = "run.run";
inline constexpr std::string_view kRunStop = "run.stop";
inline constexpr std::string_view kStatusError = "status.error";
inline constexpr std::string_view kStatusFailed = "status.failed";
inline constexpr std::string_view kStatusInfo = "status.info";
inline constexpr std::string_view kStatusLoading = "status.loading";
inline constexpr std::string_view kStatusOk = "status.ok";
inline constexpr std::string_view kStatusWarning = "status.warning";
inline constexpr std::string_view kToolwindowHistory = "toolwindow.history";
inline constexpr std::string_view kToolwindowNotifications = "toolwindow.notifications";
inline constexpr std::string_view kToolwindowStructure = "toolwindow.structure";
inline constexpr std::string_view kToolwindowTerminal = "toolwindow.terminal";
inline constexpr std::string_view kVcsBranch = "vcs.branch";
inline constexpr std::string_view kWindowMaximize = "window.maximize";
inline constexpr std::string_view kWindowMinimize = "window.minimize";

// Every id above, in manifest order.
inline constexpr std::array kAllIconIds{
    kActionsAdd,
    kActionsClose,
    kActionsFilter,
    kActionsMore,
    kActionsOk,
    kActionsPin,
    kActionsRefresh,
    kActionsRemove,
    kActionsSearch,
    kActionsSettings,
    kActionsShow,
    kBuildBuild,
    kBuildPackage,
    kEditCopy,
    kEditCut,
    kEditPaste,
    kEditRedo,
    kEditUndo,
    kFileSave,
    kLayoutPanelBottom,
    kLayoutPanelLeft,
    kNavBack,
    kNavChevronDown,
    kNavChevronLeft,
    kNavChevronRight,
    kNavChevronUp,
    kNavForward,
    kNodesFile,
    kNodesFileCode,
    kNodesFolder,
    kNodesFolderOpen,
    kRunDebug,
    kRunRun,
    kRunStop,
    kStatusError,
    kStatusFailed,
    kStatusInfo,
    kStatusLoading,
    kStatusOk,
    kStatusWarning,
    kToolwindowHistory,
    kToolwindowNotifications,
    kToolwindowStructure,
    kToolwindowTerminal,
    kVcsBranch,
    kWindowMaximize,
    kWindowMinimize,
};

} // namespace frost::ui::icons
