// SPDX-License-Identifier: Apache-2.0
//
// BindingIndex and KeymapService: overrides, unresolved ids, shortcut order and the
// reachable primary shortcut, priorities, conflicts and unreachable bindings; the
// service's automatic rebuild, layout changes, preference validation, and settings fault
// injection through MemoryKeymapSettings.

#include "frost/input/keymap_service.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/synthetic_layouts.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {
namespace {

using namespace std::chrono_literals;

Sequence seq(std::string_view text) {
    auto parsed = parseSequence(text, SyntaxMode::Strict);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(Sequence{});
}

std::vector<ActionOverride> overridesOf(std::initializer_list<ActionOverride> overrides) { return overrides; }

Keymap keymapOf(std::string_view bindings) {
    const std::string text = std::string("frost-keymap 1\nid test\nname Test\n[default]\n") + std::string(bindings);
    auto keymap = parseKeymap(text);
    EXPECT_TRUE(keymap.has_value()) << keymap.error().message();
    return keymap ? std::move(*keymap) : Keymap{};
}

std::vector<std::string> names(const ActionRegistry& registry, std::span<const ActionId> ids) {
    std::vector<std::string> out;
    for (const ActionId id : ids) {
        out.push_back(registry.info(id).id);
    }
    return out;
}

std::vector<std::string> formatted(std::span<const Sequence> sequences) {
    std::vector<std::string> out;
    for (const Sequence& s : sequences) {
        out.push_back(formatSequence(s));
    }
    return out;
}

ActionId add(ActionRegistry& registry, std::string id, int priority = 0) {
    ActionSpec spec;
    spec.id = std::move(id);
    spec.name = spec.id;
    spec.priority = priority;
    spec.handler = [](const ActionContext&, const InvokeInfo&) {};
    auto added = registry.add(std::move(spec));
    EXPECT_TRUE(added.has_value());
    return added.value_or(kNoAction);
}

const LayoutTable& enUs() { return simulatedLayout(SimulatedLayoutId::EnUs); }
const LayoutTable& deDe() { return simulatedLayout(SimulatedLayoutId::DeDe); }
// No key types '/': the unreachable-binding subject since de-DE reaches Slash (ADR 0030).
const LayoutTable& noSlash() {
    static const LayoutTable table = test::synthetic::noSlash();
    return table;
}

TEST(BindingIndex, OverridesReplaceAndRemove) {
    ActionRegistry registry;
    const ActionId copy = add(registry, "editor.copy");
    const ActionId paste = add(registry, "editor.paste");
    const Keymap keymap = keymapOf("editor.copy = Ctrl+C | Ctrl+Insert\neditor.paste = Ctrl+V\n");
    const std::vector<ActionOverride> overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}, {"editor.paste", {}}};
    const BindingIndex index = BindingIndex::build(keymap, KeymapPlatform::Windows, overrides, registry);
    EXPECT_TRUE(index.single(Stroke{Modifier::Ctrl, Key::C}).empty());
    EXPECT_TRUE(index.single(Stroke{Modifier::Ctrl, Key::Insert}).empty());
    EXPECT_EQ(names(registry, index.single(Stroke{Modifier::Ctrl | Modifier::Shift, Key::C})),
              std::vector<std::string>{"editor.copy"});
    EXPECT_TRUE(index.single(Stroke{Modifier::Ctrl, Key::V}).empty());
    EXPECT_EQ(formatted(index.shortcutsFor(copy)), std::vector<std::string>{"Ctrl+Shift+C"});
    EXPECT_TRUE(index.shortcutsFor(paste).empty());
    EXPECT_EQ(index.size(), 1U);
    EXPECT_TRUE(index.shortcutsFor(ActionId{77}).empty()) << "unknown ids have no shortcuts";
}

TEST(BindingIndex, OverrideForAnActionTheKeymapDoesNotName) {
    ActionRegistry registry;
    const ActionId extra = add(registry, "tools.extra");
    const BindingIndex index = BindingIndex::build(keymapOf(""), KeymapPlatform::Linux,
                                                   overridesOf({{"tools.extra", {seq("Ctrl+Alt+X")}}}), registry);
    EXPECT_EQ(formatted(index.shortcutsFor(extra)), std::vector<std::string>{"Ctrl+Alt+X"});
}

