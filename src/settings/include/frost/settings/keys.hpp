// SPDX-License-Identifier: Apache-2.0
//
// Typed handles for FrostIDE's built-in setting keys (builtinRegistry(), ADR 0021), so readers write
// `snapshot.get(key::editorTabSize)` and get a std::int64_t, never a string lookup with a cast.
// SettingsRegistry.TypedKeysMatchTheRegistry checks every handle's name and type.
#pragma once

#include "frost/settings/value.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace frost::settings {

// A key of a known C++ type. A plain value; the name refers to static storage.
template<SettingType T>
struct Key {
    using Type = T;
    std::string_view name;
};

namespace key {

// Editor: font.
inline constexpr Key<std::string> editorFontFamily{"editor.fontFamily"};
inline constexpr Key<std::int64_t> editorFontSize{"editor.fontSize"};
inline constexpr Key<double> editorLineSpacing{"editor.lineSpacing"};
inline constexpr Key<bool> editorFontLigatures{"editor.fontLigatures"};

// Editor: general.
inline constexpr Key<bool> editorSoftWrap{"editor.softWrap"};
inline constexpr Key<bool> editorLineNumbers{"editor.lineNumbers"};
inline constexpr Key<std::string> editorShowWhitespace{"editor.showWhitespace"};
inline constexpr Key<bool> editorIndentGuides{"editor.indentGuides"};
inline constexpr Key<std::int64_t> editorRightMargin{"editor.rightMargin"};
inline constexpr Key<bool> editorShowRightMargin{"editor.showRightMargin"};
inline constexpr Key<bool> editorCaretBlinking{"editor.caretBlinking"};
inline constexpr Key<std::string> editorRenderer{"editor.renderer"};

// Editor: code style.
inline constexpr Key<std::int64_t> editorTabSize{"editor.tabSize"};
inline constexpr Key<std::int64_t> editorIndentSize{"editor.indentSize"};
inline constexpr Key<bool> editorInsertSpaces{"editor.insertSpaces"};
inline constexpr Key<bool> editorDetectIndentation{"editor.detectIndentation"};
inline constexpr Key<std::string> editorTrimTrailingWhitespace{"editor.trimTrailingWhitespace"};
inline constexpr Key<bool> editorInsertFinalNewline{"editor.insertFinalNewline"};
// Actions on save (REF-12): clangd formatting and its include cleaner, both off by default.
inline constexpr Key<bool> editorFormatOnSave{"editor.formatOnSave"};
inline constexpr Key<bool> editorOptimizeImportsOnSave{"editor.optimizeImportsOnSave"};
inline constexpr Key<bool> editorEditorConfig{"editor.editorConfig"};

// Files and encodings.
inline constexpr Key<std::string> filesEncoding{"files.encoding"};
inline constexpr Key<bool> filesUtf8Bom{"files.utf8Bom"};
inline constexpr Key<std::string> filesLineSeparator{"files.lineSeparator"};

// Appearance (frost::ui's IAppearanceSettings adapter reads these).
inline constexpr Key<std::string> uiTheme{"ui.theme"};
inline constexpr Key<std::string> uiThemeSyncLight{"ui.theme.syncLight"};
inline constexpr Key<std::string> uiThemeSyncDark{"ui.theme.syncDark"};
inline constexpr Key<std::string> uiThemeSyncHighContrast{"ui.theme.syncHighContrast"};
inline constexpr Key<bool> uiCompactMode{"ui.compactMode"};
inline constexpr Key<std::int64_t> uiZoom{"ui.zoom"};
inline constexpr Key<bool> uiToolbarVisible{"ui.toolbar.visible"};
inline constexpr Key<bool> uiStatusBarVisible{"ui.statusBar.visible"};
inline constexpr Key<RecordList> uiToolWindows{"ui.toolWindows"};

// Keymap (frost::input's IKeymapSettings adapter reads these).
inline constexpr Key<std::string> keymapId{"keymap.id"};
inline constexpr Key<std::int64_t> keymapDoubleTapIntervalMs{"keymap.doubleTapIntervalMs"};
inline constexpr Key<RecordList> keymapOverrides{"keymap.overrides"};

// Saving, autosave and hot exit (m1/save-manager).
inline constexpr Key<bool> saveAutosaveOnFrameDeactivation{"save.autosave.onFrameDeactivation"};
inline constexpr Key<bool> saveAutosaveOnIdle{"save.autosave.onIdle"};
inline constexpr Key<std::int64_t> saveAutosaveIdleSeconds{"save.autosave.idleSeconds"};
inline constexpr Key<bool> saveHotExit{"save.hotExit"};

// Local history retention (frost::storage's RetentionPolicy).
inline constexpr Key<bool> localHistoryEnabled{"localHistory.enabled"};
inline constexpr Key<std::int64_t> localHistoryMaxAgeDays{"localHistory.maxAgeDays"};
inline constexpr Key<std::int64_t> localHistoryMaxTotalMiB{"localHistory.maxTotalMiB"};
inline constexpr Key<bool> localHistoryKeepNewestPerPath{"localHistory.keepNewestPerPath"};

// Network and update checks (application scope only).
inline constexpr Key<bool> networkOffline{"network.offline"};
inline constexpr Key<std::string> updateChannel{"updates.channel"};

// General behaviour.
inline constexpr Key<bool> generalReopenLastProject{"general.reopenLastProject"};
inline constexpr Key<bool> generalConfirmExit{"general.confirmExit"};
inline constexpr Key<StringList> generalRecentProjects{"general.recentProjects"};

// Build.
inline constexpr Key<bool> buildSaveAllBeforeBuild{"build.saveAllBeforeBuild"};
inline constexpr Key<StringList> buildMsbuildExtraArguments{"build.msbuild.extraArguments"};
inline constexpr Key<std::int64_t> buildMsbuildMaxCpuCount{"build.msbuild.maxCpuCount"};

// CMake profiles (frost::build's cmakeProfileRecords reads the records).
inline constexpr Key<RecordList> cmakeProfiles{"cmake.profiles"};
inline constexpr Key<RecordList> cmakeBuildWrappers{"cmake.buildWrappers"};

// Tool paths: Application-only `executes` keys; empty means "detect" or "bundled".
inline constexpr Key<std::string> toolsMsbuildPath{"tools.msbuild.path"};
inline constexpr Key<std::string> toolsCmakePath{"tools.cmake.path"};
inline constexpr Key<std::string> toolsNinjaPath{"tools.ninja.path"};
inline constexpr Key<std::string> toolsGitPath{"tools.git.path"};
inline constexpr Key<std::string> luaRuntimeVersion{"lua.runtimeVersion"};
inline constexpr Key<std::string> toolsManagedDotnetPath{"tools.managed.dotnetPath"};
inline constexpr Key<std::string> toolsManagedServerPath{"tools.managed.serverAssembly"};
inline constexpr Key<std::string> toolsLuaLanguageServerPath{"tools.luaLanguageServer.path"};
inline constexpr Key<std::string> toolsClangdPath{"tools.clangd.path"};
inline constexpr Key<std::string> toolsClangFormatPath{"tools.clangFormat.path"};
inline constexpr Key<std::string> toolsClangTidyPath{"tools.clangTidy.path"};
inline constexpr Key<StringList> toolsClangTidyPlugins{"tools.clangTidy.plugins"};
inline constexpr Key<std::string> toolsDebuggerPath{"tools.debugger.path"};
inline constexpr Key<std::string> toolsLldbDapPath{"tools.lldbDap.path"};
inline constexpr Key<std::string> debugSourceMapFrom{"debug.sourceMap.from"};
inline constexpr Key<std::string> debugSourceMapTo{"debug.sourceMap.to"};

// Terminal (frost::term, ADR 0051). terminal.shell is an Application-only `executes` key.
inline constexpr Key<std::string> terminalShell{"terminal.shell"};
inline constexpr Key<std::int64_t> terminalScrollbackLines{"terminal.scrollbackLines"};

} // namespace key

} // namespace frost::settings
