// SPDX-License-Identifier: Apache-2.0
//
// FindActionModel (row 78 acceptance, model half): a golden ranking over a 40-action
// fixture registry, synonyms, disabled and inapplicable actions, usage, and the
// properties: independence from registration order, valid highlights, and monotonic
// narrowing for name-only queries.

#include "frost/input/find_action.hpp"

#include "input/support/golden.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {
namespace {

struct FixtureAction {
    std::string_view id;
    std::string_view name;
    std::vector<std::string> synonyms;
    ContextRule context;
    AvailabilityLevel level = AvailabilityLevel::L0;
    bool executesProjectCode = false;
    bool placeholder = false;
    bool searchable = true;
};

// 40 actions: names, synonyms, contexts, a level-gated, two project-code, a placeholder,
// a non-searchable and a context-limited action, and non-ASCII names.
std::vector<FixtureAction> fixtureActions() {
    const ContextRule editor{{ContextKey::EditorFocus}, {}};
    return {
        {"editor.copy", "Copy", {"Clipboard copy"}, editor},
        {"editor.copyPath", "Copy Path", {}, {}},
        {"editor.copyReference", "Copy Reference", {}, {}},
        {"editor.paste", "Paste", {}, editor},
        {"editor.pasteFromHistory", "Paste from History...", {}, editor},
        {"editor.cut", "Cut", {}, editor},
        {"editor.undo", "Undo", {}, editor},
        {"code.reformat", "Reformat Code", {"Format document", "Beautify"}, editor},
        {"code.reformatFile", "Reformat File...", {}, {}},
        {"code.optimizeImports", "Optimize Imports", {}, {}},
        {"code.completion", "Basic Completion", {"Autocomplete"}, editor},
        {"code.generate", "Generate...", {}, editor},
        {"refactor.rename", "Rename...", {}, {}},
        {"refactor.extractMethod", "Extract Method...", {}, editor},
        {"refactor.copyClass", "Copy Class...", {}, {}},
        {"search.findAction", "Find Action...", {"Command palette"}, {}},
        {"search.everywhere", "Search Everywhere", {}, {}},
        {"search.find", "Find...", {}, {}},
        {"search.replace", "Replace...", {}, {}},
        {"navigate.gotoClass", "Go to Class...", {}, {}},
        {"navigate.gotoFile", "Go to File...", {}, {}},
        {"navigate.gotoLine", "Go to Line:Column...", {}, editor},
        {"navigate.gotoSymbol", "Go to Symbol...", {}, {}, AvailabilityLevel::L3},
        {"navigate.nextError", "Next Highlighted Error", {}, {}},
        {"editor.lineUp", "Up", {}, editor},
        {"editor.lineEnd", "Move Caret to Line End", {"End"}, editor},
        {"editor.textEnd", "Move Caret to Text End", {}, editor},
        {"editor.moveLineUp", "Move Line Up", {}, editor},
        {"window.project", "Project", {}, {}},
        {"window.terminal", "Terminal", {}, {}},
        {"window.terminalClear", "Clear Terminal Buffer", {}, {{ContextKey::TerminalFocus}, {}}},
        {"vcs.commit", "Commit...", {}, {}},
        {"vcs.commitAndPush", "Commit and Push...", {}, {}},
        {"run.run", "Run", {}, {}, AvailabilityLevel::L0, true},
        {"run.debug", "Debug", {}, {}, AvailabilityLevel::L0, true},
        {"file.settings", "Settings...", {"Preferences", "Options"}, {}},
        {"tools.mueller", "M\xC3\xBCller Tool", {}, {}},
        {"view.overview",
         "\xC3\x9C"
         "bersicht",
         {},
         {}},
        {"debug.toggleBreakpoint", "Toggle Line Breakpoint", {}, {}, AvailabilityLevel::L0, false, true},
        {"tools.hidden", "Hidden Copy", {}, {}, AvailabilityLevel::L0, false, false, false},
    };
}

constexpr std::string_view kFixtureKeymap = "frost-keymap 1\nid fixture\nname Fixture\n[default]\n"
                                            "editor.copy = Ctrl+C | Ctrl+Insert\n"
                                            "editor.copyPath = Ctrl+Shift+C\n"
                                            "editor.copyReference = Ctrl+Alt+Shift+C\n"
                                            "editor.paste = Ctrl+V\n"
                                            "editor.pasteFromHistory = Ctrl+Shift+V\n"
                                            "editor.cut = Ctrl+X\n"
                                            "editor.undo = Ctrl+Z\n"
                                            "code.reformat = Ctrl+Alt+L\n"
                                            "code.reformatFile = Ctrl+Alt+Shift+L\n"
                                            "code.optimizeImports = Ctrl+Alt+O\n"
                                            "code.completion = Ctrl+Space\n"
                                            "code.generate = Alt+Insert\n"
                                            "refactor.rename = Shift+F6\n"
                                            "refactor.extractMethod = Ctrl+Alt+M\n"
                                            "refactor.copyClass = F5\n"
                                            "search.findAction = Ctrl+Shift+A\n"
                                            "search.everywhere = DoubleShift\n"
                                            "search.find = Ctrl+F\n"
                                            "search.replace = Ctrl+R\n"
                                            "navigate.gotoClass = Ctrl+N\n"
                                            "navigate.gotoFile = Ctrl+Shift+N\n"
                                            "navigate.gotoLine = Ctrl+G\n"
                                            "navigate.gotoSymbol = Ctrl+Alt+Shift+N\n"
                                            "navigate.nextError = F2\n"
                                            "editor.lineUp = Up\n"
                                            "editor.lineEnd = End\n"
                                            "editor.textEnd = Ctrl+End\n"
                                            "editor.moveLineUp = Alt+Shift+Up\n"
                                            "window.project = Alt+Digit1\n"
                                            "window.terminal = Alt+F12\n"
                                            "vcs.commit = Ctrl+K\n"
                                            "vcs.commitAndPush = Ctrl+K, Ctrl+P\n"
                                            "run.run = Shift+F10\n"
                                            "run.debug = Shift+F9\n"
                                            "file.settings = Ctrl+Alt+S\n"
                                            "debug.toggleBreakpoint = Ctrl+F8\n"
                                            "tools.hidden = Ctrl+Alt+H\n";

void registerFixture(ActionRegistry& registry, const std::vector<FixtureAction>& actions) {
    for (const FixtureAction& a : actions) {
        ActionSpec spec;
        spec.id = std::string(a.id);
        spec.name = std::string(a.name);
        spec.synonyms = a.synonyms;
        spec.context = a.context;
        spec.requiredLevel = a.level;
        spec.executesProjectCode = a.executesProjectCode;
        spec.searchable = a.searchable;
        if (!a.placeholder) {
            spec.handler = [](const ActionContext&, const InvokeInfo&) {};
        }
        EXPECT_TRUE(registry.add(std::move(spec)).has_value()) << a.id;
    }
}

// The context the golden is taken in: editor focus, project open, level L2, untrusted.
ActionContext fixtureContext() {
    return ActionContext{
        .keys = {ContextKey::EditorFocus, ContextKey::ProjectOpen}, .level = AvailabilityLevel::L2, .trusted = false};
}

struct Fixture {
    explicit Fixture(const std::vector<FixtureAction>& actions = fixtureActions()) {
        registerFixture(registry, actions);
        auto keymap = parseKeymap(kFixtureKeymap);
        EXPECT_TRUE(keymap.has_value());
        index = BindingIndex::build(*keymap, KeymapPlatform::Windows, {}, registry);
    }
    ActionRegistry registry;
    BindingIndex index;
};

std::string_view sourceName(MatchSource source) {
    switch (source) {
    case MatchSource::Name: return "name";
    case MatchSource::Synonym: return "synonym";
    case MatchSource::Shortcut: return "shortcut";
    }
    return "?";
}

// The text a hit matched (name or synonym).
std::string_view hitText(const ActionRegistry& registry, const FindActionHit& hit) {
    const ActionInfo& info = registry.info(hit.id);
    if (hit.source == MatchSource::Synonym) {
        return info.synonyms.at(static_cast<std::size_t>(hit.textIndex) - 1U);
    }
    return info.name;
}

std::string describeHits(const ActionRegistry& registry, std::span<const FindActionHit> hits) {
    std::string out;
    for (const FindActionHit& hit : hits) {
        // The matched text with [ ] around the highlighted ranges; shortcut hits show the action's name.
        std::string marked;
        if (hit.source == MatchSource::Shortcut) {
            marked = "(by shortcut) " + registry.info(hit.id).name;
        } else {
            const std::string_view text = hitText(registry, hit);
            std::size_t at = 0;
            for (std::size_t r = 0; r < hit.highlightCount; ++r) {
                const TextRange range = hit.highlight[r];
                const auto offset = static_cast<std::size_t>(range.offset);
                marked += text.substr(at, offset - at);
                marked += '[';
                marked += text.substr(offset, range.length);
                marked += ']';
                at = offset + range.length;
            }
            marked += text.substr(at);
        }
        out += std::format("  {:>4} {:<8} {:<24} {}{}{}\n", hit.score, sourceName(hit.source), registry.info(hit.id).id,
                           marked, hit.highlightTruncated ? " (truncated)" : "",
                           hit.enabled ? "" : std::format(" [disabled: {}]", toString(hit.disabledReason)));
    }
    return out;
}

std::vector<std::string> queries() {
    return {"copy",   "Copy", "rc",    "refco", "code", "ref co", "cpy", "ctrl+shift+a",        "Ctrl Shift A",
            "ctrl+k", "f2",   "alt+1", "a",     "up",   "end",    "",    std::string(300, 'x'), "\xC3\xBC"};
}

TEST(FindActionModel, GoldenRanking) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    std::string out = "# FindActionModel ranking over the 40-action fixture in the context {EditorFocus, ProjectOpen}, "
                      "L2, untrusted. Consistency golden: review rank changes by hand.\n";
    for (const std::string& q : queries()) {
        const std::string shown = q.size() > 40 ? std::format("{} x {}", q.size(), q[0]) : q;
        out += std::format("query \"{}\"{}\n", shown, isShortcutQuery(q) ? " (shortcut tier)" : "");
        out += describeHits(fixture.registry, model.query(q, fixture.index, fixtureContext(), {.limit = 12}));
    }
    test::expectGolden("find-action-ranking.txt", out);
}