TEST(BindingIndex, LaterOverridesWinAndManyOverridesApply) {
    ActionRegistry registry;
    const ActionId copy = add(registry, "editor.copy");
    const ActionId extra = add(registry, "tools.extra");
    const Keymap keymap = keymapOf("editor.copy = Ctrl+C\n");
    const std::vector<ActionOverride> repeated = {{"tools.extra", {seq("Ctrl+Alt+X")}},
                                                  {"editor.copy", {seq("Ctrl+Shift+C")}},
                                                  {"tools.extra", {seq("Ctrl+Alt+Y")}},
                                                  {"editor.copy", {seq("Ctrl+Insert")}}};
    const BindingIndex index = BindingIndex::build(keymap, KeymapPlatform::Windows, repeated, registry);
    EXPECT_EQ(formatted(index.shortcutsFor(copy)), std::vector<std::string>{"Ctrl+Insert"});
    EXPECT_EQ(formatted(index.shortcutsFor(extra)), std::vector<std::string>{"Ctrl+Alt+Y"})
        << "an appended override is replaced by a later one too";
    EXPECT_EQ(index.size(), 2U);

    // The validate() maximum: 4096 overrides, most for actions not registered yet.
    std::vector<ActionOverride> many;
    for (std::size_t i = 0; i < kMaxOverrides - 1; ++i) {
        many.push_back({std::format("future.action{}", i), {seq("Ctrl+F12")}});
    }
    many.push_back({"editor.copy", {seq("Ctrl+Alt+C")}});
    const BindingIndex big = BindingIndex::build(keymap, KeymapPlatform::Windows, many, registry);
    EXPECT_EQ(formatted(big.shortcutsFor(copy)), std::vector<std::string>{"Ctrl+Alt+C"});
    EXPECT_EQ(big.unresolved().size(), kMaxOverrides - 1);
}

TEST(BindingIndex, UnresolvedIdsAreListed) {
    ActionRegistry registry;
    static_cast<void>(add(registry, "editor.copy"));
    const Keymap keymap = keymapOf("future.thing = Ctrl+F12\neditor.copy = Ctrl+C\nanother.later = Alt+F12\n");
    const BindingIndex index =
        BindingIndex::build(keymap, KeymapPlatform::Windows,
                            overridesOf({{"zeta.missing", {seq("Ctrl+Q")}}, {"future.thing", {}}}), registry);
    const std::vector<std::string> unresolved(index.unresolved().begin(), index.unresolved().end());
    EXPECT_EQ(unresolved, (std::vector<std::string>{"another.later", "future.thing", "zeta.missing"}));
    EXPECT_EQ(index.size(), 1U);
}

TEST(BindingIndex, ShortcutsForKeepsKeymapOrderWithoutDuplicates) {
    ActionRegistry registry;
    const ActionId copy = add(registry, "editor.copy");
    const BindingIndex index = BindingIndex::build(
        keymapOf("editor.copy = Ctrl+Insert | Ctrl+C | Ctrl+K, Ctrl+C | DoubleCtrl\n"), KeymapPlatform::Windows,
        overridesOf({{"editor.copy", {seq("Ctrl+Insert"), seq("Ctrl+C"), seq("Ctrl+Insert")}}}), registry);
    // Overrides are validated before they reach the index; the index still drops repeats.
    EXPECT_EQ(formatted(index.shortcutsFor(copy)), (std::vector<std::string>{"Ctrl+Insert", "Ctrl+C"}));
    const BindingIndex plain =
        BindingIndex::build(keymapOf("editor.copy = Ctrl+Insert | Ctrl+C | Ctrl+K, Ctrl+C | DoubleCtrl\n"),
                            KeymapPlatform::Windows, {}, registry);
    EXPECT_EQ(formatted(plain.shortcutsFor(copy)),
              (std::vector<std::string>{"Ctrl+Insert", "Ctrl+C", "Ctrl+K, Ctrl+C", "DoubleCtrl"}));
    EXPECT_EQ(names(registry, plain.prefixed(Stroke{Modifier::Ctrl, Key::K})), std::vector<std::string>{"editor.copy"});
    EXPECT_EQ(names(registry, plain.second(Stroke{Modifier::Ctrl, Key::K}, Stroke{Modifier::Ctrl, Key::C})),
              std::vector<std::string>{"editor.copy"});
    EXPECT_EQ(names(registry, plain.gesture(Gesture::DoubleCtrl)), std::vector<std::string>{"editor.copy"});
    EXPECT_TRUE(plain.gesture(Gesture::DoubleShift).empty());
    EXPECT_TRUE(plain.gesture(Gesture::None).empty());
    EXPECT_EQ(plain.size(), 4U);
}

