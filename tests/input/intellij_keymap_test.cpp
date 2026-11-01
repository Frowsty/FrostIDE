// SPDX-License-Identifier: Apache-2.0
//
// IntelliJKeymap (PLAN.md row 227 acceptance): the built-in IntelliJ-compatible keymap
// parses, its resolved bindings per platform match the reviewed goldens, its action ids
// follow ADR 0016's convention, the macOS resolution is defined for every action (no macOS
// behaviour is claimed: D24), and its conflicts equal the reviewed lists.

#include "frost/input/keymap.hpp"

#include "input/support/golden.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {
namespace {

std::string platformName(KeymapPlatform platform) {
    switch (platform) {
    case KeymapPlatform::Windows: return "windows";
    case KeymapPlatform::Linux: return "linux";
    case KeymapPlatform::MacOS: return "macos";
    }
    return "?";
}

constexpr std::array kPlatforms = {KeymapPlatform::Windows, KeymapPlatform::Linux, KeymapPlatform::MacOS};

std::string resolvedText(const Keymap& keymap, KeymapPlatform platform) {
    std::string out = std::format("# The built-in IntelliJ keymap resolved for {}. Consistency golden: review every "
                                  "change against the JetBrains default keymap reference.\n",
                                  platformName(platform));
    for (const Binding& binding : keymap.bindings(platform)) {
        out += binding.actionId + " =";
        for (std::size_t i = 0; i < binding.sequences.size(); ++i) {
            out += (i == 0 ? " " : " | ") + formatSequence(binding.sequences[i]);
        }
        out += '\n';
    }
    return out;
}

std::vector<std::string> conflictList(KeymapPlatform platform) {
    ActionRegistry registry;
    const Keymap keymap = test::intellij();
    static_cast<void>(test::registerKeymapActions(registry, keymap));
    const BindingIndex index = BindingIndex::build(keymap, platform, {}, registry);
    std::vector<std::string> out;
    for (const Conflict& conflict : index.conflicts()) {
        std::string line = formatSequence(conflict.sequence) + (conflict.prefixConflict ? " (prefix):" : ":");
        for (const ActionId id : conflict.actions) {
            line += " " + registry.info(id).id;
        }
        out.push_back(line);
    }
    return out;
}

TEST(IntelliJKeymap, Parses) {
    auto keymap = builtinKeymap("intellij");
    ASSERT_TRUE(keymap.has_value()) << keymap.error().message();
    EXPECT_EQ(keymap->id(), "intellij");
    EXPECT_FALSE(keymap->name().empty());
    EXPECT_TRUE(keymap->hasSection(KeymapSection::Default));
    EXPECT_GT(keymap->section(KeymapSection::Default).size(), 200U);
}

TEST(IntelliJKeymap, ResolvedGoldens) {
    const Keymap keymap = test::intellij();
    for (const KeymapPlatform platform : kPlatforms) {
        SCOPED_TRACE(platformName(platform));
        test::expectGolden(std::format("intellij-{}.txt", platformName(platform)), resolvedText(keymap, platform));
    }
}

TEST(IntelliJKeymap, ActionIdsFollowTheConvention) {
    const std::set<std::string_view> groups = {"editor", "code",  "search",    "navigate", "refactor",
                                               "window", "file",  "view",      "vcs",      "run",
                                               "debug",  "build", "bookmarks", "tools",    "agent"};
    const Keymap keymap = test::intellij();
    for (const KeymapSection section :
         {KeymapSection::Default, KeymapSection::Windows, KeymapSection::Linux, KeymapSection::MacOS}) {
        for (const Binding& binding : keymap.section(section)) {
            EXPECT_TRUE(isValidActionId(binding.actionId)) << binding.actionId;
            const std::string_view id = binding.actionId;
            const std::size_t dot = id.find('.');
            ASSERT_NE(dot, std::string_view::npos) << id;
            EXPECT_TRUE(groups.contains(id.substr(0, dot))) << id;
            EXPECT_EQ(id.find('.', dot + 1), std::string_view::npos) << "group.name, one dot: " << id;
        }
    }
}

TEST(IntelliJKeymap, MacOSResolutionIsDefinedForEveryAction) {
    const Keymap keymap = test::intellij();
    std::set<std::string> resolved;
    for (const Binding& binding : keymap.bindings(KeymapPlatform::MacOS)) {
        EXPECT_TRUE(resolved.insert(binding.actionId).second) << "listed once: " << binding.actionId;
        for (const Sequence& s : binding.sequences) {
            EXPECT_TRUE(s.valid()) << binding.actionId;
        }
    }
    for (const Binding& binding : keymap.section(KeymapSection::Default)) {
        EXPECT_TRUE(resolved.contains(binding.actionId)) << binding.actionId;
    }
    // Windows and Linux resolve the same actions as [default].
    for (const KeymapPlatform platform : {KeymapPlatform::Windows, KeymapPlatform::Linux}) {
        EXPECT_EQ(keymap.bindings(platform).size(), keymap.section(KeymapSection::Default).size());
    }
}

TEST(IntelliJKeymap, ConflictsOnWindowsAndLinux) {
    const std::vector<std::string> expected = {"Escape: editor.escape window.focusEditor",
                                               "Alt+Insert: code.generate file.new"};
    for (const KeymapPlatform platform : {KeymapPlatform::Windows, KeymapPlatform::Linux}) {
        std::vector<std::string> actual = conflictList(platform);
        std::vector<std::string> sortedExpected = expected;
        std::ranges::sort(actual);
        std::ranges::sort(sortedExpected);
        EXPECT_EQ(actual, sortedExpected)
            << platformName(platform) << ": both pairs are contextual (editor vs tool window, editor vs project view)";
    }
}

TEST(IntelliJKeymap, MacOSConflicts) {
    // The reviewed allowlist: every collision the Ctrl/Meta derivation could cause is resolved
    // in [macos]; only the contextual pairs may remain.
    const std::set<std::string> allowlist = {"Escape: editor.escape window.focusEditor",
                                             "Alt+Insert: code.generate file.new"};
    const std::vector<std::string> actual = conflictList(KeymapPlatform::MacOS);
    for (const std::string& line : actual) {
        EXPECT_TRUE(allowlist.contains(line)) << "unresolved macOS conflict: " << line;
    }
    EXPECT_NE(std::ranges::find(actual, "Escape: editor.escape window.focusEditor"), actual.end());
}

TEST(IntelliJKeymap, NoBindingSwallowsTyping) {
    const Keymap keymap = test::intellij();
    for (const KeymapPlatform platform : kPlatforms) {
        for (const Binding& binding : keymap.bindings(platform)) {
            for (const Sequence& s : binding.sequences) {
                if (s.isGesture()) {
                    continue;
                }
                EXPECT_FALSE(isTypingKey(s.first().key) && !s.first().mods.hasCommandModifier())
                    << platformName(platform) << ": " << binding.actionId << " = " << formatSequence(s);
            }
        }
    }
}

TEST(IntelliJKeymap, ToolbarAndStatusBarFocusLinesExist) {
    const Keymap keymap = test::intellij();
    const auto find = [&](std::string_view id) -> const Binding* {
        for (const Binding& b : keymap.section(KeymapSection::Default)) {
            if (b.actionId == id) {
                return &b;
            }
        }
        return nullptr;
    };
    const Binding* toolbar = find("window.focusToolbar");
    const Binding* statusBar = find("window.focusStatusBar");
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(statusBar, nullptr);
    ASSERT_EQ(toolbar->sequences.size(), 1U);
    ASSERT_EQ(statusBar->sequences.size(), 1U);
    EXPECT_EQ(formatSequence(toolbar->sequences[0]), "Alt+PageUp");
    EXPECT_EQ(formatSequence(statusBar->sequences[0]), "Alt+PageDown");
}

TEST(IntelliJKeymap, SpotChecks) {
    const Keymap keymap = test::intellij();
    const auto shortcuts = [&](KeymapPlatform platform, std::string_view id) {
        std::vector<std::string> out;
        for (const Binding& b : keymap.bindings(platform)) {
            if (b.actionId == id) {
                for (const Sequence& s : b.sequences) {
                    out.push_back(formatSequence(s));
                }
            }
        }
        return out;
    };
    using V = std::vector<std::string>;
    EXPECT_EQ(shortcuts(KeymapPlatform::Windows, "search.everywhere"), V{"DoubleShift"});
    EXPECT_EQ(shortcuts(KeymapPlatform::Windows, "search.findAction"), V{"Ctrl+Shift+A"});
    EXPECT_EQ(shortcuts(KeymapPlatform::Windows, "code.reformat"), V{"Ctrl+Alt+L"});
    EXPECT_EQ(shortcuts(KeymapPlatform::Windows, "editor.commentLine"), (V{"Ctrl+Slash", "Ctrl+NumpadDivide"}));
    EXPECT_EQ(shortcuts(KeymapPlatform::Windows, "agent.terminal"), V{}) << "explicitly unbound";
    EXPECT_EQ(shortcuts(KeymapPlatform::MacOS, "search.findAction"), V{"Shift+Meta+A"}) << "derived";
    EXPECT_EQ(shortcuts(KeymapPlatform::MacOS, "file.settings"), V{"Meta+Comma"}) << "listed in [macos]";
}

} // namespace
} // namespace frost::input
