// SPDX-License-Identifier: Apache-2.0
//
// Test support for frost::input: registering the actions a keymap names (with stub
// handlers that record their invocations), the all-permissive context, and small event
// helpers shared by the suites.
#pragma once

#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/shortcut_matcher.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {

// Readable gtest output for the key-model types (found by ADL). Every input test
// includes this header, so all translation units print these types the same way.
void PrintTo(const Sequence& sequence, std::ostream* os);
void PrintTo(const Stroke& stroke, std::ostream* os);
void PrintTo(Key key, std::ostream* os);
void PrintTo(Code code, std::ostream* os);
void PrintTo(MatchOutcome outcome, std::ostream* os);
void PrintTo(ActionId id, std::ostream* os);

} // namespace frost::input

namespace frost::input::test {

// One recorded handler call.
struct Invocation {
    std::string id;
    InvokeInfo info;
};

// The handlers' shared record (owned by the test; must outlive the registry's handlers).
struct InvocationLog {
    std::vector<Invocation> calls;
};

// The context rule the keymap actions get: editor.* need EditorFocus, window.focusEditor
// ToolWindowFocus, file.new ProjectViewFocus; everything else is global.
[[nodiscard]] ContextRule contextFor(std::string_view id);

// Registers every action id `keymap` names in any section, in first-appearance order
// (default, windows, linux, macos), with contextFor() rules and handlers that append to
// `log` (or do nothing when it is null). Returns the ids in registration order.
std::vector<ActionId> registerKeymapActions(ActionRegistry& registry, const Keymap& keymap,
                                            InvocationLog* log = nullptr);

// Every context key set, level L4, trusted: everything registered is enabled.
[[nodiscard]] ActionContext allContexts();

// The built-in IntelliJ keymap; fails the test (and returns an empty keymap) if it does not parse.
[[nodiscard]] Keymap intellij();

// A press or release of `code` with the given modifiers and no text (for matcher tests
// that do not need the simulator). osTimeMs is 1000.
[[nodiscard]] KeyEvent keyPress(Code code, Modifiers mods = {});
[[nodiscard]] KeyEvent keyRelease(Code code, Modifiers mods = {});

// The string id of a matcher result's action ("" for none).
[[nodiscard]] std::string actionName(const ActionRegistry& registry, ActionId id);

} // namespace frost::input::test
