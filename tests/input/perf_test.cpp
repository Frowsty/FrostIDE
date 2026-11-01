// SPDX-License-Identifier: Apache-2.0
//
// InputPerf: timing reports for the keymap start-up cost (Budget: B1), the matcher's
// per-event cost (Budget: B5) and a Find Action query. Generous bounds, asserted only in
// optimised, unsanitised builds (kAssertBudgets); otherwise reported as test properties.

#include "frost/input/find_action.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/shortcut_matcher.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/test_actions.hpp"
#include "sched/sched_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <vector>

namespace frost::input {
namespace {

using Clock = std::chrono::steady_clock;

double medianOf(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

void report(std::string_view name, double value, double bound, std::string_view unit) {
    ::testing::Test::RecordProperty(std::string(name), std::format("{:.3f}", value));
    std::cout << std::format("[ perf ] {} = {:.3f} {} (bound {:.3f} {}{})\n", name, value, unit, bound, unit,
                             sched::test::kAssertBudgets ? ", asserted" : ", reported only");
    if (sched::test::kAssertBudgets) {
        EXPECT_LE(value, bound) << name;
    }
}

TEST(InputPerf, KeymapAndIndexBuild) {
    ActionRegistry registry;
    static_cast<void>(test::registerKeymapActions(registry, test::intellij()));
    std::vector<double> samples;
    std::size_t bindings = 0;
    for (int i = 0; i < 21; ++i) {
        const auto start = Clock::now();
        auto keymap = builtinKeymap("intellij");
        ASSERT_TRUE(keymap.has_value());
        const BindingIndex index = BindingIndex::build(*keymap, KeymapPlatform::Windows, {}, registry);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        bindings = index.size();
    }
    EXPECT_GT(bindings, 200U);
    report("keymap_and_index_build_ms", medianOf(samples), 1.0, "ms"); // Budget: B1.
}

// An imported keymap is untrusted input (row 227): 80,000 bindings in descending id order
// must parse in about the time ascending ones take (a sorted-vector duplicate check made
// this 63 times slower). Reported as the descending/ascending ratio.
TEST(InputPerf, KeymapParseDescendingIds) {
    constexpr int kCount = 80'000;
    const auto keymapText = [&](bool descending) {
        std::string text = "frost-keymap 1\nid x\nname X\n[default]\n";
        for (int i = 0; i < kCount; ++i) {
            text += std::format("a.x{:05} =\n", descending ? kCount - i : i + 1);
        }
        return text;
    };
    const auto timeParse = [](const std::string& text) {
        std::vector<double> samples;
        for (int i = 0; i < 5; ++i) {
            const auto start = Clock::now();
            const auto keymap = parseKeymap(text);
            samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            EXPECT_TRUE(keymap.has_value());
        }
        return medianOf(samples);
    };
    const double ascending = timeParse(keymapText(false));
    const double descending = timeParse(keymapText(true));
    std::cout << std::format("[ perf ] keymap parse of {} bindings: ascending {:.1f} ms, descending {:.1f} ms\n",
                             kCount, ascending, descending);
    report("keymap_parse_descending_over_ascending", descending / ascending, 3.0, "x");
}

TEST(InputPerf, MatcherFeed) {
    ActionRegistry registry;
    const Keymap keymap = test::intellij();
    static_cast<void>(test::registerKeymapActions(registry, keymap));
    const BindingIndex index = BindingIndex::build(keymap, KeymapPlatform::Windows, {}, registry);
    ManualClock clock;
    SimulatedKeyboard keyboard(simulatedLayout(SimulatedLayoutId::DeDe), KeyPlatform::Windows, clock);
    std::vector<KeyEvent> events;
    const auto add = [&](std::vector<KeyEvent> more) { events.insert(events.end(), more.begin(), more.end()); };
    for (const Code code : {Code::KeyA, Code::KeyS, Code::KeyL, Code::KeyY, Code::Slash, Code::F7, Code::Backquote}) {
        add(keyboard.tap(code));
        add(keyboard.chord({Code::ControlLeft}, code));
        add(keyboard.chord({Code::ControlLeft, Code::AltLeft}, code));
        add(keyboard.chord({Code::AltRight}, code));
    }
    const LayoutTable& layout = simulatedLayout(SimulatedLayoutId::DeDe);
    const auto gate = [](ActionId) { return true; };
    ShortcutMatcher matcher;
    constexpr std::size_t kEvents = 1'000'000;
    constexpr std::size_t kBatch = 10'000;
    std::vector<double> samples;
    std::size_t fires = 0;
    for (std::size_t done = 0; done < kEvents; done += kBatch) {
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kBatch; ++i) {
            fires += matcher.feed(events[(done + i) % events.size()], index, layout, gate).outcome == MatchOutcome::Fire
                         ? 1U
                         : 0U;
        }
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count() / kBatch);
    }
    EXPECT_GT(fires, 0U);
    report("matcher_feed_us", medianOf(samples), 2.0, "us"); // Budget: B5.
}

TEST(InputPerf, FindActionQuery) {
    ActionRegistry registry;
    const std::vector<std::string> words = {"Reformat", "Code",   "Copy", "Paste", "Go",    "To",   "File",
                                            "Class",    "Symbol", "Run",  "Debug", "Build", "Next", "Error"};
    for (std::size_t i = 0; i < 500; ++i) {
        ActionSpec spec;
        spec.id = std::format("perf.action{}", i);
        spec.name = std::format("{} {} {} {}", words[i % words.size()], words[(i / 3) % words.size()],
                                words[(i / 7) % words.size()], i);
        spec.synonyms = {std::format("{} alias", words[(i / 5) % words.size()])};
        spec.handler = [](const ActionContext&, const InvokeInfo&) {};
        ASSERT_TRUE(registry.add(std::move(spec)).has_value());
    }
    const BindingIndex index = BindingIndex::build(test::intellij(), KeymapPlatform::Windows, {}, registry);
    const FindActionModel model(registry);
    std::vector<double> samples;
    std::size_t hits = 0;
    for (int i = 0; i < 21; ++i) {
        const auto start = Clock::now();
        for (const std::string_view q : {"rc", "go to file", "copy", "ctrl+shift+a", "err"}) {
            hits += model.query(q, index, test::allContexts()).size();
        }
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count() / 5.0);
    }
    EXPECT_GT(hits, 0U);
    report("find_action_query_ms", medianOf(samples), 2.0, "ms");
}

} // namespace
} // namespace frost::input