TEST(BindingIndex, PrimaryShortcutSkipsUnreachableSequences) {
    ActionRegistry registry;
    static_cast<void>(test::registerKeymapActions(registry, test::intellij()));
    const BindingIndex index = BindingIndex::build(test::intellij(), KeymapPlatform::Windows, {}, registry);
    const ActionId comment = *registry.find("editor.commentLine");
    EXPECT_EQ(index.primaryShortcut(comment, enUs()), seq("Ctrl+Slash"));
    EXPECT_EQ(index.primaryShortcut(comment, deDe()), seq("Ctrl+Slash")) << "the '-' key (rule 3.6)";
    EXPECT_EQ(index.primaryShortcut(comment, noSlash()), seq("Ctrl+NumpadDivide"));
    const ActionId everywhere = *registry.find("search.everywhere");
    EXPECT_EQ(index.primaryShortcut(everywhere, deDe()), Sequence::of(Gesture::DoubleShift));
    const ActionId terminal = *registry.find("agent.terminal");
    EXPECT_FALSE(index.primaryShortcut(terminal, enUs()).has_value()) << "explicitly unbound";
}

TEST(BindingIndex, PriorityOrdersCandidates) {
    ActionRegistry registry;
    const ActionId low = add(registry, "test.low", 0);
    const ActionId high = add(registry, "test.high", 5);
    const ActionId later = add(registry, "test.later", 0);
    const BindingIndex index =
        BindingIndex::build(keymapOf("test.low = Ctrl+J\ntest.high = Ctrl+J\ntest.later = Ctrl+J\n"),
                            KeymapPlatform::Windows, {}, registry);
    const auto candidates = index.single(Stroke{Modifier::Ctrl, Key::J});
    ASSERT_EQ(candidates.size(), 3U);
    EXPECT_EQ(candidates[0], high) << "priority first";
    EXPECT_EQ(candidates[1], low) << "then keymap order";
    EXPECT_EQ(candidates[2], later);
}

TEST(BindingIndex, ConflictsAndUnreachable) {
    ActionRegistry registry;
    const ActionId a = add(registry, "test.a");
    const ActionId b = add(registry, "test.b");
    const ActionId c = add(registry, "test.c");
    const ActionId d = add(registry, "test.d");
    const BindingIndex index = BindingIndex::build(keymapOf("test.a = Ctrl+J | Ctrl+K | DoubleShift\n"
                                                            "test.b = Ctrl+J | Ctrl+K, Ctrl+C | DoubleShift\n"
                                                            "test.c = Ctrl+K, Ctrl+C | Ctrl+Slash\n"
                                                            "test.d = Ctrl+Alt+Z\n"),
                                                   KeymapPlatform::Windows, {}, registry);
    const std::vector<Conflict> conflicts = index.conflicts();
    const std::vector<Conflict> expected = {
        {Sequence::of(Gesture::DoubleShift), {a, b}, false},
        {seq("Ctrl+J"), {a, b}, false},
        {seq("Ctrl+K"), {a, b, c}, true},
        {seq("Ctrl+K, Ctrl+C"), {b, c}, false},
    };
    ASSERT_EQ(conflicts.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(conflicts[i].sequence, expected[i].sequence) << i;
        EXPECT_EQ(conflicts[i].actions, expected[i].actions) << i;
        EXPECT_EQ(conflicts[i].prefixConflict, expected[i].prefixConflict) << i;
    }
    EXPECT_TRUE(index.unreachable(enUs()).empty());
    EXPECT_TRUE(index.unreachable(deDe()).empty());
    EXPECT_EQ(formatted(index.unreachable(noSlash())), std::vector<std::string>{"Ctrl+Slash"});
    EXPECT_EQ(index.primaryShortcut(c, noSlash()), seq("Ctrl+K, Ctrl+C"));
    EXPECT_EQ(index.primaryShortcut(c, deDe()), seq("Ctrl+K, Ctrl+C"));
    EXPECT_EQ(index.primaryShortcut(d, noSlash()), seq("Ctrl+Alt+Z"));
}

// A double tap of Shift with `gap` between the four events, as raw events.
std::vector<KeyEvent> shiftTaps(std::uint32_t t0, std::uint32_t gap) {
    KeyEvent press = test::keyPress(Code::ShiftLeft, Modifier::Shift);
    KeyEvent release = test::keyRelease(Code::ShiftLeft);
    std::vector<KeyEvent> events;
    for (std::uint32_t i = 0; i < 4; ++i) {
        KeyEvent e = i % 2 == 0 ? press : release;
        e.osTimeMs = t0 + i * gap;
        events.push_back(e);
    }
    return events;
}

