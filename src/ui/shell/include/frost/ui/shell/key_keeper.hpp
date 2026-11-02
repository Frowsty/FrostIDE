// SPDX-License-Identifier: Apache-2.0
//
// KeyKeeper: the interface a widget implements to keep some keys from the IDE's keymap while it has
// focus (the terminal's "terminal keeps these keys" table, ADR 0051).
#pragma once

class QKeyEvent;

namespace frost::ui {

/*
 * ShortcutDispatcher asks the widget a key event is delivered to: when it implements KeyKeeper and
 * keepsKey() is true, the dispatcher accepts the ShortcutOverride (no QShortcut or menu mnemonic
 * fires) and passes the press and release to the widget untouched, so no keymap action runs. The
 * answer must depend only on the event (key, modifiers, text) and the widget's state. GUI thread
 * only; must not block.
 */
class KeyKeeper {
public:
    [[nodiscard]] virtual bool keepsKey(const QKeyEvent& event) const = 0;

protected:
    KeyKeeper() = default;
    KeyKeeper(const KeyKeeper&) = default;
    KeyKeeper& operator=(const KeyKeeper&) = default;
    ~KeyKeeper() = default;
};

} // namespace frost::ui
