// SPDX-License-Identifier: Apache-2.0
//
// KeymapReachability: every default shortcut of the built-in IntelliJ keymap, per action and
// per sequence, on the simulated layouts and both platforms (ADR 0030 decision 4; the user's
// "Support german keyboard layout", ADR 0016 user decisions). de-DE and en-US must be
// complete on Windows and on Linux under its M1 mapping (hard asserts, so regenerating the
// golden cannot hide a gap); pl-PL and fr-FR are compared with an in-code list of expected
// gaps (empty). GermanDailyShortcuts presses the user's most-used shortcuts on the physical
// de-DE keys (ADR 0032). Wayland shares the X11 code paths of the simulator and the mapping (every
// non-Windows branch tests only `== Windows`), so X11 covers Linux. The golden is a
// consistency check that pins the counts and the informational dead-key strokes.

#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/golden.hpp"
#include "input/support/matcher_rig.hpp"
#include "input/support/reachability.hpp"
#include "input/support/synthetic_layouts.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {
namespace {

using test::analyzeReachability;
using test::GapKind;
using test::ReachabilityGap;
using test::ReachabilityReport;

constexpr std::array kPlatforms = {KeyPlatform::Windows, KeyPlatform::X11};

std::string gapsText(const ReachabilityReport& report) { return report.format(); }

void expectComplete(SimulatedLayoutId id) {
    for (const KeyPlatform platform : kPlatforms) {
        const ReachabilityReport report = analyzeReachability(simulatedLayout(id), platform);
        SCOPED_TRACE(gapsText(report));
        EXPECT_GT(report.actions, 150U) << "not vacuous";
        EXPECT_GT(report.sequences, 200U) << "not vacuous";
        EXPECT_TRUE(report.gaps.empty());
        EXPECT_TRUE(report.numpadOnly.empty()) << "a shortcut reachable only on the numpad is not complete support";
    }
}

// The user's requirement: every default shortcut, comment line and block included, on de-DE.
TEST(KeymapReachability, GermanIsComplete) { expectComplete(SimulatedLayoutId::DeDe); }

TEST(KeymapReachability, EnglishIsComplete) { expectComplete(SimulatedLayoutId::EnUs); }

/*
 * The shortcuts the user uses most (ADR 0032: FrostIDE is their daily IDE on a German
 * keyboard), pressed on the physical de-DE keys a German typist uses, on Windows and on
 * Linux under its M1 mapping: Build, comment line and block (the '-' key right of '.'),
 * find, go to file, navigate back and forward, duplicate line, move line and statement,
 * Find Action, plus undo/redo (QWERTZ: the key labelled Z) and Save All. Build Solution is
 * build.project (Ctrl+F9) until the W1 build slice registers its actions.
 */
TEST(KeymapReachability, GermanDailyShortcuts) {
    struct Press {
        Modifiers mods;
        Code code;
        std::string_view action;
    };
    const Modifiers ctrl = Modifier::Ctrl;
    const Modifiers ctrlShift = Modifier::Ctrl | Modifier::Shift;
    const Modifiers ctrlAlt = Modifier::Ctrl | Modifier::Alt;
    const Modifiers altShift = Modifier::Alt | Modifier::Shift;
    const std::array presses = {
        Press{ctrl, Code::F9, "build.project"},
        Press{ctrl, Code::Slash, "editor.commentLine"},
        Press{ctrlShift, Code::Slash, "editor.commentBlock"},
        Press{ctrl, Code::NumpadDivide, "editor.commentLine"},
        Press{ctrl, Code::KeyF, "editor.find"},
        Press{ctrlShift, Code::KeyF, "search.findInFiles"},
        Press{ctrlShift, Code::KeyN, "navigate.gotoFile"},
        Press{ctrlAlt, Code::ArrowLeft, "navigate.back"},
        Press{ctrlAlt, Code::ArrowRight, "navigate.forward"},
        Press{ctrl, Code::KeyD, "editor.duplicate"},
        Press{altShift, Code::ArrowUp, "editor.moveLineUp"},
        Press{altShift, Code::ArrowDown, "editor.moveLineDown"},
        Press{ctrlShift, Code::ArrowUp, "editor.moveStatementUp"},
        Press{ctrlShift, Code::ArrowDown, "editor.moveStatementDown"},
        Press{ctrlShift, Code::KeyA, "search.findAction"},
        Press{ctrl, Code::KeyY, "editor.undo"},
        Press{ctrlShift, Code::KeyY, "editor.redo"},
        Press{ctrl, Code::KeyS, "file.saveAll"},
    };
    for (const KeyPlatform platform : kPlatforms) {
        test::MatcherRig rig(simulatedLayout(SimulatedLayoutId::DeDe), platform);
        for (const Press& p : presses) {
            rig.resetKeyboard();
            EXPECT_EQ(rig.stroke(p.mods, p.code).fired, p.action)
                << test::keyPlatformName(platform) << " " << codeName(p.code);
        }
        // Search Everywhere: two taps of Shift.
        rig.resetKeyboard();
        static_cast<void>(rig.tap(Code::ShiftLeft));
        EXPECT_EQ(rig.tap(Code::ShiftLeft).fired, "search.everywhere") << test::keyPlatformName(platform);
        if (platform == KeyPlatform::Windows) {
            // AltGr (right Alt) with an arrow produces nothing, so it acts as Ctrl+Alt.
            rig.resetKeyboard();
            EXPECT_EQ(rig.chord({Code::AltRight}, Code::ArrowLeft).fired, "navigate.back");
            // Menus show what the keycaps say.
            const auto shown = [&](std::string_view action) {
                const std::optional<Sequence> s = rig.index.primaryShortcut(rig.idOf(action), *rig.table);
                return s ? toDisplayText(*s, *rig.table, KeymapPlatform::Windows) : std::string("(none)");
            };
            EXPECT_EQ(shown("editor.commentLine"), "Ctrl+-");
            EXPECT_EQ(shown("editor.commentBlock"), "Ctrl+Shift+-");
            EXPECT_EQ(shown("navigate.gotoFile"), "Ctrl+Shift+N");
            EXPECT_EQ(shown("build.project"), "Ctrl+F9");
        }
    }
}

// pl-PL and fr-FR are reported; their expected gaps are listed here (none today).
TEST(KeymapReachability, PolishAndFrench) {
    for (const SimulatedLayoutId id : {SimulatedLayoutId::PlPl, SimulatedLayoutId::FrFr}) {
        for (const KeyPlatform platform : kPlatforms) {
            const ReachabilityReport report = analyzeReachability(simulatedLayout(id), platform);
            SCOPED_TRACE(gapsText(report));
            const std::vector<ReachabilityGap> expected;
            EXPECT_EQ(report.gaps, expected);
            EXPECT_TRUE(report.numpadOnly.empty());
        }
    }
}

// Eight blocks, {en-US, de-DE, pl-PL, fr-FR} x {Windows, X11}.
TEST(KeymapReachability, Golden) {
    std::string out = "# KeymapReachability over the built-in IntelliJ keymap. Consistency golden: the simulated "
                      "layouts, SimulatedKeyboard and the matcher; dead-key strokes are informational.\n";
    for (const SimulatedLayoutId id :
         {SimulatedLayoutId::EnUs, SimulatedLayoutId::DeDe, SimulatedLayoutId::PlPl, SimulatedLayoutId::FrFr}) {
        for (const KeyPlatform platform : kPlatforms) {
            out += analyzeReachability(simulatedLayout(id), platform, {}, simulatedLayoutName(id)).format();
        }
    }
    test::expectGolden("keymap-reachability.txt", out);
}

// Non-vacuity: on a layout with no '/' the suite reports both comment shortcuts as
// unreachable and the comment actions as numpad-only.
TEST(KeymapReachability, DetectsGaps) {
    const LayoutTable noSlash = test::synthetic::noSlash();
    const ReachabilityReport report = analyzeReachability(noSlash, KeyPlatform::Windows);
    SCOPED_TRACE(gapsText(report));
    const std::vector<ReachabilityGap> expected = {
        {"editor.commentBlock", "Ctrl+Shift+Slash", GapKind::Unreachable, "no key maps to Slash"},
        {"editor.commentLine", "Ctrl+Slash", GapKind::Unreachable, "no key maps to Slash"},
    };
    EXPECT_EQ(report.gaps, expected);
    EXPECT_EQ(report.numpadOnly, (std::vector<std::string>{"editor.commentBlock", "editor.commentLine"}));

    // A bare dead key as a second stroke composes instead of matching (de '´' on Equal):
    // reported as a Misread, and its press as a dead-key stroke. Two actions on one stroke
    // are a conflict, not a gap.
    constexpr std::string_view kKeymap = "frost-keymap 1\nid synthetic\nname Synthetic\n[default]\n"
                                         "test.deadSecond = Ctrl+K, Equal\n"
                                         "test.alone = Ctrl+Shift+F5\n"
                                         "test.shadowed = Ctrl+Shift+F5\n";
    const ReachabilityReport synthetic =
        analyzeReachability(simulatedLayout(SimulatedLayoutId::DeDe), KeyPlatform::Windows, kKeymap, "synthetic");
    SCOPED_TRACE(gapsText(synthetic));
    const std::vector<ReachabilityGap> misread = {
        {"test.deadSecond", "Ctrl+K, Equal", GapKind::Misread, "Equal read as nothing"}};
    EXPECT_EQ(synthetic.gaps, misread);
    EXPECT_EQ(synthetic.deadKeyStrokes, std::vector<std::string>{"Equal test.deadSecond"});
    EXPECT_EQ(synthetic.actions, 3U);
    EXPECT_EQ(synthetic.sequences, 3U);
}

} // namespace
} // namespace frost::input