bool firesDoubleShift(KeymapService& service, std::uint32_t t0, std::uint32_t gap) {
    bool fired = false;
    for (const KeyEvent& e : shiftTaps(t0, gap)) {
        fired = service.handle(e, test::allContexts()).outcome == MatchOutcome::Fire || fired;
    }
    return fired;
}

struct ServiceRig {
    ServiceRig() { static_cast<void>(test::registerKeymapActions(registry, test::intellij())); }
    ActionRegistry registry;
    MemoryKeymapSettings settings;

    KeymapService create(const LayoutTable& layout = simulatedLayout(SimulatedLayoutId::EnUs),
                         IClock::Duration interval = 400ms) {
        auto service = KeymapService::create(registry, settings, KeymapPlatform::Windows, layout, interval);
        EXPECT_TRUE(service.has_value());
        return std::move(*service);
    }
    std::string fire(KeymapService& service, KeyEvent event, const ActionContext& context = test::allContexts()) {
        const MatchResult r = service.handle(event, context);
        return r.outcome == MatchOutcome::Fire ? registry.info(r.action).id : std::string();
    }
};

TEST(KeymapService, CreateWithDefaults) {
    ServiceRig rig;
    KeymapService service = rig.create();
    EXPECT_FALSE(service.lastSettingsError().has_value());
    EXPECT_EQ(rig.settings.loadCount(), 1U);
    EXPECT_EQ(rig.settings.storeCount(), 0U) << "create() never stores";
    EXPECT_EQ(service.keymap().id(), "intellij");
    EXPECT_EQ(service.preferences(), KeymapPreferences{});
    EXPECT_EQ(service.platform(), KeymapPlatform::Windows);
    EXPECT_EQ(service.layout().id(), "en-US:00000409");
    EXPECT_GT(service.bindings().size(), 200U);
    EXPECT_TRUE(service.bindings().unresolved().empty());
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyS, Modifier::Ctrl)), "file.saveAll");
    const ActionId copy = *rig.registry.find("editor.copy");
    EXPECT_EQ(formatted(service.shortcutsFor(copy)), (std::vector<std::string>{"Ctrl+C", "Ctrl+Insert"}));
    EXPECT_EQ(service.primaryShortcut(copy), seq("Ctrl+C"));
    EXPECT_TRUE(service.unreachableBindings().empty());
}

TEST(KeymapService, GateUsesTheContext) {
    ServiceRig rig;
    KeymapService service = rig.create();
    const KeyEvent ctrlC = test::keyPress(Code::KeyC, Modifier::Ctrl);
    EXPECT_EQ(rig.fire(service, ctrlC, ActionContext{.keys = {}, .level = AvailabilityLevel::L4, .trusted = true}), "")
        << "editor.copy needs EditorFocus";
    EXPECT_EQ(rig.fire(service, ctrlC, test::allContexts()), "editor.copy");
    EXPECT_EQ(service.peek(ctrlC, ActionContext{}), MatchOutcome::PassThrough);
    EXPECT_EQ(service.peek(ctrlC, test::allContexts()), MatchOutcome::Fire);
}

