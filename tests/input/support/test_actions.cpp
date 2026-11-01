// SPDX-License-Identifier: Apache-2.0
//
// Test support for frost::input (test_actions.hpp).

#include "input/support/test_actions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>

namespace frost::input {

void PrintTo(const Sequence& sequence, std::ostream* os) {
    *os << (sequence.valid() ? formatSequence(sequence) : std::string("<empty sequence>"));
}
void PrintTo(const Stroke& stroke, std::ostream* os) { *os << formatStroke(stroke); }
void PrintTo(Key key, std::ostream* os) { *os << "Key::" << keyName(key); }
void PrintTo(Code code, std::ostream* os) { *os << "Code::" << codeName(code); }
void PrintTo(MatchOutcome outcome, std::ostream* os) { *os << toString(outcome); }
void PrintTo(ActionId id, std::ostream* os) {
    if (id == kNoAction) {
        *os << "kNoAction";
    } else {
        *os << "ActionId(" << id.value() << ")";
    }
}

} // namespace frost::input

namespace frost::input::test {

ContextRule contextFor(std::string_view id) {
    if (id.starts_with("editor.")) {
        return ContextRule{{ContextKey::EditorFocus}, {}};
    }
    if (id == "window.focusEditor") {
        return ContextRule{{ContextKey::ToolWindowFocus}, {}};
    }
    if (id == "file.new") {
        return ContextRule{{ContextKey::ProjectViewFocus}, {}};
    }
    return ContextRule{};
}

std::vector<ActionId> registerKeymapActions(ActionRegistry& registry, const Keymap& keymap, InvocationLog* log) {
    std::vector<std::string> ids;
    for (const KeymapSection section :
         {KeymapSection::Default, KeymapSection::Windows, KeymapSection::Linux, KeymapSection::MacOS}) {
        for (const Binding& binding : keymap.section(section)) {
            if (std::ranges::find(ids, binding.actionId) == ids.end()) {
                ids.push_back(binding.actionId);
            }
        }
    }
    std::vector<ActionId> registered;
    for (const std::string& id : ids) {
        ActionSpec spec;
        spec.id = id;
        spec.name = id;
        spec.context = contextFor(id);
        spec.handler = [log, id](const ActionContext&, const InvokeInfo& info) {
            if (log != nullptr) {
                log->calls.push_back(Invocation{id, info});
            }
        };
        auto added = registry.add(std::move(spec));
        EXPECT_TRUE(added.has_value()) << id;
        if (added) {
            registered.push_back(*added);
        }
    }
    return registered;
}

ActionContext allContexts() {
    return ActionContext{.keys = ContextKeys::all(), .level = AvailabilityLevel::L4, .trusted = true};
}

Keymap intellij() {
    auto keymap = builtinKeymap("intellij");
    EXPECT_TRUE(keymap.has_value()) << (keymap ? std::string() : keymap.error().describe());
    return keymap ? std::move(*keymap) : Keymap{};
}

KeyEvent keyPress(Code code, Modifiers mods) {
    KeyEvent event;
    event.type = KeyEventType::Press;
    event.code = code;
    event.modifiers = mods;
    event.osTimeMs = 1000;
    return event;
}

KeyEvent keyRelease(Code code, Modifiers mods) {
    KeyEvent event = keyPress(code, mods);
    event.type = KeyEventType::Release;
    return event;
}

std::string actionName(const ActionRegistry& registry, ActionId id) {
    return id == kNoAction ? std::string() : registry.info(id).id;
}

} // namespace frost::input::test
