// SPDX-License-Identifier: Apache-2.0
//
// FrostIDE's built-in setting keys (registry.hpp, keys.hpp; ADR 0021). Defaults follow Rider and
// IntelliJ where FrostIDE has the same feature (JetBrains Mono 13 at 1.2 line spacing, 4-space
// indents, the IntelliJ keymap, the dark theme, autosave on frame deactivation and when idle).
// Every key carries the title, description and category the M15 Settings dialog is generated from.

#include "frost/core/assert.hpp"
#include "frost/settings/registry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace frost::settings {

namespace {

struct Entry {
    const char* key;
    const char* title;
    const char* description;
    const char* category;
    Schema schema;
    Value defaultValue;
    ScopeMask scopes;
    bool executes = false;
    bool advanced = false;
};

[[nodiscard]] std::vector<EnumOption> options(std::initializer_list<std::pair<const char*, const char*>> list) {
    std::vector<EnumOption> result;
    for (const auto& [value, label] : list) {
        result.push_back(EnumOption{.value = value, .label = label, .description = {}});
    }
    return result;
}

[[nodiscard]] Value text(const char* value) { return Value(std::string(value)); }
[[nodiscard]] Value integer(std::int64_t value) { return Value(value); }
[[nodiscard]] Value emptyList() { return Value(StringList{}); }

// An Application-only `executes` key naming a program; "" means detect or use the bundled one.
[[nodiscard]] Entry toolPath(const char* key, const char* title, const char* description) {
    return Entry{key,
                 title,
                 description,
                 "Build, Execution, Deployment/Toolchains",
                 stringSchema(4096, StringHint::Executable),
                 text(""),
                 kApplicationOnly,
                 true,
                 false};
}

[[nodiscard]] std::vector<Entry> entries() {
    const ScopeMask projectToo = kAllFileScopes;
    const ScopeMask editorConfig = kAllScopesWithEditorConfig;
    return {
        // Editor: font.
        {"editor.fontFamily", "Font",
         "The editor font family. FrostIDE bundles JetBrains Mono, Fira Code, Cascadia Code, Cascadia Mono, "
         "Source Code Pro, IBM Plex Mono, Inconsolata, Intel One Mono, CommitMono, and Fantasque Sans Mono.",
         "Editor/Font",
         stringSchema(256, StringHint::FontFamily,
                      options({{"JetBrains Mono", "JetBrains Mono"},
                               {"Fira Code", "Fira Code"},
                               {"Cascadia Code", "Cascadia Code"},
                               {"Cascadia Mono", "Cascadia Mono"},
                               {"Source Code Pro", "Source Code Pro"},
                               {"IBM Plex Mono", "IBM Plex Mono"},
                               {"Inconsolata", "Inconsolata"},
                               {"Intel One Mono", "Intel One Mono"},
                               {"CommitMono", "CommitMono"},
                               {"Fantasque Sans Mono", "Fantasque Sans Mono"}})),
         text("JetBrains Mono"), kApplicationAndPersonal},
        {"editor.fontSize", "Size", "The editor font size in points, before IDE zoom.", "Editor/Font",
         integerSchema(4, 96), integer(13), kApplicationAndPersonal},
        {"editor.lineSpacing", "Line height", "The editor line height as a multiple of the font size.", "Editor/Font",
         numberSchema(0.6, 3.0), Value(1.2), kApplicationAndPersonal},
        {"editor.fontLigatures", "Enable ligatures", "Draws programming ligatures when the font has them.",
         "Editor/Font", boolSchema(), Value(false), kApplicationAndPersonal},

        // Editor: general.
        {"editor.softWrap", "Soft-wrap lines", "Wraps long lines at the window edge instead of scrolling sideways.",
         "Editor/General", boolSchema(), Value(false), projectToo},
        {"editor.lineNumbers", "Show line numbers", "Shows line numbers in the gutter.", "Editor/General/Appearance",
         boolSchema(), Value(true), kApplicationAndPersonal},
        {"editor.showWhitespace", "Show whitespaces",
         "Draws spaces and tabs as faint marks: never, inside the selection only, or everywhere.",
         "Editor/General/Appearance",
         enumSchema(options({{"none", "Never"}, {"selection", "In the selection"}, {"all", "Everywhere"}})),
         text("none"), kApplicationAndPersonal},
        {"editor.indentGuides", "Show indent guides", "Draws vertical guides at each indentation level.",
         "Editor/General/Appearance", boolSchema(), Value(true), kApplicationAndPersonal},
        {"editor.rightMargin", "Hard wrap at", "The right margin column (also EditorConfig's max_line_length).",
         "Editor/Code Style", integerSchema(1, 1000), integer(120), editorConfig},
        {"editor.showRightMargin", "Show the right margin", "Draws a vertical line at the right margin column.",
         "Editor/General/Appearance", boolSchema(), Value(true), kApplicationAndPersonal},
        {"editor.caretBlinking", "Caret blinking", "Makes the text caret blink.", "Editor/General/Appearance",
         boolSchema(), Value(true), kApplicationOnly},
        {"editor.renderer", "Editor renderer",
         "How the editor draws: auto uses the GPU (Direct3D 11) on Windows except in remote sessions and on a "
         "software adapter, and the CPU elsewhere; raster always draws on the CPU; rhi always tries the GPU. A GPU "
         "failure falls back to the CPU renderer.",
         "Editor/General/Appearance",
         enumSchema(options({{"auto", "Automatic"}, {"raster", "CPU (raster)"}, {"rhi", "GPU (QRhi)"}})), text("auto"),
         kApplicationOnly, false, true},

        // Editor: code style.
        {"editor.tabSize", "Tab size", "How many columns a tab character advances (EditorConfig's tab_width).",
         "Editor/Code Style", integerSchema(1, 32), integer(4), editorConfig},
        {"editor.indentSize", "Indent", "How many columns one indentation level is (EditorConfig's indent_size).",
         "Editor/Code Style", integerSchema(1, 32), integer(4), editorConfig},
        {"editor.insertSpaces", "Use spaces for indentation",
         "Indents with spaces instead of tab characters (EditorConfig's indent_style).", "Editor/Code Style",
         boolSchema(), Value(true), editorConfig},
        {"editor.detectIndentation", "Detect and use existing file indents",
         "Uses the indentation a file already has instead of the settings above when they differ.", "Editor/Code Style",
         boolSchema(), Value(true), projectToo},
        {"editor.trimTrailingWhitespace", "Strip trailing spaces on save",
         "Removes trailing whitespace when saving: never, on modified lines only, or on every line "
         "(EditorConfig's trim_trailing_whitespace).",
         "Editor/General/On Save",
         enumSchema(options({{"none", "None"}, {"modified", "Modified lines"}, {"all", "All lines"}})),
         text("modified"), editorConfig},
        {"editor.insertFinalNewline", "Ensure every saved file ends with a line break",
         "Adds a final line break when saving a file that lacks one (EditorConfig's insert_final_newline).",
         "Editor/General/On Save", boolSchema(), Value(false), editorConfig},
        // REF-12: actions on save, computed by clangd for C and C++ files; off by default.
        {"editor.formatOnSave", "Reformat code on save",
         "Reformats C and C++ files before saving with clangd's formatting (clang-format with the .clang-format "
         "above the file). Skipped, with a notification, when clangd is not running or does not answer in time.",
         "Editor/General/On Save", boolSchema(), Value(false), projectToo},
        {"editor.optimizeImportsOnSave", "Optimize includes on save",
         "Applies clangd's include-cleaner fix (unused and missing includes) to C and C++ files before saving, "
         "when clangd's diagnostics describe the text being saved.",
         "Editor/General/On Save", boolSchema(), Value(false), projectToo},
        {"editor.editorConfig", "Enable EditorConfig support",
         "Applies the .editorconfig files above each file on top of these settings.", "Editor/Code Style", boolSchema(),
         Value(true), projectToo},

        // Files and encodings.
        {"files.encoding", "Default encoding",
         "The encoding for new files and for files whose encoding cannot be detected (EditorConfig's charset).",
         "Editor/File Encodings",
         stringSchema(64, StringHint::Encoding,
                      options({{"UTF-8", "UTF-8"},
                               {"UTF-16LE", "UTF-16 LE"},
                               {"UTF-16BE", "UTF-16 BE"},
                               {"windows-1252", "Windows-1252"},
                               {"ISO-8859-1", "ISO-8859-1"}})),
         text("UTF-8"), editorConfig},
        {"files.utf8Bom", "Create UTF-8 files with a BOM",
         "Writes a byte-order mark at the start of new UTF-8 files (EditorConfig's charset = utf-8-bom).",
         "Editor/File Encodings", boolSchema(), Value(false), editorConfig},
        {"files.lineSeparator", "Line separator for new files",
         "The line break new files use; existing files keep theirs (EditorConfig's end_of_line).", "Editor/Code Style",
         enumSchema(options({{"system", "System-dependent"},
                             {"lf", "Unix and macOS (LF)"},
                             {"crlf", "Windows (CRLF)"},
                             {"cr", "Classic Mac OS (CR)"}})),
         text("system"), editorConfig},

        // Outbound requests require explicit per-check consent even while online.
        {"network.offline", "Work offline", "Disables outbound update checks and downloads.",
         "Appearance & Behavior/Updates", boolSchema(), Value(false), kApplicationOnly},
        {"updates.channel", "Update channel", "Selects stable, EAP, or nightly releases from Frowsty/FrostIDE.",
         "Appearance & Behavior/Updates",
         enumSchema(options({{"stable", "Stable"}, {"eap", "EAP"}, {"nightly", "Nightly"}})), text("stable"),
         kApplicationOnly},
        // Appearance.
        {"ui.theme", "Theme",
         "The IDE theme, or sync-with-os to follow the system's light, dark and high-contrast mode.",
         "Appearance & Behavior/Appearance",
         stringSchema(64, StringHint::ThemeId,
                      options({{"rider-dark", "Rider Dark"},
                               {"rider-light", "Rider Light"},
                               {"rider-islands-dark", "Rider Islands Dark"},
                               {"darcula", "Darcula"},
                               {"glacier-dark", "Glacier Dark"},
                               {"glacier-light", "Glacier Light"},
                               {"glacier-classic", "Glacier Classic"},
                               {"glacier-high-contrast", "Glacier High Contrast"},
                               {"monokai-pro", "Monokai Pro"},
                               {"dracula", "Dracula"},
                               {"nord", "Nord"},
                               {"one-dark", "One Dark"},
                               {"solarized-dark", "Solarized Dark"},
                               {"solarized-light", "Solarized Light"},
                               {"catppuccin-mocha", "Catppuccin Mocha"},
                               {"catppuccin-macchiato", "Catppuccin Macchiato"},
                               {"catppuccin-frappe", "Catppuccin Frappe"},
                               {"catppuccin-latte", "Catppuccin Latte"},
                               {"tokyo-night", "Tokyo Night"},
                               {"tokyo-night-storm", "Tokyo Night Storm"},
                               {"tokyo-night-light", "Tokyo Night Light"},
                               {"gruvbox-dark", "Gruvbox Dark"},
                               {"gruvbox-light", "Gruvbox Light"},
                               {"github-dark", "GitHub Dark"},
                               {"github-light", "GitHub Light"},
                               {"ayu-dark", "Ayu Dark"},
                               {"ayu-mirage", "Ayu Mirage"},
                               {"ayu-light", "Ayu Light"},
                               {"everforest-dark", "Everforest Dark"},
                               {"kanagawa-wave", "Kanagawa Wave"},
                               {"rose-pine", "Rose Pine"},
                               {"rose-pine-dawn", "Rose Pine Dawn"},
                               {"night-owl", "Night Owl"},
                               {"synthwave-84", "SynthWave '84"},
                               {"sync-with-os", "Sync with OS"}})),
         text("glacier-dark"), kApplicationOnly},
        {"ui.theme.syncLight", "Theme for light mode", "The theme sync-with-os uses while the system is light.",
         "Appearance & Behavior/Appearance", stringSchema(64, StringHint::ThemeId), text("glacier-light"),
         kApplicationOnly, false, true},
        {"ui.theme.syncDark", "Theme for dark mode", "The theme sync-with-os uses while the system is dark.",
         "Appearance & Behavior/Appearance", stringSchema(64, StringHint::ThemeId), text("glacier-dark"),
         kApplicationOnly, false, true},
        {"ui.theme.syncHighContrast", "Theme for high contrast",
         "The theme sync-with-os uses while the system's high-contrast mode is on.", "Appearance & Behavior/Appearance",
         stringSchema(64, StringHint::ThemeId), text("glacier-high-contrast"), kApplicationOnly, false, true},
        {"ui.compactMode", "Compact mode", "Uses smaller paddings in tool windows, tabs and toolbars.",
         "Appearance & Behavior/Appearance", boolSchema(), Value(false), kApplicationOnly},
        {"ui.zoom", "IDE zoom", "Scales the whole IDE, in percent.", "Appearance & Behavior/Appearance",
         integerSchema(50, 300), integer(100), kApplicationOnly},
        // The window layout the shell restores (W1, ADR 0044); written by the IDE, not the dialog.
        {"ui.toolbar.visible", "Show main toolbar", "Shows the main toolbar under the menu bar.",
         "Appearance & Behavior/Appearance", boolSchema(), Value(true), kApplicationOnly},
        {"ui.statusBar.visible", "Show status bar", "Shows the status bar at the bottom of the window.",
         "Appearance & Behavior/Appearance", boolSchema(), Value(true), kApplicationOnly},
        {"ui.toolWindows", "Tool window layout",
         "Which tool windows are open and their sizes, as the IDE last left them.", "Appearance & Behavior/Appearance",
         recordListSchema({FieldSpec{.name = "id",
                                     .type = FieldType::String,
                                     .required = true,
                                     .description = "The tool window id (\"project\", \"build\")."},
                           FieldSpec{.name = "visible",
                                     .type = FieldType::Bool,
                                     .required = true,
                                     .description = "Whether the tool window is open."},
                           FieldSpec{.name = "size",
                                     .type = FieldType::Integer,
                                     .required = false,
                                     .description = "Its width or height in pixels; 0 or absent uses the default."}},
                          64),
         Value(RecordList{}), kApplicationOnly, false, true},

        // Keymap.
        {"keymap.id", "Keymap", "The keyboard shortcut scheme.", "Keymap",
         enumSchema(options({{"intellij", "IntelliJ IDEA (Windows and Linux)"},
                             {"visual-studio", "Visual Studio"},
                             {"visual-studio-2022", "Visual Studio 2022"},
                             {"resharper", "ReSharper (Visual Studio scheme)"}})),
         text("intellij"), kApplicationOnly},
        {"keymap.doubleTapIntervalMs", "Double modifier interval",
         "How quickly a modifier must be pressed twice (Double Shift) in milliseconds; 0 uses the system's "
         "double-click time.",
         "Keymap", integerSchema(0, 2000), integer(0), kApplicationOnly, false, true},
        {"keymap.overrides", "Custom shortcuts",
         "Actions whose shortcuts differ from the keymap: each entry replaces the action's whole shortcut list.",
         "Keymap",
         recordListSchema(
             {FieldSpec{.name = "action", .type = FieldType::String, .required = true, .description = "The action id."},
              FieldSpec{.name = "shortcuts",
                        .type = FieldType::StringList,
                        .required = true,
                        .description = "Shortcuts in strict syntax (\"Ctrl+K, Ctrl+C\"); empty removes all."}}),
         Value(RecordList{}), kApplicationOnly},

        // Saving and autosave.
        {"save.autosave.onFrameDeactivation", "Save files when switching to a different application",
         "Saves every modified file when the IDE window loses focus.", "Appearance & Behavior/System Settings",
         boolSchema(), Value(true), kApplicationAndPersonal},
        {"save.autosave.onIdle", "Save files if the IDE is idle",
         "Saves every modified file after the IDE has been idle for the time below.",
         "Appearance & Behavior/System Settings", boolSchema(), Value(true), kApplicationAndPersonal},
        {"save.autosave.idleSeconds", "Idle time before saving", "Seconds without input before an idle save.",
         "Appearance & Behavior/System Settings", integerSchema(1, 3600), integer(15), kApplicationAndPersonal},
        {"save.hotExit", "Keep unsaved changes on exit",
         "Remembers unsaved edits across restarts and crashes in the hot-exit journal.",
         "Appearance & Behavior/System Settings", boolSchema(), Value(true), kApplicationOnly},

        // Local history.
        {"localHistory.enabled", "Record local history", "Keeps a revision of every file each time it is saved.",
         "Appearance & Behavior/Local History", boolSchema(), Value(true), kApplicationOnly},
        {"localHistory.maxAgeDays", "Keep local history for", "Days after which revisions are pruned.",
         "Appearance & Behavior/Local History", integerSchema(1, 3650), integer(5), kApplicationOnly},
        {"localHistory.maxTotalMiB", "Local history size limit",
         "The most content, in MiB, local history keeps; the oldest revisions go first.",
         "Appearance & Behavior/Local History", integerSchema(16, 1'048'576), integer(1024), kApplicationOnly},
        {"localHistory.keepNewestPerPath", "Always keep each file's newest revision",
         "Pruning never removes the newest revision of a file.", "Appearance & Behavior/Local History", boolSchema(),
         Value(true), kApplicationOnly, false, true},

        // General.
        // Off by default: FrostIDE starts in the stripped-down launcher (open, recent and new project),
        // the user's design (2026-11-08). Reopening the last project straight into the IDE is opt-in.
        {"general.reopenLastProject", "Reopen projects on startup", "Opens the last project when FrostIDE starts.",
         "Appearance & Behavior/System Settings", boolSchema(), Value(false), kApplicationOnly},
        {"general.confirmExit", "Confirm before exiting the IDE", "Asks before closing the last window.",
         "Appearance & Behavior/System Settings", boolSchema(), Value(false), kApplicationOnly},
        {"general.recentProjects", "Recent projects",
         "The solutions and folders opened most recently, newest first (File > Open Recent and the Welcome screen).",
         "Appearance & Behavior/System Settings", stringListSchema(50, 4096, StringHint::Path), emptyList(),
         kApplicationOnly, false, true},

        // Build.
        {"build.saveAllBeforeBuild", "Save all files before building",
         "Saves every modified file before a build starts.", "Build, Execution, Deployment/Build", boolSchema(),
         Value(true), projectToo},
        {"build.msbuild.extraArguments", "Additional MSBuild arguments",
         "Extra command-line arguments for every MSBuild run. They can load loggers and tasks, so a project's "
         "value applies only once the project is trusted.",
         "Build, Execution, Deployment/Build", stringListSchema(64, 1024, StringHint::Command), emptyList(), projectToo,
         true},
        {"build.msbuild.maxCpuCount", "Parallel project builds",
         "How many projects MSBuild builds at once (/m); 0 uses every processor.", "Build, Execution, Deployment/Build",
         integerSchema(0, 256), integer(0), projectToo},

        // CMake profiles (frost::build's CMakeService; ADR 0046). They configure and build the folder,
        // so a project's value applies only once the project is trusted.
        {"cmake.profiles", "CMake profiles",
         "The CMake profiles of folders without CMake presets. Empty uses one Debug profile (Ninja, "
         "cmake-build-debug).",
         "Build, Execution, Deployment/CMake",
         recordListSchema(
             {FieldSpec{
                  .name = "name", .type = FieldType::String, .required = true, .description = "The profile's name."},
              FieldSpec{.name = "buildType",
                        .type = FieldType::String,
                        .required = false,
                        .description = "CMAKE_BUILD_TYPE (Debug, Release, RelWithDebInfo, MinSizeRel)."},
              FieldSpec{.name = "generator",
                        .type = FieldType::String,
                        .required = false,
                        .description = "The CMake generator; empty uses Ninja."},
              FieldSpec{.name = "buildDirectory",
                        .type = FieldType::String,
                        .required = false,
                        .description = "The build directory, relative to the folder; empty uses "
                                       "cmake-build-<profile name in lower case>."},
              FieldSpec{.name = "cacheVariables",
                        .type = FieldType::StringList,
                        .required = false,
                        .description = "Cache variables as NAME[:TYPE]=VALUE."},
              FieldSpec{.name = "environment",
                        .type = FieldType::StringList,
                        .required = false,
                        .description = "Environment variables as NAME=VALUE; NAME alone removes one."},
              FieldSpec{.name = "buildOptions",
                        .type = FieldType::StringList,
                        .required = false,
                        .description = "Arguments for the build tool (after --)."},
              FieldSpec{.name = "jobs",
                        .type = FieldType::Integer,
                        .required = false,
                        .description = "Parallel build jobs (-j); 0 leaves it to the build tool."},
              FieldSpec{.name = "root",
                        .type = FieldType::String,
                        .required = false,
                        .description = "The folder the profile is for (Application scope); empty applies to "
                                       "every folder."}}),
         Value(RecordList{}), projectToo, true},

        {"cmake.buildWrappers", "CMake build wrappers",
         "Personal build commands for specific CMake roots and profiles; arguments are separate values.",
         "Build, Execution, Deployment/CMake",
         recordListSchema(
             {FieldSpec{
                  .name = "root", .type = FieldType::String, .required = true, .description = "Absolute project root."},
              FieldSpec{.name = "profile",
                        .type = FieldType::String,
                        .required = true,
                        .description = "Exact CMake profile name."},
              FieldSpec{.name = "executable",
                        .type = FieldType::String,
                        .required = true,
                        .description = "Absolute executable path."},
              FieldSpec{.name = "defaultTarget",
                        .type = FieldType::String,
                        .required = false,
                        .description = "Target used when none is selected."},
              FieldSpec{.name = "buildArguments",
                        .type = FieldType::StringList,
                        .required = true,
                        .description = "Arguments for Build."},
              FieldSpec{.name = "cleanArguments",
                        .type = FieldType::StringList,
                        .required = false,
                        .description = "Arguments for Clean."},
              FieldSpec{.name = "rebuildArguments",
                        .type = FieldType::StringList,
                        .required = false,
                        .description = "Arguments for Rebuild."}}),
         Value(RecordList{}), kApplicationOnly, true},

        {"lua.runtimeVersion", "Lua runtime version", "Default Lua runtime for LuaLS; .luarc.json takes priority.",
         "Languages & Frameworks/Lua",
         enumSchema(options({{"Lua 5.1", "Lua 5.1"},
                             {"Lua 5.2", "Lua 5.2"},
                             {"Lua 5.3", "Lua 5.3"},
                             {"Lua 5.4", "Lua 5.4"},
                             {"LuaJIT", "LuaJIT"}})),
         text("Lua 5.4"), kApplicationAndPersonal},

        // Tool paths (Application-only executes keys).
        toolPath("tools.msbuild.path", "MSBuild", "MSBuild.exe to build solutions with; empty finds it with vswhere."),
        toolPath("tools.vcpkg.path", "vcpkg", "Absolute user-installed vcpkg executable; dependency operations require explicit consent."),
        toolPath("tools.conan.path", "Conan 2", "Absolute user-installed Conan 2 executable; dependency operations require explicit consent."),
        toolPath("tools.nuget.path", "NuGet", "Absolute user-installed NuGet.exe for native packages.config restore from a local feed."),
        toolPath("tools.cmake.path", "CMake", "The cmake executable; empty uses the detected one."),
        toolPath("tools.ninja.path", "Ninja", "The ninja executable; empty uses the detected one."),
        toolPath("tools.git.path", "Git", "The git executable; empty uses the detected one."),
        toolPath("tools.managed.dotnetPath", "Managed .NET SDK", "Absolute dotnet executable for C# language service."),
        toolPath("tools.managed.serverAssembly", "C# language server", "Absolute csharp-ls server assembly path."),
        toolPath("tools.luaLanguageServer.path", "Lua language server",
                 "Absolute LuaLS executable path; empty searches PATH."),
        toolPath("tools.clangd.path", "clangd", "The clangd executable; empty uses the bundled one."),
        toolPath("tools.clangFormat.path", "clang-format", "The clang-format executable; empty uses the bundled one."),
        toolPath("tools.clangTidy.path", "clang-tidy", "The clang-tidy executable; empty uses the bundled one."),
        {"tools.clangTidy.plugins", "clang-tidy plugins", "Modules clang-tidy loads with --load.",
         "Build, Execution, Deployment/Toolchains", stringListSchema(32, 4096, StringHint::Executable), emptyList(),
         kApplicationOnly, true, true},
        toolPath("tools.debugger.path", "Debugger", "The debug adapter executable; empty uses the bundled one."),
        toolPath("tools.lldbDap.path", "lldb-dap", "The lldb-dap executable; empty uses the SDK-pinned one."),
        toolPath("tools.gdb.path", "GDB",
                 "Absolute user-installed GNU GDB >=16 executable with Python/DAP support (Linux)."),
        toolPath("tools.dbgEng.path", "Frost DbgEng adapter",
                 "Absolute frost-dbgeng-dap executable; empty uses the staged Windows adapter."),
        {"debug.nativeBackend", "Native debugger", "Backend used for ordinary native launch and attach sessions.",
         "Build, Execution, Deployment/Debugger",
#ifdef __linux__
         enumSchema(options({{"lldb-dap", "LLDB"}, {"gdb", "GDB (Linux, version 16 or newer)"}})),
#elif defined(_WIN32)
         enumSchema(options({{"lldb-dap", "LLDB"}, {"frost-dbgeng", "Frost DbgEng"}})),
#else
         enumSchema(options({{"lldb-dap", "LLDB"}})),
#endif
         text("lldb-dap"), kApplicationOnly, true},
        {"debug.sourceMap.from", "Debug source build root",
         "Translate this build-time source root to the local project root. This does not verify source contents.",
         "Build, Execution, Deployment/Debugger", stringSchema(4096, StringHint::Path), text(""), kApplicationOnly},
        {"debug.sourceMap.to", "Debug source project root",
         "Trusted local project root for source path translation; leave both fields empty to disable.",
         "Build, Execution, Deployment/Debugger", stringSchema(4096, StringHint::Path), text(""), kApplicationOnly},

        // Terminal (ADR 0051).
        {"terminal.shell", "Shell path",
         "The shell the terminal starts, as an absolute path; empty uses PowerShell 7, Windows PowerShell or "
         "cmd.exe on Windows and $SHELL or /bin/bash on Linux.",
         "Tools/Terminal", stringSchema(4096, StringHint::Executable), text(""), kApplicationOnly, true},
        {"terminal.scrollbackLines", "Scrollback buffer", "How many lines each terminal tab keeps above the screen.",
         "Tools/Terminal", integerSchema(0, 1'000'000), integer(10000), kApplicationOnly},
    };
}

[[nodiscard]] Registry build() {
    Registry registry;
    for (Entry& entry : entries()) {
        auto added = registry.add(KeySpec{.key = entry.key,
                                          .title = entry.title,
                                          .description = entry.description,
                                          .category = entry.category,
                                          .schema = std::move(entry.schema),
                                          .defaultValue = std::move(entry.defaultValue),
                                          .scopes = entry.scopes,
                                          .executes = entry.executes,
                                          .advanced = entry.advanced});
        FROST_ASSERT(added.has_value()); // SettingsRegistry.BuiltinKeysAreValid names the entry.
    }
    return registry;
}

} // namespace

const Registry& builtinRegistry() {
    // Built on first use and never destroyed (no exit-time destructor; startup hygiene, PLAN.md 2.1).
    static const Registry* const registry = new Registry(build());
    return *registry;
}

} // namespace frost::settings