TEST(KeymapService, RebuildsAfterRegistryGrowth) {
    ActionRegistry registry;
    MemoryKeymapSettings settings;
    auto created = KeymapService::create(registry, settings, KeymapPlatform::Windows, enUs());
    ASSERT_TRUE(created.has_value());
    KeymapService& service = *created;
    const KeyEvent ctrlS = test::keyPress(Code::KeyS, Modifier::Ctrl);
    EXPECT_EQ(service.handle(ctrlS, test::allContexts()).outcome, MatchOutcome::PassThrough);
    EXPECT_GT(service.bindings().unresolved().size(), 200U);

    const std::uint64_t before = service.bindingsGeneration();
    const ActionId saveAll = add(registry, "file.saveAll");
    EXPECT_EQ(service.peek(ctrlS, test::allContexts()), MatchOutcome::Fire) << "peek sees the new action";
    EXPECT_EQ(service.bindingsGeneration(), before + 1);
    const MatchResult r = service.handle(ctrlS, test::allContexts());
    EXPECT_EQ(r.outcome, MatchOutcome::Fire);
    EXPECT_EQ(r.action, saveAll);
    EXPECT_EQ(service.bindingsGeneration(), before + 1) << "rebuilt once";

    // handle() first after growth works the same way.
    const ActionId copy = add(registry, "editor.copy");
    const MatchResult copied = service.handle(test::keyPress(Code::KeyC, Modifier::Ctrl), test::allContexts());
    EXPECT_EQ(copied.action, copy);
    EXPECT_EQ(service.bindingsGeneration(), before + 2);
    EXPECT_EQ(formatted(service.shortcutsFor(copy)), (std::vector<std::string>{"Ctrl+C", "Ctrl+Insert"}));

    // The binding queries rebuild too, before any key arrives: a menu built right after
    // registration shows the shortcut, and the generation tells consumers to re-query.
    const ActionId paste = add(registry, "editor.paste");
    EXPECT_EQ(formatted(service.shortcutsFor(paste)), (std::vector<std::string>{"Ctrl+V", "Shift+Insert"}));
    EXPECT_EQ(service.bindingsGeneration(), before + 3);
    const ActionId undo = add(registry, "editor.undo");
    EXPECT_EQ(service.primaryShortcut(undo), seq("Ctrl+Z"));
    EXPECT_EQ(service.bindingsGeneration(), before + 4);
    const ActionId find = add(registry, "editor.find");
    EXPECT_FALSE(service.bindings().shortcutsFor(find).empty());
    EXPECT_EQ(service.bindingsGeneration(), before + 5);
    static_cast<void>(add(registry, "editor.commentLine"));
    EXPECT_TRUE(service.unreachableBindings().empty());
    EXPECT_EQ(service.bindingsGeneration(), before + 6);
    EXPECT_EQ(formatted(service.shortcutsFor(paste)), (std::vector<std::string>{"Ctrl+V", "Shift+Insert"}));
    EXPECT_EQ(service.bindingsGeneration(), before + 6) << "no growth, no rebuild";
}

// The trust gate (PLAN.md section 3.5) on the keyboard path: an executesProjectCode action
// never fires, starts a sequence or completes a gesture from an untrusted context, and the
// next enabled candidate of the same stroke wins instead.
TEST(KeymapService, TrustGatesTheKeyboardPath) {
    ActionRegistry registry;
    const auto addProjectCode = [&](std::string id, int priority = 0) {
        ActionSpec spec;
        spec.id = std::move(id);
        spec.name = spec.id;
        spec.priority = priority;
        spec.executesProjectCode = true;
        spec.handler = [](const ActionContext&, const InvokeInfo&) {};
        auto added = registry.add(std::move(spec));
        EXPECT_TRUE(added.has_value());
        return added.value_or(kNoAction);
    };
    const ActionId run = addProjectCode("run.run", 1);
    const ActionId fallback = add(registry, "test.safeShiftF10");
    const ActionId chord = addProjectCode("test.projectChord");
    const ActionId anything = addProjectCode("tools.runAnything");
    MemoryKeymapSettings settings(KeymapPreferences{
        .overrides = {{"test.safeShiftF10", {seq("Shift+F10")}}, {"test.projectChord", {seq("Ctrl+K, Ctrl+R")}}}});
    auto created = KeymapService::create(registry, settings, KeymapPlatform::Windows, enUs());
    ASSERT_TRUE(created.has_value());
    KeymapService& service = *created;
    ActionContext untrusted = test::allContexts();
    untrusted.trusted = false;
    const ActionContext trusted = test::allContexts();

    const KeyEvent shiftF10 = test::keyPress(Code::F10, Modifier::Shift);
    EXPECT_EQ(service.peek(shiftF10, untrusted), MatchOutcome::Fire);
    MatchResult r = service.handle(shiftF10, untrusted);
    EXPECT_EQ(r.outcome, MatchOutcome::Fire);
    EXPECT_EQ(r.action, fallback) << "the project-code action is skipped, the next candidate fires";
    r = service.handle(shiftF10, trusted);
    EXPECT_EQ(r.action, run) << "trusted: the higher-priority project-code action fires";

    const KeyEvent ctrlK = test::keyPress(Code::KeyK, Modifier::Ctrl);
    EXPECT_EQ(service.handle(ctrlK, untrusted).outcome, MatchOutcome::PassThrough) << "no prefix from project code";
    EXPECT_FALSE(service.pendingPrefix().has_value());
    EXPECT_EQ(service.handle(ctrlK, trusted).outcome, MatchOutcome::Pending);
    // Trust revoked between the strokes: the second stroke is consumed, nothing fires.
    r = service.handle(test::keyPress(Code::KeyR, Modifier::Ctrl), untrusted);
    EXPECT_EQ(r.outcome, MatchOutcome::Consumed);
    EXPECT_TRUE(r.cancelledPending);
    EXPECT_EQ(service.handle(ctrlK, trusted).outcome, MatchOutcome::Pending);
    r = service.handle(test::keyPress(Code::KeyR, Modifier::Ctrl), trusted);
    EXPECT_EQ(r.action, chord);

    const auto doubleCtrl = [&](const ActionContext& context) {
        MatchResult last;
        for (int i = 0; i < 2; ++i) {
            last = service.handle(test::keyPress(Code::ControlLeft, Modifier::Ctrl), context);
            last = service.handle(test::keyRelease(Code::ControlLeft), context);
        }
        return last;
    };
    EXPECT_EQ(doubleCtrl(untrusted).outcome, MatchOutcome::PassThrough) << "no gesture from project code";
    r = doubleCtrl(trusted);
    EXPECT_EQ(r.outcome, MatchOutcome::Fire);
    EXPECT_EQ(r.action, anything);
}