TEST(FindActionModel, SpotChecks) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    const auto top = [&](std::string_view q) {
        const auto hits = model.query(q, fixture.index, fixtureContext());
        return hits.empty() ? std::string() : fixture.registry.info(hits[0].id).id;
    };
    EXPECT_EQ(top("copy"), "editor.copy");
    EXPECT_EQ(top("Copy"), "editor.copy");
    EXPECT_EQ(top("rc"), "code.reformat");
    EXPECT_EQ(top("refco"), "code.reformat");
    EXPECT_EQ(top("ref co"), "code.reformat");
    EXPECT_EQ(top("ctrl+shift+a"), "search.findAction");
    EXPECT_EQ(top("Ctrl Shift A"), "search.findAction");
    EXPECT_EQ(top("ctrl+k"), "vcs.commit");
    EXPECT_EQ(top("f2"), "navigate.nextError");
    EXPECT_EQ(top("alt+1"), "window.project");
    EXPECT_EQ(top("up"), "editor.lineUp");
    EXPECT_EQ(top(""), "");
    EXPECT_EQ(top("   "), "");
    EXPECT_EQ(top("\xC3\xBC"), "tools.mueller") << "non-ASCII matches exactly";

    const auto k = model.query("ctrl+k", fixture.index, fixtureContext());
    ASSERT_GE(k.size(), 2U);
    EXPECT_EQ(k[0].score, 950);
    EXPECT_EQ(k[0].source, MatchSource::Shortcut);
    EXPECT_EQ(fixture.registry.info(k[1].id).id, "vcs.commitAndPush");
    EXPECT_EQ(k[1].score, 850) << "a two-stroke binding starting with the query";

    for (const std::string_view bare : {"a", "up", "end", "tab", "x"}) {
        EXPECT_FALSE(isShortcutQuery(bare)) << bare;
    }
    for (const std::string_view shortcut :
         {"ctrl+k", "f2", "F12", "alt+1", "Ctrl Shift A", "shift+f6", "doubleshift", "ctrl+k, ctrl+p"}) {
        EXPECT_TRUE(isShortcutQuery(shortcut)) << shortcut;
    }
    // The hidden (searchable=false) action never appears, even by its shortcut.
    for (const std::string_view q : {"hidden", "ctrl+alt+h"}) {
        for (const FindActionHit& hit : model.query(q, fixture.index, fixtureContext())) {
            EXPECT_NE(fixture.registry.info(hit.id).id, "tools.hidden") << q;
        }
    }
}

