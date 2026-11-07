// SPDX-License-Identifier: Apache-2.0
//
// UX-1 keymap coverage: every action id a built-in keymap (IntelliJ, Visual Studio) binds on a
// supported platform (Windows, Linux) resolves to an action the workbench registers, so no
// shortcut is silently dead. The ids still missing are listed in kKnownGaps, owned by UX-3; the
// list may only shrink: the test fails when a new binding has no action, when a listed id gets
// registered (remove it from the list) and when a listed id is no longer bound anywhere. The
// registry is the real one of a WorkbenchRig (offscreen, a scratch work directory, no project).

#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"

#include "app/workbench_rig.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/*
 * Bound ids with no registered action yet, sorted. Owner: UX-3 (implement the action or drop the
 * binding). Never add to this list: a new binding needs its action in the same change.
 */
constexpr auto kKnownGaps = std::to_array<std::string_view>({
    "agent.atMention",
    "bookmarks.goto1",
    "bookmarks.goto2",
    "bookmarks.goto3",
    "bookmarks.goto4",
    "bookmarks.goto5",
    "bookmarks.goto6",
    "bookmarks.goto7",
    "bookmarks.goto8",
    "bookmarks.goto9",
    "bookmarks.toggle1",
    "bookmarks.toggle2",
    "bookmarks.toggle3",
    "bookmarks.toggle4",
    "bookmarks.toggle5",
    "bookmarks.toggle6",
    "bookmarks.toggle7",
    "bookmarks.toggle8",
    "bookmarks.toggle9",
    "bookmarks.toggleMnemonic",
    "build.project",
    "code.reformatFile",
    "code.smartCompletion",
    "debug.smartStepInto",
    "debug.viewBreakpoints",
    "editor.autoIndentLines",
    "editor.codeBlockEnd",
    "editor.codeBlockEndWithSelection",
    "editor.codeBlockStart",
    "editor.codeBlockStartWithSelection",
    "editor.findWordAtCaret",
    "editor.moveStatementDown",
    "editor.moveStatementUp",
    "editor.pageBottom",
    "editor.pageBottomWithSelection",
    "editor.pageTop",
    "editor.pageTopWithSelection",
    "editor.pasteFromHistory",
    "editor.pasteWithoutFormatting",
    "editor.splitLine",
    "editor.startNewLineBefore",
    "editor.toggleOverwrite",
    "navigate.matchingBrace",
    "navigate.selectIn",
    "refactor.copy",
    "run.debugContext",
    "run.runContext",
    "tools.runAnything",
    "vcs.quickPopup",
    "window.bookmarks",
    "window.git",
    "window.maximizeToolWindow",
});
// The UX-1 baseline: the list never grows past it.
static_assert(kKnownGaps.size() <= 52, "kKnownGaps only shrinks: register the action instead");

// The supported platforms (macOS is unsupported until a test leg exists).
constexpr std::array kPlatforms{frost::input::KeymapPlatform::Windows, frost::input::KeymapPlatform::Linux};

// Every id bound to at least one sequence, with the keymaps that bind it ("intellij/windows", ...).
std::map<std::string, std::set<std::string>> boundIds() {
    std::map<std::string, std::set<std::string>> out;
    for (const std::string_view id : frost::input::builtinKeymapIds()) {
        auto keymap = frost::input::builtinKeymap(id);
        EXPECT_TRUE(keymap.has_value()) << id << ": " << keymap.error().describe();
        if (!keymap) {
            continue;
        }
        for (const frost::input::KeymapPlatform platform : kPlatforms) {
            const std::string where =
                std::string(id) + (platform == frost::input::KeymapPlatform::Windows ? "/windows" : "/linux");
            for (const frost::input::Binding& binding : keymap->bindings(platform)) {
                if (!binding.sequences.empty()) {
                    out[binding.actionId].insert(where);
                }
            }
        }
    }
    return out;
}

TEST(KeymapCoverage, EveryBuiltinBindingResolvesToARegisteredAction) {
    ASSERT_TRUE(std::ranges::is_sorted(kKnownGaps));
    ASSERT_EQ(std::ranges::adjacent_find(kKnownGaps), kKnownGaps.end());
    frost::app::test::WorkDir dir("keymap-coverage");
    // The product's defaults register the Git actions (no project is opened, so git never runs);
    // clangd stays off, its actions are registered without a server.
    frost::app::test::RigOptions options;
    options.workbench.git = true;
    frost::app::test::WorkbenchRig rig(dir, std::move(options));
    const frost::input::ActionRegistry& registry = rig.workbench->registry();
    const auto bound = boundIds();
    ASSERT_FALSE(bound.empty());

    std::vector<std::string> unregistered;
    for (const auto& [id, keymaps] : bound) {
        const bool registered = registry.find(id).has_value();
        const bool known = std::ranges::binary_search(kKnownGaps, std::string_view(id));
        if (!registered && !known) {
            std::string where;
            for (const std::string& keymap : keymaps) {
                where += (where.empty() ? "" : ", ") + keymap;
            }
            ADD_FAILURE() << id << " is bound (" << where << ") but no action is registered";
        }
        if (!registered) {
            unregistered.push_back(id);
        }
    }
    for (const std::string_view gap : kKnownGaps) {
        EXPECT_FALSE(registry.find(gap).has_value())
            << gap << " is registered now: remove it from kKnownGaps (the list only shrinks)";
        EXPECT_TRUE(bound.contains(std::string(gap)))
            << gap << " is no longer bound by any built-in keymap: remove it from kKnownGaps";
    }
    // The current list, for the failure output when it has to change.
    std::string list;
    for (const std::string& id : unregistered) {
        list += "    \"" + id + "\",\n";
    }
    EXPECT_EQ(unregistered.size(), kKnownGaps.size())
        << "unregistered bound ids (" << unregistered.size() << " of " << bound.size() << "):\n"
        << list;
}

} // namespace