TEST(KeymapService, SetLayoutResetsAPendingStroke) {
    ServiceRig rig;
    static_cast<void>(add(rig.registry, "test.chord"));
    rig.settings.set(KeymapPreferences{.overrides = {{"test.chord", {seq("Ctrl+K, Ctrl+C")}}}});
    KeymapService service = rig.create();
    EXPECT_EQ(service.handle(test::keyPress(Code::KeyK, Modifier::Ctrl), test::allContexts()).outcome,
              MatchOutcome::Pending);
    EXPECT_TRUE(service.pendingPrefix().has_value());
    const std::uint64_t before = service.bindingsGeneration();
    service.setLayout(deDe());
    EXPECT_FALSE(service.pendingPrefix().has_value());
    EXPECT_EQ(service.bindingsGeneration(), before + 1);
    EXPECT_EQ(service.layout().id(), "de-DE:00000407");
    EXPECT_TRUE(service.unreachableBindings().empty()) << "de-DE reaches every default (ADR 0030)";
    EXPECT_EQ(service.primaryShortcut(*rig.registry.find("editor.commentLine")), seq("Ctrl+Slash"));
    service.setLayout(noSlash());
    EXPECT_EQ(service.bindingsGeneration(), before + 2);
    // Sorted by Sequence order: modifier bits first (Ctrl before Ctrl+Shift), then the key.
    EXPECT_EQ(formatted(service.unreachableBindings()), (std::vector<std::string>{"Ctrl+Slash", "Ctrl+Shift+Slash"}));
    EXPECT_EQ(service.primaryShortcut(*rig.registry.find("editor.commentLine")), seq("Ctrl+NumpadDivide"));
    // reset() drops a pending stroke too.
    static_cast<void>(service.handle(test::keyPress(Code::KeyK, Modifier::Ctrl), test::allContexts()));
    service.reset();
    EXPECT_FALSE(service.pendingPrefix().has_value());
}

TEST(KeymapService, LoadFailureUsesDefaults) {
    for (const ErrorCode code : {ErrorCode::IoError, ErrorCode::Corrupt}) {
        ServiceRig rig;
        rig.settings.set(KeymapPreferences{.overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}}});
        rig.settings.injectFault(SettingsOp::Load, code);
        KeymapService service = rig.create();
        ASSERT_TRUE(service.lastSettingsError().has_value());
        EXPECT_EQ(service.lastSettingsError()->code(), code);
        EXPECT_TRUE(service.preferences().overrides.empty());
        EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl)), "editor.copy");
        EXPECT_EQ(rig.settings.storeCount(), 0U);
    }
}

TEST(KeymapService, InvalidOverridesAreDroppedValidOnesApply) {
    ServiceRig rig;
    rig.settings.set(KeymapPreferences{.overrides = {
                                           {"editor.copy", {seq("Ctrl+Shift+C")}},
                                           {"Bad_Id", {seq("Ctrl+B")}},
                                           {"editor.cut", {Sequence::of(Stroke{{}, Key::A})}},
                                           {"editor.paste", {seq("Ctrl+Shift+V")}},
                                           {"editor.copy", {seq("Ctrl+Alt+C")}},
                                       }});
    KeymapService service = rig.create();
    ASSERT_TRUE(service.lastSettingsError().has_value());
    EXPECT_EQ(service.lastSettingsError()->code(), ErrorCode::InvalidArgument);
    EXPECT_NE(service.lastSettingsError()->message().find("Bad_Id"), std::string::npos)
        << service.lastSettingsError()->message();
    ASSERT_EQ(service.preferences().overrides.size(), 2U);
    EXPECT_EQ(service.preferences().overrides[0].actionId, "editor.copy");
    EXPECT_EQ(service.preferences().overrides[1].actionId, "editor.paste");
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl | Modifier::Shift)), "editor.copy");
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyV, Modifier::Ctrl | Modifier::Shift)), "editor.paste");
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyX, Modifier::Ctrl)), "editor.cut") << "cut keeps its default";
    EXPECT_EQ(rig.settings.storeCount(), 0U) << "sanitising never writes back";
}

