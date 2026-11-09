// SPDX-License-Identifier: Apache-2.0
//
// UX-21: the built-in Visual Studio 2022 and ReSharper keymaps (src/input/src/keymap_visual_studio.cpp).
// Both parse; their shortcuts for the commands a Visual Studio user reaches for first equal the
// published defaults (the oracle is the documentation the keymaps cite: Microsoft Learn "Keyboard
// shortcuts in Visual Studio" and JetBrains ReSharper Help "Keyboard shortcuts", Visual Studio
// scheme, transcribed into kDocumented below); no single-stroke shortcut is shadowed by a two-stroke
// prefix (which would make a documented shortcut dead); the only sequences bound to two actions are
// the ones the documentation gives two meanings; and no binding swallows plain typing. That every
// bound id is a registered action is tests/input/keymap_coverage_test.cpp's (UX-1).

#include "frost/input/keymap.hpp"

#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {
namespace {

constexpr std::array kSupported = {KeymapPlatform::Windows, KeymapPlatform::Linux};

struct Documented {
    std::string_view keymap;
    std::string_view action;
    std::vector<std::string_view> shortcuts; // In keymap order; the first is the primary one.
};

// The published defaults, per keymap (see the file comment for the sources).
const std::vector<Documented>& documented() {
    static const std::vector<Documented> table = {
        // Visual Studio 2022 (Microsoft Learn).
        {"visual-studio-2022", "build.buildSolution", {"Ctrl+Shift+B"}},
        {"visual-studio-2022", "build.cancelBuild", {"Ctrl+Pause"}},
        {"visual-studio-2022", "run.debug", {"F5"}},
        {"visual-studio-2022", "run.runWithoutDebugging", {"Ctrl+F5"}},
        {"visual-studio-2022", "run.stop", {"Shift+F5"}},
        {"visual-studio-2022", "run.rerun", {"Ctrl+Shift+F5"}},
        {"visual-studio-2022", "debug.toggleBreakpoint", {"F9"}},
        {"visual-studio-2022", "debug.stepOver", {"F10"}},
        {"visual-studio-2022", "debug.stepInto", {"F11"}},
        {"visual-studio-2022", "debug.stepOut", {"Shift+F11"}},
        {"visual-studio-2022", "debug.runToCursor", {"Ctrl+F10"}},
        {"visual-studio-2022", "debug.setNextStatement", {"Ctrl+Shift+F10"}},
        {"visual-studio-2022", "debug.evaluate", {"Shift+F9", "Ctrl+Alt+Q"}},
        {"visual-studio-2022", "debug.attachToProcess", {"Ctrl+Alt+P"}},
        {"visual-studio-2022", "debug.viewBreakpoints", {"Ctrl+Alt+B"}},
        {"visual-studio-2022", "navigate.declaration", {"F12", "Ctrl+F12"}},
        {"visual-studio-2022", "navigate.findUsages", {"Shift+F12"}},
        {"visual-studio-2022", "navigate.quickDefinition", {"Alt+F12"}},
        {"visual-studio-2022", "navigate.back", {"Ctrl+Minus"}},
        {"visual-studio-2022", "navigate.forward", {"Ctrl+Shift+Minus"}},
        {"visual-studio-2022", "navigate.relatedFiles", {"Ctrl+K, Ctrl+O"}},
        {"visual-studio-2022", "navigate.fileStructure", {"Alt+Backslash", "Ctrl+Digit1, Ctrl+M"}},
        {"visual-studio-2022", "search.everywhere", {"Ctrl+T", "Ctrl+Comma"}},
        {"visual-studio-2022", "search.findInFiles", {"Ctrl+Shift+F"}},
        {"visual-studio-2022", "search.replaceInFiles", {"Ctrl+Shift+H"}},
        {"visual-studio-2022", "editor.gotoLine", {"Ctrl+G"}},
        {"visual-studio-2022", "editor.duplicate", {"Ctrl+D"}},
        {"visual-studio-2022", "editor.deleteLine", {"Ctrl+Shift+L"}},
        {"visual-studio-2022", "editor.moveLineUp", {"Alt+Up"}},
        {"visual-studio-2022", "editor.moveLineDown", {"Alt+Down"}},
        {"visual-studio-2022",
         "editor.commentLine",
         {"Ctrl+Slash", "Ctrl+K, Ctrl+Slash", "Ctrl+K, Ctrl+C", "Ctrl+K, Ctrl+U"}},
        {"visual-studio-2022", "editor.foldExpandAll", {"Ctrl+M, Ctrl+X"}},
        {"visual-studio-2022", "code.reformat", {"Ctrl+K, Ctrl+F"}},
        {"visual-studio-2022", "code.reformatFile", {"Ctrl+K, Ctrl+D"}},
        {"visual-studio-2022", "code.showIntentions", {"Ctrl+Period", "Alt+Shift+F10"}},
        {"visual-studio-2022", "code.quickDocumentation", {"Ctrl+K, Ctrl+I"}},
        {"visual-studio-2022", "refactor.rename", {"Ctrl+R, Ctrl+R"}},
        {"visual-studio-2022", "window.project", {"Ctrl+Alt+L"}},
        {"visual-studio-2022", "window.problems", {"Ctrl+Backslash, Ctrl+E", "Ctrl+Backslash, E"}},
        {"visual-studio-2022", "window.closeTab", {"Ctrl+F4", "Ctrl+W"}},
        {"visual-studio-2022", "file.saveDocument", {"Ctrl+S"}},
        {"visual-studio-2022", "file.saveAll", {"Ctrl+Shift+S"}},
        {"visual-studio-2022", "bookmarks.toggle", {"Ctrl+K, Ctrl+K"}},
        // ReSharper, Visual Studio scheme (JetBrains ReSharper Help), on top of Visual Studio 2022.
        {"resharper", "code.showIntentions", {"Alt+Enter", "Ctrl+Period"}},
        {"resharper", "refactor.this", {"Ctrl+Shift+R"}},
        {"resharper", "refactor.rename", {"Ctrl+R, R", "Ctrl+R, Ctrl+R"}},
        {"resharper", "refactor.move", {"Ctrl+R, O"}},
        {"resharper", "refactor.safeDelete", {"Ctrl+R, D"}},
        {"resharper", "refactor.extractVariable", {"Ctrl+R, V"}},
        {"resharper", "refactor.inline", {"Ctrl+R, I"}},
        {"resharper", "refactor.changeSignature", {"Ctrl+R, S"}},
        {"resharper", "search.everywhere", {"Ctrl+T"}},
        {"resharper", "search.findAction", {"Ctrl+Shift+A", "Ctrl+Q"}},
        {"resharper", "navigate.gotoFile", {"Ctrl+Shift+T"}},
        {"resharper", "navigate.gotoSymbol", {"Alt+Shift+T"}},
        {"resharper", "navigate.fileStructure", {"Alt+Backslash"}},
        {"resharper", "navigate.declaration", {"F12"}},
        {"resharper", "navigate.implementation", {"Ctrl+F12"}},
        {"resharper", "navigate.relatedFiles", {"Alt+O", "Ctrl+Alt+F7"}},
        {"resharper", "navigate.superMethod", {"Alt+Home"}},
        {"resharper", "navigate.nextMember", {"Alt+Down"}},
        {"resharper", "navigate.previousMember", {"Alt+Up"}},
        {"resharper", "navigate.recentFiles", {"Ctrl+Comma"}},
        {"resharper", "navigate.findUsages", {"Shift+F12"}},
        {"resharper", "navigate.typeHierarchy", {"Ctrl+E, H"}},
        {"resharper", "code.completeStatement", {"Ctrl+Shift+Enter"}},
        {"resharper", "code.reformat", {"Ctrl+Alt+Enter", "Ctrl+K, Ctrl+F"}},
        {"resharper", "code.generate", {"Alt+Insert"}},
        {"resharper", "editor.extendSelection", {"Ctrl+Alt+Right", "Alt+Shift+Equal"}},
        {"resharper", "editor.joinLines", {"Ctrl+Shift+J"}},
        {"resharper",
         "editor.commentLine",
         {"Ctrl+Alt+Slash", "Ctrl+K, Ctrl+Slash", "Ctrl+K, Ctrl+C", "Ctrl+K, Ctrl+U"}},
        {"resharper", "debug.viewBreakpoints", {"Ctrl+Alt+F9", "Ctrl+Alt+B"}},
        {"resharper", "build.buildSolution", {"Ctrl+Shift+B"}},
        {"resharper", "run.debug", {"F5"}},
        {"resharper", "debug.toggleBreakpoint", {"F9"}},
    };
    return table;
}

// The resolved sequences of `action` in `keymap` for `platform`, formatted.
std::vector<std::string> shortcuts(const Keymap& keymap, KeymapPlatform platform, std::string_view action) {
    std::vector<std::string> out;
    for (const Binding& binding : keymap.bindings(platform))
        if (binding.actionId == action)
            for (const Sequence& sequence : binding.sequences)
                out.push_back(formatSequence(sequence));
    return out;
}

Keymap load(std::string_view id) {
    auto keymap = builtinKeymap(id);
    EXPECT_TRUE(keymap.has_value()) << id << ": " << keymap.error().describe();
    return keymap ? *keymap : Keymap{};
}

TEST(VisualStudioKeymaps, ParseWithTheirIdsAndNames) {
    const Keymap vs = load("visual-studio-2022");
    EXPECT_EQ(vs.id(), "visual-studio-2022");
    EXPECT_EQ(vs.name(), "Visual Studio 2022");
    const Keymap resharper = load("resharper");
    EXPECT_EQ(resharper.id(), "resharper");
    EXPECT_EQ(resharper.name(), "ReSharper (Visual Studio scheme)");
    // The canonical form parses back to the same keymap (format v1's round trip).
    for (const Keymap* keymap : {&vs, &resharper}) {
        auto again = parseKeymap(formatKeymap(*keymap));
        ASSERT_TRUE(again.has_value()) << again.error().describe();
        EXPECT_EQ(*again, *keymap);
    }
}

TEST(VisualStudioKeymaps, TheDocumentedShortcutsAreTheKeymapsOwn) {
    for (const Documented& row : documented()) {
        const Keymap keymap = load(row.keymap);
        const std::vector<std::string> expected(row.shortcuts.begin(), row.shortcuts.end());
        for (const KeymapPlatform platform : kSupported)
            EXPECT_EQ(shortcuts(keymap, platform, row.action), expected)
                << row.keymap << " " << row.action << (platform == KeymapPlatform::Windows ? " (windows)" : " (linux)");
    }
}

// Every conflict of `keymap` on `platform`: "<sequence>[ (prefix)]: <action> <action>...".
std::vector<std::string> conflicts(const Keymap& keymap, KeymapPlatform platform) {
    ActionRegistry registry;
    static_cast<void>(test::registerKeymapActions(registry, keymap));
    const BindingIndex index = BindingIndex::build(keymap, platform, {}, registry);
    std::vector<std::string> out;
    for (const Conflict& conflict : index.conflicts()) {
        std::string line = formatSequence(conflict.sequence) + (conflict.prefixConflict ? " (prefix):" : ":");
        std::vector<std::string> ids;
        for (const ActionId id : conflict.actions)
            ids.push_back(registry.info(id).id);
        std::ranges::sort(ids);
        for (const std::string& id : ids)
            line += " " + id;
        out.push_back(line);
    }
    std::ranges::sort(out);
    return out;
}

TEST(VisualStudioKeymaps, OnlyTheDocumentedDoubleMeaningsConflictAndNoPrefixShadowsAShortcut) {
    // F5 is Start and, while debugging, Continue; Esc cancels the editor's selection and, from a
    // tool window, activates the document window (Window.ActivateDocumentWindow).
    const std::vector<std::string> expected = {"Escape: editor.escape window.focusEditor",
                                               "F5: debug.resume run.debug"};
    for (const std::string_view id : {"visual-studio-2022", "resharper"}) {
        const Keymap keymap = load(id);
        for (const KeymapPlatform platform : kSupported)
            EXPECT_EQ(conflicts(keymap, platform), expected) << id;
    }
}

TEST(VisualStudioKeymaps, NoBindingSwallowsTyping) {
    for (const std::string_view id : {"visual-studio-2022", "resharper"}) {
        const Keymap keymap = load(id);
        for (const KeymapPlatform platform : kSupported)
            for (const Binding& binding : keymap.bindings(platform)) {
                EXPECT_TRUE(isValidActionId(binding.actionId)) << binding.actionId;
                for (const Sequence& sequence : binding.sequences) {
                    ASSERT_TRUE(sequence.valid()) << id << " " << binding.actionId;
                    if (!sequence.isGesture())
                        EXPECT_FALSE(isTypingKey(sequence.first().key) && !sequence.first().mods.hasCommandModifier())
                            << id << ": " << binding.actionId << " = " << formatSequence(sequence);
                }
            }
    }
}

TEST(VisualStudioKeymaps, LeaveTheDefaultKeymapAlone) {
    // UX-21's picker offers these keymaps; it never changes the default (Settings > Keymap).
    EXPECT_EQ(KeymapPreferences{}.keymapId, "intellij");
    KeymapPreferences preferences;
    for (const std::string_view id : {"visual-studio-2022", "resharper"}) {
        preferences.keymapId = std::string(id);
        EXPECT_TRUE(validate(preferences).has_value()) << id;
    }
}

} // namespace
} // namespace frost::input