TEST(FindActionModel, QueryShortcut) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    const auto hits =
        model.queryShortcut(*parseSequence("Ctrl+K", SyntaxMode::Strict), fixture.index, test::allContexts());
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_EQ(fixture.registry.info(hits[0].id).id, "vcs.commit");
    EXPECT_EQ(hits[0].score, 950);
    EXPECT_EQ(fixture.registry.info(hits[1].id).id, "vcs.commitAndPush");
    EXPECT_EQ(hits[1].score, 850);
    EXPECT_TRUE(model.queryShortcut(Sequence{}, fixture.index, test::allContexts()).empty());
    const auto gesture = model.queryShortcut(Sequence::of(Gesture::DoubleShift), fixture.index, test::allContexts());
    ASSERT_EQ(gesture.size(), 1U);
    EXPECT_EQ(fixture.registry.info(gesture[0].id).id, "search.everywhere");
}

TEST(FindActionModel, SynonymsRankBelowNames) {
    const Fixture fixture({{"test.named", "Beautify", {}, {}}, {"test.synonym", "Pretty Printer", {"Beautify"}, {}}});
    const FindActionModel model(fixture.registry);
    const auto hits = model.query("beautify", fixture.index, test::allContexts());
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_EQ(fixture.registry.info(hits[0].id).id, "test.named");
    EXPECT_EQ(hits[0].source, MatchSource::Name);
    EXPECT_EQ(fixture.registry.info(hits[1].id).id, "test.synonym");
    EXPECT_EQ(hits[1].source, MatchSource::Synonym);
    EXPECT_EQ(hits[1].textIndex, 1);
    EXPECT_EQ(hits[0].score - hits[1].score, 50);
}