TEST(KeymapService, StoreFailureKeepsTheOldBindings) {
    ServiceRig rig;
    KeymapService service = rig.create();
    const std::uint64_t generation = service.bindingsGeneration();
    rig.settings.injectFault(SettingsOp::Store, ErrorCode::IoError);
    auto result = service.setPreferences(KeymapPreferences{.overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    EXPECT_EQ(rig.settings.storeCount(), 1U);
    EXPECT_EQ(service.bindingsGeneration(), generation);
    EXPECT_TRUE(service.preferences().overrides.empty());
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl)), "editor.copy");
    EXPECT_TRUE(rig.settings.current().overrides.empty());

    // The same change succeeds once the fault is gone, and is stored.
    ASSERT_TRUE(service.setPreferences(KeymapPreferences{.overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}}}));
    EXPECT_EQ(rig.settings.current().overrides.size(), 1U);
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl)), "");
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl | Modifier::Shift)), "editor.copy");
    EXPECT_GT(service.bindingsGeneration(), generation);
    EXPECT_FALSE(service.lastSettingsError().has_value());
}

TEST(KeymapService, InvalidPreferencesAreRejectedBeforeStoring) {
    ServiceRig rig;
    KeymapService service = rig.create();
    auto result = service.setPreferences(KeymapPreferences{.overrides = {{"Bad_Id", {}}}});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(rig.settings.storeCount(), 0U);
    EXPECT_FALSE(service.setPreferences(KeymapPreferences{.keymapId = "vs", .overrides = {}}).has_value());
    EXPECT_EQ(rig.settings.storeCount(), 0U);
}

TEST(KeymapService, ReloadErrorKeepsTheCurrentBindings) {
    ServiceRig rig;
    KeymapService service = rig.create();
    rig.settings.set(KeymapPreferences{.overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}}});
    rig.settings.injectFault(SettingsOp::Load, ErrorCode::Corrupt);
    const std::uint64_t generation = service.bindingsGeneration();
    service.reloadPreferences();
    ASSERT_TRUE(service.lastSettingsError().has_value());
    EXPECT_EQ(service.lastSettingsError()->code(), ErrorCode::Corrupt);
    EXPECT_EQ(service.bindingsGeneration(), generation);
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl)), "editor.copy");

    service.reloadPreferences();
    EXPECT_FALSE(service.lastSettingsError().has_value());
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl | Modifier::Shift)), "editor.copy");
    EXPECT_EQ(rig.settings.storeCount(), 0U) << "reloads never store";
}

TEST(KeymapService, UnknownKeymapIdFallsBackAndKeepsOverrides) {
    ServiceRig rig;
    rig.settings.set(KeymapPreferences{.keymapId = "vs", .overrides = {{"editor.copy", {seq("Ctrl+Shift+C")}}}});
    KeymapService service = rig.create();
    EXPECT_EQ(service.keymap().id(), "intellij");
    EXPECT_EQ(service.preferences().keymapId, "intellij");
    ASSERT_TRUE(service.lastSettingsError().has_value());
    EXPECT_NE(service.lastSettingsError()->message().find("'vs'"), std::string::npos);
    EXPECT_EQ(rig.fire(service, test::keyPress(Code::KeyC, Modifier::Ctrl | Modifier::Shift)), "editor.copy");
}

TEST(KeymapService, DoubleTapIntervalFromPreferencesOrPlatform) {
    ServiceRig rig;
    KeymapService service = rig.create(enUs(), 500ms);
    EXPECT_TRUE(firesDoubleShift(service, 1000, 450)) << "the platform interval (500 ms)";
    ASSERT_TRUE(service.setPreferences(KeymapPreferences{.doubleTapInterval = 200ms, .overrides = {}}));
    EXPECT_FALSE(firesDoubleShift(service, 10000, 250)) << "the preference (200 ms)";
    EXPECT_TRUE(firesDoubleShift(service, 20000, 200));
    ASSERT_TRUE(service.setPreferences(KeymapPreferences{}));
    EXPECT_TRUE(firesDoubleShift(service, 30000, 450)) << "0 means the platform interval again";
}

TEST(KeymapService, ServiceIsMovable) {
    ServiceRig rig;
    KeymapService first = rig.create();
    KeymapService second = std::move(first);
    EXPECT_EQ(rig.fire(second, test::keyPress(Code::KeyS, Modifier::Ctrl)), "file.saveAll");
}

