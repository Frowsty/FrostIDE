// SPDX-License-Identifier: Apache-2.0
//
// ActionRegistry: registration and validation, lookup, context rules, the gating order of
// whyDisabled (including the trust gate for actions that execute project code, PLAN.md
// sections 3.3 and 3.5), tryInvoke, and storage stability when a handler registers more
// actions.

#include "frost/input/action.hpp"

#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {
namespace {

ActionSpec spec(std::string id, std::string name = "Name") {
    ActionSpec s;
    s.id = std::move(id);
    s.name = std::move(name);
    s.handler = [](const ActionContext&, const InvokeInfo&) {};
    return s;
}

ActionContext context(ContextKeys keys = {}, AvailabilityLevel level = AvailabilityLevel::L4, bool trusted = true) {
    return ActionContext{.keys = keys, .level = level, .trusted = trusted};
}

TEST(ActionRegistry, AddAndFind) {
    ActionRegistry registry;
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_EQ(registry.generation(), 0U);
    ActionSpec s = spec("editor.copy", "Copy");
    s.description = "Copy the selection";
    s.group = "Edit";
    s.synonyms = {"duplicate to clipboard"};
    s.priority = 3;
    s.requiredLevel = AvailabilityLevel::L1;
    auto id = registry.add(std::move(s));
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(registry.find("editor.copy"), *id);
    EXPECT_FALSE(registry.find("editor.paste").has_value());
    EXPECT_FALSE(registry.find("").has_value());
    const ActionInfo& info = registry.info(*id);
    EXPECT_EQ(info.id, "editor.copy");
    EXPECT_EQ(info.name, "Copy");
    EXPECT_EQ(info.description, "Copy the selection");
    EXPECT_EQ(info.group, "Edit");
    EXPECT_EQ(info.synonyms, std::vector<std::string>{"duplicate to clipboard"});
    EXPECT_EQ(info.priority, 3);
    EXPECT_EQ(info.requiredLevel, AvailabilityLevel::L1);
    EXPECT_TRUE(info.hasHandler);
    EXPECT_TRUE(info.searchable);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_EQ(registry.generation(), 1U);
}

TEST(ActionRegistry, DuplicateIdIsRejected) {
    ActionRegistry registry;
    ASSERT_TRUE(registry.add(spec("editor.copy")).has_value());
    auto again = registry.add(spec("editor.copy"));
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_EQ(registry.generation(), 1U) << "failed adds do not count";
}

TEST(ActionRegistry, ActionIdPattern) {
    struct Case {
        std::string_view id;
        bool valid;
    };
    const Case cases[] = {
        {"editor.copy", true},
        {"a.b", true},
        {"editor.copyAsPlainText", true},
        {"a1.b2.c3", true},
        {"refactor.extractMethod2", true},
        {std::string_view("abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghi"), true},   // 64 bytes
        {std::string_view("abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij"), false}, // 65
        {"", false},
        {"copy", false},
        {"Editor.copy", false},
        {"editor.Copy", false},
        {"editor..copy", false},
        {".editor.copy", false},
        {"editor.copy.", false},
        {"editor.1copy", false},
        {"editor.co-py", false},
        {"editor.co_py", false},
        {"editor. copy", false},
        {"editor.copy ", false},
        {"\xC3\xA9"
         "diteur.copy",
         false},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(isValidActionId(c.id), c.valid) << "'" << c.id << "'";
        ActionRegistry registry;
        auto added = registry.add(spec(std::string(c.id)));
        EXPECT_EQ(added.has_value(), c.valid) << "'" << c.id << "'";
        if (!added) {
            EXPECT_EQ(added.error().code(), ErrorCode::InvalidArgument);
        }
    }
}

TEST(ActionRegistry, NameAndSynonymLimits) {
    ActionRegistry registry;
    EXPECT_FALSE(registry.add(spec("a.empty", "")).has_value());
    EXPECT_TRUE(registry.add(spec("a.max", std::string(256, 'n'))).has_value());
    EXPECT_FALSE(registry.add(spec("a.long", std::string(257, 'n'))).has_value());

    ActionSpec many = spec("a.many");
    many.synonyms.assign(32, "s");
    EXPECT_TRUE(registry.add(std::move(many)).has_value());
    ActionSpec tooMany = spec("a.tooMany");
    tooMany.synonyms.assign(33, "s");
    EXPECT_FALSE(registry.add(std::move(tooMany)).has_value());
    ActionSpec emptySynonym = spec("a.emptySynonym");
    emptySynonym.synonyms = {"ok", ""};
    EXPECT_FALSE(registry.add(std::move(emptySynonym)).has_value());
    ActionSpec longSynonym = spec("a.longSynonym");
    longSynonym.synonyms = {std::string(257, 's')};
    EXPECT_FALSE(registry.add(std::move(longSynonym)).has_value());
    EXPECT_EQ(registry.size(), 2U);
}

TEST(ActionRegistry, ContextRuleAllAndNone) {
    const ContextRule rule{{ContextKey::EditorFocus, ContextKey::ProjectOpen}, {ContextKey::EditorReadOnly}};
    EXPECT_TRUE(rule.matches({ContextKey::EditorFocus, ContextKey::ProjectOpen}));
    EXPECT_TRUE(rule.matches({ContextKey::EditorFocus, ContextKey::ProjectOpen, ContextKey::Debugging}));
    EXPECT_FALSE(rule.matches({ContextKey::EditorFocus}));
    EXPECT_FALSE(rule.matches({ContextKey::EditorFocus, ContextKey::ProjectOpen, ContextKey::EditorReadOnly}));
    EXPECT_TRUE(ContextRule{}.matches({}));
    EXPECT_TRUE(ContextRule{}.matches(ContextKeys::all()));

    ContextKeys keys;
    EXPECT_TRUE(keys.empty());
    keys.set(ContextKey::Debugging);
    EXPECT_TRUE(keys.has(ContextKey::Debugging));
    keys.set(ContextKey::Debugging, false);
    EXPECT_FALSE(keys.has(ContextKey::Debugging));
    EXPECT_EQ(keys.bits(), 0U);

    ActionRegistry registry;
    ActionSpec s = spec("editor.paste");
    s.context = rule;
    const ActionId id = *registry.add(std::move(s));
    EXPECT_TRUE(registry.isApplicable(id, context({ContextKey::EditorFocus, ContextKey::ProjectOpen})));
    EXPECT_FALSE(registry.isApplicable(id, context({ContextKey::EditorFocus})));
    EXPECT_FALSE(registry.isApplicable(ActionId{99}, context())) << "unknown ids are never applicable";
}

TEST(ActionRegistry, WhyDisabledOrder) {
    ActionRegistry registry;
    bool callbackSays = false;
    int callbackCalls = 0;
    ActionSpec s = spec("run.run");
    s.context = ContextRule{{ContextKey::ProjectOpen}, {}};
    s.requiredLevel = AvailabilityLevel::L2;
    s.executesProjectCode = true;
    s.enabled = [&](const ActionContext&) {
        ++callbackCalls;
        return callbackSays;
    };
    const ActionId id = *registry.add(std::move(s));
    const ContextKeys open{ContextKey::ProjectOpen};

    // Each reason masks the later ones.
    EXPECT_EQ(registry.whyDisabled(id, context({}, AvailabilityLevel::L0, false)), DisabledReason::NotApplicable);
    EXPECT_EQ(registry.whyDisabled(id, context(open, AvailabilityLevel::L1, false)), DisabledReason::Level);
    EXPECT_EQ(registry.whyDisabled(id, context(open, AvailabilityLevel::L2, false)), DisabledReason::Untrusted);
    EXPECT_EQ(callbackCalls, 0) << "the callback runs last";
    EXPECT_EQ(registry.whyDisabled(id, context(open, AvailabilityLevel::L2, true)), DisabledReason::Callback);
    EXPECT_EQ(callbackCalls, 1);
    callbackSays = true;
    EXPECT_EQ(registry.whyDisabled(id, context(open, AvailabilityLevel::L4, true)), DisabledReason::None);
    EXPECT_TRUE(registry.isEnabled(id, context(open, AvailabilityLevel::L4, true)));

    // A placeholder without a handler.
    ActionSpec placeholder;
    placeholder.id = "debug.stepInto";
    placeholder.name = "Step Into";
    const ActionId p = *registry.add(std::move(placeholder));
    EXPECT_FALSE(registry.info(p).hasHandler);
    EXPECT_EQ(registry.whyDisabled(p, context()), DisabledReason::NoHandler);

    EXPECT_EQ(toString(DisabledReason::Untrusted), "Untrusted");
    EXPECT_EQ(toString(DisabledReason::None), "None");
}

TEST(ActionRegistry, TrustGatesOnlyProjectCode) {
    ActionRegistry registry;
    ActionSpec runs = spec("run.run");
    runs.executesProjectCode = true;
    const ActionId run = *registry.add(std::move(runs));
    const ActionId copy = *registry.add(spec("editor.copy"));
    const ActionContext untrusted = context({}, AvailabilityLevel::L4, false);
    EXPECT_EQ(registry.whyDisabled(run, untrusted), DisabledReason::Untrusted);
    EXPECT_EQ(registry.whyDisabled(copy, untrusted), DisabledReason::None);
    // Every invocation source is gated the same way.
    for (const InvokeSource source :
         {InvokeSource::Keyboard, InvokeSource::Menu, InvokeSource::FindAction, InvokeSource::Automation}) {
        EXPECT_EQ(registry.tryInvoke(run, untrusted, InvokeInfo{source, false}), InvokeResult::Disabled);
    }
    EXPECT_EQ(registry.tryInvoke(run, context()), InvokeResult::Ran);
}

TEST(ActionRegistry, TryInvoke) {
    ActionRegistry registry;
    std::vector<InvokeInfo> calls;
    ActionSpec s = spec("editor.copy");
    s.context = ContextRule{{ContextKey::EditorFocus}, {}};
    s.handler = [&](const ActionContext& ctx, const InvokeInfo& info) {
        EXPECT_TRUE(ctx.keys.has(ContextKey::EditorFocus));
        calls.push_back(info);
    };
    const ActionId id = *registry.add(std::move(s));
    const InvokeInfo menu{InvokeSource::Menu, false};
    const InvokeInfo repeat{InvokeSource::Keyboard, true};
    EXPECT_EQ(registry.tryInvoke(id, context({ContextKey::EditorFocus}), menu), InvokeResult::Ran);
    EXPECT_EQ(registry.tryInvoke(id, context({ContextKey::EditorFocus}), repeat), InvokeResult::Ran);
    EXPECT_EQ(registry.tryInvoke(id, context({})), InvokeResult::Disabled);
    EXPECT_EQ(calls, (std::vector<InvokeInfo>{menu, repeat}));

    ActionSpec placeholder;
    placeholder.id = "debug.stepOver";
    placeholder.name = "Step Over";
    const ActionId p = *registry.add(std::move(placeholder));
    EXPECT_EQ(registry.tryInvoke(p, context()), InvokeResult::NoHandler);
    EXPECT_EQ(registry.tryInvoke(ActionId{1000}, context()), InvokeResult::Disabled) << "unknown ids never run";
}

TEST(ActionRegistry, HandlerMayRegisterMoreActions) {
    ActionRegistry registry;
    int ran = 0;
    std::string observedName;
    ActionSpec grow = spec("tools.grow", "Grow");
    grow.handler = [&registry, &ran, &observedName](const ActionContext&, const InvokeInfo&) {
        // Enough additions to force the deque and the index to reallocate.
        for (int i = 0; i < 200; ++i) {
            EXPECT_TRUE(registry.add(spec(std::format("tools.added{}", i))).has_value());
        }
        ++ran;
        // The handler's own captures and entry are still alive.
        observedName = registry.info(*registry.find("tools.grow")).name;
    };
    const ActionId id = *registry.add(std::move(grow));
    const ActionInfo& before = registry.info(id);
    EXPECT_EQ(registry.tryInvoke(id, context()), InvokeResult::Ran);
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(observedName, "Grow");
    EXPECT_EQ(registry.size(), 201U);
    EXPECT_EQ(&before, &registry.info(id)) << "info() references are stable";
    EXPECT_EQ(before.id, "tools.grow");
    EXPECT_TRUE(registry.find("tools.added199").has_value());
}

TEST(ActionRegistry, DeterministicOrderAndGeneration) {
    ActionRegistry registry;
    const std::vector<std::string> ids = {"z.last", "a.first", "m.middle", "b.second"};
    std::vector<ActionId> issued;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        issued.push_back(*registry.add(spec(ids[i])));
        EXPECT_EQ(registry.generation(), i + 1);
    }
    const auto all = registry.all();
    ASSERT_EQ(all.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(all[i], issued[i]) << "registration order";
        EXPECT_EQ(registry.info(all[i]).id, ids[i]);
        EXPECT_EQ(all[i].value(), i) << "ids are dense";
    }
    for (const std::string& id : ids) {
        EXPECT_TRUE(registry.find(id).has_value()) << id;
    }
}

TEST(ActionRegistry, KeymapActionsRegister) {
    ActionRegistry registry;
    test::InvocationLog log;
    const Keymap keymap = test::intellij();
    const std::vector<ActionId> ids = test::registerKeymapActions(registry, keymap, &log);
    EXPECT_GT(ids.size(), 200U);
    EXPECT_EQ(registry.size(), ids.size());
    const ActionId copy = *registry.find("editor.copy");
    EXPECT_EQ(registry.tryInvoke(copy, test::allContexts(), InvokeInfo{InvokeSource::Keyboard, true}),
              InvokeResult::Ran);
    ASSERT_EQ(log.calls.size(), 1U);
    EXPECT_EQ(log.calls[0].id, "editor.copy");
    EXPECT_TRUE(log.calls[0].info.autoRepeat);
}

} // namespace
} // namespace frost::input
