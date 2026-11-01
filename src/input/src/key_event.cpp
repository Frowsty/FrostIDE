// SPDX-License-Identifier: Apache-2.0
//
// KeyEvent helpers: printable-text detection and typedText, the rule that decides which
// pass-through presses insert text (key_event.hpp; ADR 0016 decision 4).

#include "frost/input/key_event.hpp"

namespace frost::input {

bool KeyEvent::hasPrintableText() const noexcept {
    for (std::uint8_t i = 0; i < textLength && i < text.size(); ++i) {
        if (text[i] >= 0x20 && text[i] != 0x7F) {
            return true;
        }
    }
    return false;
}

std::u32string_view typedText(const KeyEvent& event, KeyPlatform platform) noexcept {
    if (!event.isPress() || !event.hasPrintableText()) {
        return {};
    }
    const bool ctrl = event.modifiers.has(Modifier::Ctrl);
    const bool alt = event.modifiers.has(Modifier::Alt);
    const bool meta = event.modifiers.has(Modifier::Meta);
    const bool typed =
        (!ctrl && !alt && !meta) || event.altGr || (platform == KeyPlatform::Windows && ctrl && alt && !meta);
    return typed ? event.textView() : std::u32string_view();
}

} // namespace frost::input