TEST(KeymapPreferencesValidation, Bounds) {
    EXPECT_TRUE(validate(KeymapPreferences{}).has_value());
    EXPECT_TRUE(validate(KeymapPreferences{.doubleTapInterval = 2s, .overrides = {}}).has_value());
    EXPECT_FALSE(validate(KeymapPreferences{.doubleTapInterval = 2s + 1ns, .overrides = {}}).has_value());
    EXPECT_FALSE(validate(KeymapPreferences{.doubleTapInterval = -1ns, .overrides = {}}).has_value());
    EXPECT_FALSE(validate(KeymapPreferences{.keymapId = "vs", .overrides = {}}).has_value());
    EXPECT_FALSE(validate(KeymapPreferences{.keymapId = "", .overrides = {}}).has_value());

    KeymapPreferences many;
    for (std::size_t i = 0; i < kMaxOverrides; ++i) {
        many.overrides.push_back({std::format("test.action{}", i), {}});
    }
    EXPECT_TRUE(validate(many).has_value());
    many.overrides.push_back({"test.oneTooMany", {}});
    EXPECT_FALSE(validate(many).has_value());

    ActionOverride eight{"editor.copy", {}};
    for (int i = 1; i <= 8; ++i) {
        eight.sequences.push_back(
            Sequence::of(Stroke{Modifier::Ctrl, static_cast<Key>(index(Key::F1) + static_cast<std::size_t>(i))}));
    }
    EXPECT_TRUE(validate(KeymapPreferences{.overrides = {eight}}).has_value());
    ActionOverride nine = eight;
    nine.sequences.push_back(seq("Ctrl+F12"));
    EXPECT_FALSE(validate(KeymapPreferences{.overrides = {nine}}).has_value());

    const ActionOverride repeated{"editor.copy", {seq("Ctrl+C"), seq("Ctrl+C")}};
    EXPECT_FALSE(validate(KeymapPreferences{.overrides = {repeated}}).has_value());
    const ActionOverride twice{"editor.copy", {seq("Ctrl+C")}};
    auto duplicate = validate(KeymapPreferences{.overrides = {twice, twice}});
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_NE(duplicate.error().message().find("two overrides"), std::string::npos);
    EXPECT_FALSE(validate(KeymapPreferences{.overrides = {{"editor.copy", {Sequence{}}}}}).has_value());
    EXPECT_FALSE(
        validate(KeymapPreferences{.overrides = {{"editor.copy", {Sequence::of(Stroke{{}, Key::A})}}}}).has_value());
    EXPECT_TRUE(validate(KeymapPreferences{.overrides = {{"editor.copy", {seq("Ctrl+K, C")}}}}).has_value());
}

TEST(KeymapPreferencesValidation, SanitizeResetsInvalidFields) {
    std::optional<Error> problem;
    const KeymapPreferences clean = sanitize(KeymapPreferences{.doubleTapInterval = 300ms, .overrides = {}}, problem);
    EXPECT_FALSE(problem.has_value());
    EXPECT_EQ(clean.doubleTapInterval, 300ms);

    const KeymapPreferences fixed = sanitize(KeymapPreferences{.doubleTapInterval = 5s, .overrides = {}}, problem);
    ASSERT_TRUE(problem.has_value());
    EXPECT_EQ(fixed.doubleTapInterval, IClock::Duration::zero());
    // Only the first problem is recorded.
    std::optional<Error> first;
    static_cast<void>(sanitize(KeymapPreferences{.keymapId = "x", .doubleTapInterval = 5s, .overrides = {}}, first));
    ASSERT_TRUE(first.has_value());
    EXPECT_NE(first->message().find("'x'"), std::string::npos);
}

TEST(MemoryKeymapSettings, FaultsAreConsumedInOrder) {
    MemoryKeymapSettings settings(KeymapPreferences{.doubleTapInterval = 100ms, .overrides = {}});
    settings.injectFault(SettingsOp::Load, ErrorCode::Busy, 2);
    EXPECT_EQ(settings.load().error().code(), ErrorCode::Busy);
    EXPECT_TRUE(settings.store(KeymapPreferences{}).has_value()) << "faults are per operation";
    EXPECT_EQ(settings.load().error().code(), ErrorCode::Busy);
    auto loaded = settings.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, KeymapPreferences{});
    EXPECT_EQ(settings.loadCount(), 3U);
    EXPECT_EQ(settings.storeCount(), 1U);
}

} // namespace
} // namespace frost::input
