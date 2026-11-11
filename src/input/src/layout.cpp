// SPDX-License-Identifier: Apache-2.0
//
// LayoutTable's run-time entry points, strokeFromEvent (the AltGr, dead-key and NumLock
// policy shared by the matcher and shortcut capture) and toDisplayText (layout.hpp,
// keymap.hpp; ADR 0016 decisions 3 and 4). The mapping core itself is the constexpr
// detail::computeLayout in layout.hpp.

#include "frost/input/layout.hpp"

#include "frost/core/i18n.hpp"
#include "frost/input/keymap.hpp"

#include <algorithm>
#include <string>

namespace frost::input {

namespace detail {

std::string_view describe(LayoutError error) noexcept {
    switch (error) {
    case LayoutError::None: return "valid";
    case LayoutError::IdTooLong: return "layout id longer than 23 bytes";
    case LayoutError::NotWritingKey: return "entry for a code that is not a writing key";
    case LayoutError::DuplicateCode: return "duplicate code";
    case LayoutError::BadCodePoint: return "surrogate or value above U+10FFFF";
    case LayoutError::MaskWithoutChar: return "dead or ligature bit on a level without a character";
    case LayoutError::CompositionOrder: return "compositions not sorted by (dead, base) or not unique";
    case LayoutError::BadComposition: return "composition with a zero or invalid character";
    }
    return "LayoutError(?)";
}

void invalidLayoutTable() noexcept {}

} // namespace detail

Result<LayoutTable> LayoutTable::make(std::string_view id, MappingMode mode,
                                      std::span<const std::pair<Code, KeyLevels>> keys,
                                      std::span<const Composition> compositions) {
    LayoutTable table;
    if (const auto error = detail::computeLayout(table, id, mode, keys, compositions);
        error != detail::LayoutError::None) {
        return fail(ErrorCode::InvalidArgument,
                    std::string("invalid layout table: ") + std::string(detail::describe(error)));
    }
    return table;
}

std::optional<char32_t> LayoutTable::compose(char32_t dead, char32_t base) const noexcept {
    const auto it = std::lower_bound(m_compositions.begin(), m_compositions.end(), Composition{dead, base, 0},
                                     [](const Composition& a, const Composition& b) {
                                         return a.dead != b.dead ? a.dead < b.dead : a.base < b.base;
                                     });
    if (it != m_compositions.end() && it->dead == dead && it->base == base) {
        return it->composed;
    }
    return std::nullopt;
}

bool operator==(const LayoutTable& a, const LayoutTable& b) noexcept {
    return a.id() == b.id() && a.m_mode == b.m_mode && a.m_levels == b.m_levels &&
           std::ranges::equal(a.m_compositions, b.m_compositions);
}

namespace {

// The NumLock-off meaning of numpad digits and the decimal key.
Key numLockOffKey(Code code) noexcept {
    switch (code) {
    case Code::Numpad8: return Key::Up;
    case Code::Numpad2: return Key::Down;
    case Code::Numpad4: return Key::Left;
    case Code::Numpad6: return Key::Right;
    case Code::Numpad7: return Key::Home;
    case Code::Numpad1: return Key::End;
    case Code::Numpad9: return Key::PageUp;
    case Code::Numpad3: return Key::PageDown;
    case Code::Numpad0: return Key::Insert;
    case Code::NumpadDecimal: return Key::Delete;
    default: return Key::None;
    }
}

} // namespace

std::optional<Stroke> strokeFromEvent(const KeyEvent& event, const LayoutTable& layout) noexcept {
    if (!event.isPress() || event.imeComposing || isModifierKey(event.code)) {
        return std::nullopt;
    }
    // Matcher step 6: a dead key without Ctrl, Alt or Meta composes; the OS handles it.
    if (event.deadKey != 0 && !event.modifiers.hasCommandModifier()) {
        return std::nullopt;
    }
    // Step 7: the logical key, with the NumLock-off numpad remap.
    Key key = layout.logicalKey(event.code, event.keyCharHint);
    if (!event.numLock && event.code >= Code::Numpad0 && event.code <= Code::NumpadDecimal &&
        (event.code <= Code::Numpad9 || event.code == Code::NumpadDecimal)) {
        key = numLockOffKey(event.code);
    }
    if (key == Key::None) {
        return std::nullopt;
    }
    // Step 8: the AltGr policy.
    Modifiers mods = event.modifiers;
    if (event.altGr) {
        const bool shift = event.modifiers.has(Modifier::Shift);
        const KeyLevels levels = layout.levels(event.code);
        const char32_t level = shift ? levels.altGrShift : levels.altGr;
        if (event.hasPrintableText() || level != 0) {
            return std::nullopt; // AltGr text: the widget types it.
        }
        // AltGr that produces nothing acts as Ctrl+Alt (Windows reports AltGr as Ctrl+Alt).
        mods = Modifier::Ctrl | Modifier::Alt;
        if (shift) {
            mods = mods.with(Modifier::Shift);
        }
        if (event.modifiers.has(Modifier::Meta)) {
            mods = mods.with(Modifier::Meta);
        }
    }
    return Stroke{mods, key};
}

namespace {

std::string_view namedKeyText(Key key) noexcept {
    switch (key) {
    case Key::Space: return "Space";
    case Key::Enter: return "Enter";
    case Key::Tab: return "Tab";
    case Key::Backspace: return "Backspace";
    case Key::Escape: return "Esc";
    case Key::Insert: return "Insert";
    case Key::Delete: return "Delete";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::PageUp: return "Page Up";
    case Key::PageDown: return "Page Down";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::PrintScreen: return "Print Screen";
    case Key::Pause: return "Pause";
    case Key::ContextMenu: return "Menu";
    case Key::NumpadAdd: return "Num +";
    case Key::NumpadSubtract: return "Num -";
    case Key::NumpadMultiply: return "Num *";
    case Key::NumpadDivide: return "Num /";
    case Key::NumpadDecimal: return "Num .";
    default: return {};
    }
}

// The i18n context of key and modifier names (frost/core/i18n.hpp, ADR 0038).
constexpr std::string_view kKeyNames = "frost::input::keys";

// Appends the translated modifier or key name `name` and, when `plus`, the "+" separator.
void appendName(std::string& out, std::string_view name, bool plus) {
    out += i18n::tr(kKeyNames, name);
    if (plus) {
        out.push_back('+');
    }
}

void appendStrokeText(std::string& out, Stroke stroke, const LayoutTable& layout, KeymapPlatform platform) {
    if (stroke.mods.has(Modifier::Ctrl)) {
        appendName(out, "Ctrl", true);
    }
    if (stroke.mods.has(Modifier::Alt)) {
        appendName(out, platform == KeymapPlatform::MacOS ? "Option" : "Alt", true);
    }
    if (stroke.mods.has(Modifier::Shift)) {
        appendName(out, "Shift", true);
    }
    if (stroke.mods.has(Modifier::Meta)) {
        appendName(out,
                   platform == KeymapPlatform::Windows ? "Win"
                   : platform == KeymapPlatform::Linux ? "Super"
                                                       : "Cmd",
                   true);
    }
    const Key key = stroke.key;
    if (isDigitKey(key)) {
        out.push_back(static_cast<char>('0' + (index(key) - index(Key::Digit0))));
    } else if (isNumpadKey(key) && key <= Key::Numpad9) {
        appendName(out, "Num", false);
        out.push_back(' ');
        out.push_back(static_cast<char>('0' + (index(key) - index(Key::Numpad0))));
    } else if (isTypingKey(key) && key != Key::Space) {
        const char32_t label = layout.labelFor(key);
        appendUtf8(out, label != 0 ? simpleUpper(label) : simpleUpper(static_cast<unsigned char>(usCharForKey(key))));
    } else if (const std::string_view named = namedKeyText(key); !named.empty()) {
        appendName(out, named, false);
    } else {
        out += keyName(key);
    }
}

} // namespace

std::string toDisplayText(const Sequence& sequence, const LayoutTable& layout, KeymapPlatform platform) {
    switch (sequence.gesture()) {
    case Gesture::DoubleShift: return i18n::tr(kKeyNames, "Double Shift");
    case Gesture::DoubleCtrl: return i18n::tr(kKeyNames, "Double Ctrl");
    case Gesture::None: break;
    }
    std::string out;
    for (std::size_t i = 0; i < sequence.length(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        appendStrokeText(out, sequence.stroke(i), layout, platform);
    }
    return out;
}

} // namespace frost::input