TEST(FindActionModel, DisabledActionsGoLastWithinAScore) {
    const Fixture fixture({{"run.run", "Run", {}, {}, AvailabilityLevel::L0, true},
                           {"run.zrun", "Run", {}, {}, AvailabilityLevel::L0, false},
                           {"run.placeholder", "Run", {}, {}, AvailabilityLevel::L0, false, true}});
    const FindActionModel model(fixture.registry);
    const ActionContext untrusted{.keys = {}, .level = AvailabilityLevel::L4, .trusted = false};
    const auto hits = model.query("run", fixture.index, untrusted);
    ASSERT_EQ(hits.size(), 3U);
    EXPECT_EQ(fixture.registry.info(hits[0].id).id, "run.zrun") << "enabled first despite its id";
    EXPECT_TRUE(hits[0].enabled);
    EXPECT_FALSE(hits[1].enabled);
    EXPECT_FALSE(hits[2].enabled);
    std::set<DisabledReason> reasons = {hits[1].disabledReason, hits[2].disabledReason};
    EXPECT_EQ(reasons, (std::set<DisabledReason>{DisabledReason::Untrusted, DisabledReason::NoHandler}));
    const auto enabledOnly = model.query("run", fixture.index, untrusted, {.limit = 100, .includeDisabled = false});
    ASSERT_EQ(enabledOnly.size(), 1U);
    EXPECT_EQ(fixture.registry.info(enabledOnly[0].id).id, "run.zrun");
}

TEST(FindActionModel, InapplicableActionsAreExcluded) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    const auto ids = [&](const ActionContext& context) {
        std::vector<std::string> out;
        for (const FindActionHit& hit : model.query("clear terminal", fixture.index, context, {.limit = 0})) {
            out.push_back(fixture.registry.info(hit.id).id);
        }
        return out;
    };
    const auto without = ids(fixtureContext());
    EXPECT_EQ(std::ranges::find(without, "window.terminalClear"), without.end());
    ActionContext terminal = fixtureContext();
    terminal.keys.set(ContextKey::TerminalFocus);
    const auto with = ids(terminal);
    EXPECT_NE(std::ranges::find(with, "window.terminalClear"), with.end());
    // editor.* actions need EditorFocus.
    for (const FindActionHit& hit : model.query("copy", fixture.index, ActionContext{}, {.limit = 0})) {
        EXPECT_NE(fixture.registry.info(hit.id).id, "editor.copy");
    }
}

TEST(FindActionModel, UsageBreaksTiesAndRoundTrips) {
    const Fixture fixture({{"test.alpha", "Toggle", {}, {}}, {"test.beta", "Toggle", {}, {}}});
    FindActionModel model(fixture.registry);
    const auto first = [&](const FindActionModel& m) {
        return fixture.registry.info(m.query("toggle", fixture.index, test::allContexts())[0].id).id;
    };
    EXPECT_EQ(first(model), "test.alpha") << "the string id breaks the tie";
    model.noteInvoked(*fixture.registry.find("test.beta"));
    EXPECT_EQ(first(model), "test.beta");
    model.noteInvoked(*fixture.registry.find("test.alpha"));
    model.noteInvoked(*fixture.registry.find("test.alpha"));
    EXPECT_EQ(first(model), "test.alpha");

    const auto usage = model.exportUsage();
    EXPECT_EQ(usage, (std::vector<std::pair<std::string, std::uint32_t>>{{"test.alpha", 2}, {"test.beta", 1}}));
    FindActionModel restored(fixture.registry);
    const std::vector<std::pair<std::string, std::uint32_t>> imported = {
        {"test.beta", 9}, {"future.action", 5}, {"test.alpha", 2}};
    restored.importUsage(imported);
    EXPECT_EQ(first(restored), "test.beta");
    EXPECT_EQ(restored.exportUsage(), (std::vector<std::pair<std::string, std::uint32_t>>{
                                          {"future.action", 5}, {"test.alpha", 2}, {"test.beta", 9}}))
        << "unknown ids are kept for later";
    restored.importUsage(usage);
    EXPECT_EQ(first(restored), "test.alpha");

    // Counts saturate instead of wrapping.
    restored.importUsage(std::vector<std::pair<std::string, std::uint32_t>>{{"test.beta", UINT32_MAX}});
    restored.noteInvoked(*fixture.registry.find("test.beta"));
    EXPECT_EQ(restored.exportUsage().back().second, UINT32_MAX);
}

TEST(FindActionModel, LimitCapsTheResults) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    const auto all = model.query("e", fixture.index, fixtureContext(), {.limit = 0});
    const auto five = model.query("e", fixture.index, fixtureContext(), {.limit = 5});
    ASSERT_GT(all.size(), 5U);
    ASSERT_EQ(five.size(), 5U);
    for (std::size_t i = 0; i < five.size(); ++i) {
        EXPECT_EQ(five[i].id, all[i].id);
    }
}

std::vector<std::string> idsOf(const ActionRegistry& registry, std::span<const FindActionHit> hits) {
    std::vector<std::string> out;
    for (const FindActionHit& hit : hits) {
        out.push_back(registry.info(hit.id).id + "@" + std::to_string(hit.score));
    }
    return out;
}

TEST(FindActionModel, IndependentOfRegistrationOrder) {
    const Fixture reference;
    const FindActionModel referenceModel(reference.registry);
    std::vector<std::vector<std::string>> expected;
    for (const std::string& q : queries()) {
        expected.push_back(
            idsOf(reference.registry, referenceModel.query(q, reference.index, fixtureContext(), {.limit = 0})));
    }
    std::mt19937_64 rng(78);
    for (int permutation = 0; permutation < 100; ++permutation) {
        std::vector<FixtureAction> actions = fixtureActions();
        std::ranges::shuffle(actions, rng);
        const Fixture shuffled(actions);
        const FindActionModel model(shuffled.registry);
        const std::vector<std::string> qs = queries();
        for (std::size_t i = 0; i < qs.size(); ++i) {
            EXPECT_EQ(idsOf(shuffled.registry, model.query(qs[i], shuffled.index, fixtureContext(), {.limit = 0})),
                      expected[i])
                << "permutation " << permutation << ", query '" << qs[i] << "'";
        }
        if (HasFailure()) {
            return;
        }
    }
}

// ASCII lowercase with spaces removed: how the highlight text and the query are compared.
std::string folded(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ') {
            out += c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        }
    }
    return out;
}

bool isContinuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

TEST(FindActionModel, HighlightsAreValid) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    std::vector<std::string> qs = queries();
    for (const std::string_view extra : {"go to", "move caret end", "reformat", "o", "ti", "lin", "m\xC3\xBC", "pf",
                                         "settings", "prefs", "imp", "COPY PATH"}) {
        qs.emplace_back(extra);
    }
    std::size_t checked = 0;
    for (const std::string& q : qs) {
        for (const FindActionHit& hit :
             model.query(q, fixture.index, test::allContexts(), {.limit = 0, .includeDisabled = true})) {
            if (hit.source == MatchSource::Shortcut) {
                EXPECT_EQ(hit.highlightCount, 0);
                continue;
            }
            const std::string_view text = hitText(fixture.registry, hit);
            std::string matched;
            std::size_t previousEnd = 0;
            ASSERT_GT(hit.highlightCount, 0) << q;
            for (std::size_t r = 0; r < hit.highlightCount; ++r) {
                const TextRange range = hit.highlight[r];
                const auto offset = static_cast<std::size_t>(range.offset);
                const std::size_t end = offset + range.length;
                ASSERT_LE(end, text.size()) << q << " in " << text;
                ASSERT_GT(range.length, 0);
                EXPECT_GE(offset, previousEnd) << "ranges are in order and disjoint";
                EXPECT_FALSE(offset < text.size() && isContinuationByte(text[offset])) << q;
                EXPECT_FALSE(end < text.size() && isContinuationByte(text[end])) << q;
                matched += text.substr(offset, range.length);
                previousEnd = end;
            }
            if (!hit.highlightTruncated) {
                std::string query(q.substr(0, kMaxFindActionQueryBytes));
                EXPECT_EQ(folded(matched), folded(query)) << "query '" << q << "' on '" << text << "'";
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 100U);
}

TEST(FindActionModel, LongerNameQueriesNarrowTheResults) {
    const Fixture fixture;
    const FindActionModel model(fixture.registry);
    std::size_t pairs = 0;
    for (const std::string_view phrase :
         {"reformat code", "copy path", "go to file", "move caret to text end", "paste from history",
          "basic completion", "find action", "next highlighted error", "settings", "commit and push"}) {
        std::set<std::string> previous;
        for (std::size_t n = 1; n <= phrase.size(); ++n) {
            const std::string_view q = phrase.substr(0, n);
            ASSERT_FALSE(isShortcutQuery(q)) << q;
            std::set<std::string> current;
            for (const FindActionHit& hit : model.query(q, fixture.index, test::allContexts(), {.limit = 0})) {
                current.insert(fixture.registry.info(hit.id).id);
            }
            if (n > 1) {
                EXPECT_TRUE(std::ranges::includes(previous, current)) << "'" << q << "' widened the results";
                ++pairs;
            }
            EXPECT_FALSE(current.empty()) << q;
            previous = std::move(current);
        }
    }
    EXPECT_GT(pairs, 100U);
}

} // namespace
} // namespace frost::input
