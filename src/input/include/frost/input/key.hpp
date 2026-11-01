// SPDX-License-Identifier: Apache-2.0
//
// The key model of frost::input (PLAN.md section 2.2 "Keyboard and IME", ADR 0016):
// physical key positions (Code), logical keys (Key), modifier sets, strokes, two-stroke
// sequences and double-tap gestures, plus the canonical text syntax used by keymap files
// and settings ("Ctrl+K, Ctrl+C", "DoubleShift").
/*
 * Two key spaces:
 *   - Code names a physical position, after the W3C "UI Events KeyboardEvent code Values"
 *     specification: KeyZ is the key right of Left Shift on every layout, even where it is
 *     labelled Y or W. Codes come from the OS scan code (Windows set-1 make codes, Linux
 *     evdev codes) and never depend on the active layout.
 *   - Key names what a binding refers to: letters, digits, the eleven US punctuation keys,
 *     named keys and the numpad, each named after its US-QWERTY key. The layout-aware
 *     mapping from Code to Key (layout.hpp) decides, per layout, which physical key a
 *     binding such as Ctrl+Z or Ctrl+Slash fires on.
 *
 * Writing keys are the 50 positions KeyA..IntlYen, whose meaning depends on the layout;
 * typing keys (isTypingKey) are the logical keys that type text without a modifier and so
 * can never be bound on their own (the Strict syntax rejects such bindings).
 *
 * Everything here is constexpr or O(1)/O(n) over the input, noexcept unless it allocates,
 * and thread-safe (no mutable state).
 */
#pragma once

#include "frost/core/error.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace frost::input {

// Physical key positions, named after W3C code values. Append-only within each group is
// not required (the values are never persisted; keymaps store Key names), but the
// groups' order is relied on by the range predicates below. The list macros' parameter is
// a lowercase x: an uppercase X would also replace the key name X in x(X). Formatting is
// off for the lists so they keep one group per row.
// clang-format off
#define FROST_INPUT_CODE_LIST(x)                                                                                       \
    x(Unknown)                                                                                                         \
    x(KeyA) x(KeyB) x(KeyC) x(KeyD) x(KeyE) x(KeyF) x(KeyG) x(KeyH) x(KeyI) x(KeyJ) x(KeyK) x(KeyL) x(KeyM) x(KeyN)    \
    x(KeyO) x(KeyP) x(KeyQ) x(KeyR) x(KeyS) x(KeyT) x(KeyU) x(KeyV) x(KeyW) x(KeyX) x(KeyY) x(KeyZ)                    \
    x(Digit0) x(Digit1) x(Digit2) x(Digit3) x(Digit4) x(Digit5) x(Digit6) x(Digit7) x(Digit8) x(Digit9)                \
    x(Minus) x(Equal) x(BracketLeft) x(BracketRight) x(Backslash) x(Semicolon) x(Quote) x(Backquote) x(Comma)          \
    x(Period) x(Slash) x(IntlBackslash) x(IntlRo) x(IntlYen)                                                           \
    x(Space) x(Enter) x(Tab) x(Backspace) x(Escape) x(Insert) x(Delete) x(Home) x(End) x(PageUp) x(PageDown)           \
    x(ArrowLeft) x(ArrowRight) x(ArrowUp) x(ArrowDown)                                                                 \
    x(F1) x(F2) x(F3) x(F4) x(F5) x(F6) x(F7) x(F8) x(F9) x(F10) x(F11) x(F12) x(F13) x(F14) x(F15) x(F16) x(F17)      \
    x(F18) x(F19) x(F20) x(F21) x(F22) x(F23) x(F24)                                                                   \
    x(PrintScreen) x(ScrollLock) x(Pause) x(ContextMenu) x(CapsLock) x(NumLock)                                        \
    x(Numpad0) x(Numpad1) x(Numpad2) x(Numpad3) x(Numpad4) x(Numpad5) x(Numpad6) x(Numpad7) x(Numpad8) x(Numpad9)      \
    x(NumpadAdd) x(NumpadSubtract) x(NumpadMultiply) x(NumpadDivide) x(NumpadDecimal) x(NumpadEnter)                   \
    x(ShiftLeft) x(ShiftRight) x(ControlLeft) x(ControlRight) x(AltLeft) x(AltRight) x(MetaLeft) x(MetaRight)

// Logical keys, named after their US-QWERTY key (Left/Right/Up/Down for the arrows). The
// enumerator names are the key names of the canonical syntax, so they are stable.
#define FROST_INPUT_KEY_LIST(x)                                                                                        \
    x(None)                                                                                                            \
    x(A) x(B) x(C) x(D) x(E) x(F) x(G) x(H) x(I) x(J) x(K) x(L) x(M) x(N) x(O) x(P) x(Q) x(R) x(S) x(T) x(U) x(V) x(W) \
    x(X) x(Y) x(Z)                                                                                                     \
    x(Digit0) x(Digit1) x(Digit2) x(Digit3) x(Digit4) x(Digit5) x(Digit6) x(Digit7) x(Digit8) x(Digit9)                \
    x(Backquote) x(Minus) x(Equal) x(BracketLeft) x(BracketRight) x(Backslash) x(Semicolon) x(Quote) x(Comma)          \
    x(Period) x(Slash)                                                                                                 \
    x(Space) x(Enter) x(Tab) x(Backspace) x(Escape) x(Insert) x(Delete) x(Home) x(End) x(PageUp) x(PageDown)           \
    x(Left) x(Right) x(Up) x(Down)                                                                                     \
    x(F1) x(F2) x(F3) x(F4) x(F5) x(F6) x(F7) x(F8) x(F9) x(F10) x(F11) x(F12) x(F13) x(F14) x(F15) x(F16) x(F17)      \
    x(F18) x(F19) x(F20) x(F21) x(F22) x(F23) x(F24)                                                                   \
    x(PrintScreen) x(Pause) x(ContextMenu)                                                                             \
    x(Numpad0) x(Numpad1) x(Numpad2) x(Numpad3) x(Numpad4) x(Numpad5) x(Numpad6) x(Numpad7) x(Numpad8) x(Numpad9)      \
    x(NumpadAdd) x(NumpadSubtract) x(NumpadMultiply) x(NumpadDivide) x(NumpadDecimal)
// clang-format on

#define FROST_INPUT_ENUMERATOR_(name) name,

enum class Code : std::uint8_t { FROST_INPUT_CODE_LIST(FROST_INPUT_ENUMERATOR_) };
enum class Key : std::uint8_t { FROST_INPUT_KEY_LIST(FROST_INPUT_ENUMERATOR_) };

#undef FROST_INPUT_ENUMERATOR_

// Number of Code and Key enumerators (array sizes for per-code and per-key tables).
inline constexpr std::size_t kCodeCount = static_cast<std::size_t>(Code::MetaRight) + 1;
inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::NumpadDecimal) + 1;
// Number of writing keys (KeyA..IntlYen) and the index of a writing code in per-writing-key tables.
inline constexpr std::size_t kWritingKeyCount =
    static_cast<std::size_t>(Code::IntlYen) - static_cast<std::size_t>(Code::KeyA) + 1;

[[nodiscard]] constexpr std::size_t index(Code code) noexcept { return static_cast<std::size_t>(code); }
[[nodiscard]] constexpr std::size_t index(Key key) noexcept { return static_cast<std::size_t>(key); }

// The 50 layout-dependent positions KeyA..IntlYen: 26 letters, 10 digits, the 11 US
// punctuation positions, IntlBackslash (the ISO key left of Z), IntlRo and IntlYen (JIS).
[[nodiscard]] constexpr bool isWritingKey(Code code) noexcept { return code >= Code::KeyA && code <= Code::IntlYen; }
// Writing keys plus Space: every position that types text without a modifier.
[[nodiscard]] constexpr bool isTypingCode(Code code) noexcept { return isWritingKey(code) || code == Code::Space; }
// The letter block KeyA..KeyZ (positions, not labels).
[[nodiscard]] constexpr bool isLetterAreaKey(Code code) noexcept { return code >= Code::KeyA && code <= Code::KeyZ; }
// The digit row Digit0..Digit9.
[[nodiscard]] constexpr bool isDigitRowKey(Code code) noexcept { return code >= Code::Digit0 && code <= Code::Digit9; }
// The eleven US punctuation positions Minus..Slash.
[[nodiscard]] constexpr bool isUsPunctuationKey(Code code) noexcept {
    return code >= Code::Minus && code <= Code::Slash;
}
// The eight modifier keys ShiftLeft..MetaRight.
[[nodiscard]] constexpr bool isModifierKey(Code code) noexcept {
    return code >= Code::ShiftLeft && code <= Code::MetaRight;
}
// The numpad keys Numpad0..NumpadEnter.
[[nodiscard]] constexpr bool isNumpadKey(Code code) noexcept {
    return code >= Code::Numpad0 && code <= Code::NumpadEnter;
}

// Letters, digits, the 11 punctuation keys and Space: the keys that type text on their own
// or with Shift, so a binding on them without Ctrl, Alt or Meta would swallow typing.
[[nodiscard]] constexpr bool isTypingKey(Key key) noexcept { return key >= Key::A && key <= Key::Space; }
[[nodiscard]] constexpr bool isLetterKey(Key key) noexcept { return key >= Key::A && key <= Key::Z; }
[[nodiscard]] constexpr bool isDigitKey(Key key) noexcept { return key >= Key::Digit0 && key <= Key::Digit9; }
[[nodiscard]] constexpr bool isFunctionKey(Key key) noexcept { return key >= Key::F1 && key <= Key::F24; }
[[nodiscard]] constexpr bool isNumpadKey(Key key) noexcept { return key >= Key::Numpad0 && key <= Key::NumpadDecimal; }

// The character a writing code produces on US-QWERTY without modifiers: a-z, 0-9 and the
// eleven punctuation characters ` - = [ ] \ ; ' , . /. 0 for IntlBackslash, IntlRo,
// IntlYen and every non-writing code. O(1).
[[nodiscard]] char usBaseChar(Code code) noexcept;
// The logical key named by a US base character (the inverse of usBaseChar over a-z, 0-9
// and the punctuation set; A-Z are accepted too), or Key::None. O(1).
[[nodiscard]] Key keyForUsChar(char32_t c) noexcept;
// The US base character of a typing key (a-z, 0-9, punctuation, ' ' for Space), or 0.
[[nodiscard]] char usCharForKey(Key key) noexcept;

// Maps Qt's 9-bit Windows scan code (the set-1 make code, with the extended flag of
// lParam bit 24 in bit 8) to a Code. Traps: NumLock is 0x45 with the extended flag and
// Pause 0x45 without it; Ctrl+Pause (Break) is 0x146 and maps to Pause; Alt+PrintScreen
// (SysRq) is 0x54 and maps to PrintScreen. Out-of-range or unassigned values give Unknown.
// O(1) table lookup; noexcept and thread-safe.
/*
 * Sources for the scan-code and evdev columns: Microsoft's scan-code table in "About
 * Keyboard Input" (learn.microsoft.com, Keyboard Input > Scan codes), the W3C UI Events
 * code-values specification, and the Linux evdev key codes as listed in xkeyboard-config's
 * keycodes/evdev (MIT). They were cross-checked against Chromium's
 * ui/events/keycodes/dom/dom_code_data.inc (BSD-3-Clause); no table text is copied.
 */
[[nodiscard]] Code codeFromWindowsScanCode(std::uint16_t scanCode) noexcept;
// Maps a Linux evdev key code (KEY_*; the X11/Wayland adapter subtracts 8 from the xkb
// keycode first) to a Code. Unassigned or out-of-range values give Unknown. O(1).
[[nodiscard]] Code codeFromEvdev(std::uint16_t evdev) noexcept;
// The reverse tables, for tests and the simulated keyboard: the scan code (9-bit form) or
// evdev code of a Code, or 0 when the code has none (Unknown).
[[nodiscard]] std::uint16_t windowsScanCodeFor(Code code) noexcept;
[[nodiscard]] std::uint16_t evdevFor(Code code) noexcept;

// Enumerator names ("KeyA", "ArrowLeft"; "Digit1", "Left"). Valid for the program's lifetime.
[[nodiscard]] std::string_view codeName(Code code) noexcept;
[[nodiscard]] std::string_view keyName(Key key) noexcept;

// The modifier bits of a stroke. Meta is the Windows/Super key, or Cmd on macOS.
enum class Modifier : std::uint8_t { Ctrl = 1, Alt = 2, Shift = 4, Meta = 8 };

// A set of modifiers; a trivially copyable bitmask with value semantics.
class Modifiers {
public:
    constexpr Modifiers() noexcept = default;
    // Implicit, so a single Modifier can be passed where a set is expected.
    constexpr Modifiers(Modifier modifier) noexcept // NOLINT(google-explicit-constructor)
        : m_bits(static_cast<std::uint8_t>(modifier)) {}

    // Builds a set from its bits; bits above Meta are dropped.
    [[nodiscard]] static constexpr Modifiers fromBits(std::uint8_t bits) noexcept {
        Modifiers result;
        result.m_bits = static_cast<std::uint8_t>(bits & 0x0F);
        return result;
    }

    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return m_bits; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_bits == 0; }
    [[nodiscard]] constexpr bool has(Modifier modifier) const noexcept {
        return (m_bits & static_cast<std::uint8_t>(modifier)) != 0;
    }
    [[nodiscard]] constexpr Modifiers with(Modifier modifier) const noexcept {
        return fromBits(static_cast<std::uint8_t>(m_bits | static_cast<std::uint8_t>(modifier)));
    }
    [[nodiscard]] constexpr Modifiers without(Modifier modifier) const noexcept {
        return fromBits(static_cast<std::uint8_t>(m_bits & ~static_cast<std::uint8_t>(modifier)));
    }
    // Ctrl, Alt or Meta: the modifiers that turn a typing key into a shortcut.
    [[nodiscard]] constexpr bool hasCommandModifier() const noexcept { return (m_bits & 0x0B) != 0; }

    friend constexpr Modifiers operator|(Modifiers a, Modifiers b) noexcept {
        return fromBits(static_cast<std::uint8_t>(a.m_bits | b.m_bits));
    }
    friend constexpr bool operator==(Modifiers, Modifiers) noexcept = default;
    friend constexpr auto operator<=>(Modifiers, Modifiers) noexcept = default;

private:
    std::uint8_t m_bits = 0;
};

constexpr Modifiers operator|(Modifier a, Modifier b) noexcept { return Modifiers(a) | Modifiers(b); }

// One key press with its modifiers: what a binding matches. 2 bytes, trivially copyable.
struct Stroke {
    Modifiers mods;
    Key key = Key::None;

    friend constexpr bool operator==(const Stroke&, const Stroke&) noexcept = default;
    friend constexpr auto operator<=>(const Stroke&, const Stroke&) noexcept = default;
};
static_assert(sizeof(Stroke) == 2, "Stroke is two bytes");

// Modifier double taps (Search Everywhere's double Shift, Run Anything's double Ctrl).
enum class Gesture : std::uint8_t { None, DoubleShift, DoubleCtrl };

// A binding's trigger: one stroke, two strokes ("Ctrl+K, Ctrl+C") or a gesture.
/*
 * Exactly one of these holds for a sequence built by the factories: gesture() != None and
 * length() == 0, or gesture() == None and length() is 1 or 2. A default-constructed
 * Sequence is the empty placeholder (length 0, no gesture; valid() is false), used only
 * where a value is required but unused (MatchResult for outcomes other than Fire). Unused
 * stroke slots are always value-initialised, so the defaulted comparisons and the hash
 * agree with logical equality.
 */
class Sequence {
public:
    constexpr Sequence() noexcept = default;

    [[nodiscard]] static constexpr Sequence of(Stroke first) noexcept {
        Sequence s;
        s.m_strokes[0] = first;
        s.m_length = 1;
        return s;
    }
    [[nodiscard]] static constexpr Sequence of(Stroke first, Stroke second) noexcept {
        Sequence s;
        s.m_strokes[0] = first;
        s.m_strokes[1] = second;
        s.m_length = 2;
        return s;
    }
    // Precondition: gesture != Gesture::None.
    [[nodiscard]] static Sequence of(Gesture gesture) noexcept;

    [[nodiscard]] constexpr std::uint8_t length() const noexcept { return m_length; }
    [[nodiscard]] constexpr Gesture gesture() const noexcept { return m_gesture; }
    [[nodiscard]] constexpr bool isGesture() const noexcept { return m_gesture != Gesture::None; }
    [[nodiscard]] constexpr bool valid() const noexcept { return m_gesture != Gesture::None || m_length != 0; }
    // The strokes; Stroke{} for slots at or beyond length().
    [[nodiscard]] constexpr Stroke first() const noexcept { return m_strokes[0]; }
    [[nodiscard]] constexpr Stroke second() const noexcept { return m_strokes[1]; }
    [[nodiscard]] constexpr Stroke stroke(std::size_t i) const noexcept { return i < 2 ? m_strokes[i] : Stroke{}; }

    friend constexpr bool operator==(const Sequence&, const Sequence&) noexcept = default;
    friend constexpr auto operator<=>(const Sequence&, const Sequence&) noexcept = default;

private:
    std::array<Stroke, 2> m_strokes{};
    std::uint8_t m_length = 0;
    Gesture m_gesture = Gesture::None;
};

// How strictly parseSequence reads its input.
//   Strict:  the canonical form of keymap files and settings strings (see parseSequence).
//   Lenient: user input such as Find Action queries: any case and modifier order, '+',
//            '-' or ' ' as separators, aliases and single characters.
enum class SyntaxMode : std::uint8_t { Strict, Lenient };

/*
 * Parses a sequence. Canonical (Strict) shape: "Ctrl+Alt+Shift+Meta+<KeyName>", the
 * modifiers in exactly that order, key names as in keyName(); two strokes as
 * "Ctrl+K, Ctrl+C" (a comma and exactly one space); gestures as "DoubleShift" and
 * "DoubleCtrl", alone.
 *
 * Strict rejects: duplicate or misordered modifiers, unknown keys, more than two strokes,
 * modifier-only strokes, gestures inside a sequence, wrong comma spacing, leading or
 * trailing junk, the empty string, and a single stroke or first stroke whose key is a
 * typing key (isTypingKey) with no modifier or Shift only, which would swallow typing. A
 * second stroke may be a bare typing key ("Ctrl+K, C"): it only matches after a prefix.
 *
 * Lenient: case-insensitive; modifiers in any order (repeats are ignored); '+', '-' or
 * spaces separate tokens; a separator character at the start of a stroke, at the end, or
 * directly after another separator is the key itself ("ctrl+-", "ctrl--", "ctrl++");
 * strokes are separated by a comma that follows a key ("ctrl+k, ctrl+c", "ctrl+k,c").
 * Aliases: Control->Ctrl; Cmd, Command, Win, Super->Meta; Option, Opt->Alt; Esc, Return,
 * Del, Ins, PgUp, PgDn, PgDown, Menu (ContextMenu); "num /", "num+", "num-", "num*",
 * "num." and "num 0".."num 9" for the numpad; "comma", "dot" (Period) and "plus". Single
 * characters a-z, 0-9, ` - = [ ] \ ; ' , . / name their US key; '+' and "plus" mean Equal
 * (on US, '+' is on the '=' key). Lenient accepts typing keys without modifiers ("a" parses).
 *
 * Errors: InvalidArgument with the message "column N: <reason>" (N 1-based, in bytes).
 * O(n) in the length of `text`; allocates only for the error message.
 */
[[nodiscard]] Result<Sequence> parseSequence(std::string_view text, SyntaxMode mode);

// The canonical (Strict) text of a valid sequence; parseSequence(formatSequence(s), Strict)
// == s for every Strict-valid s. The empty placeholder formats as "". O(1) allocations.
[[nodiscard]] std::string formatSequence(const Sequence& sequence);
// The canonical text of one stroke ("Ctrl+Shift+A").
[[nodiscard]] std::string formatStroke(Stroke stroke);

// ASCII and Latin-1 lowercase letters (U+00E0-U+00FE except U+00F7) and the Latin
// Extended-A letters the simulated layouts produce (ą ć ę ł ń ś ź ż) to uppercase; every
// other character is returned unchanged. Case pairs from the Unicode Character Database
// (UnicodeData.txt simple uppercase mappings).
[[nodiscard]] constexpr char32_t simpleUpper(char32_t c) noexcept {
    if (c >= U'a' && c <= U'z') {
        return c - 0x20;
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return c - 0x20;
    }
    switch (c) {
    case 0x0105: return 0x0104; // ą
    case 0x0107: return 0x0106; // ć
    case 0x0119: return 0x0118; // ę
    case 0x0142: return 0x0141; // ł
    case 0x0144: return 0x0143; // ń
    case 0x015B: return 0x015A; // ś
    case 0x017A: return 0x0179; // ź
    case 0x017C: return 0x017B; // ż
    default: return c;
    }
}

// Appends the UTF-8 encoding of `c` to `out`; surrogates and values above U+10FFFF append U+FFFD.
void appendUtf8(std::string& out, char32_t c);

} // namespace frost::input

// Sequences hash consistently with operator== (unused slots are value-initialised).
template<>
struct std::hash<frost::input::Sequence> {
    std::size_t operator()(const frost::input::Sequence& s) const noexcept {
        const auto a = s.first();
        const auto b = s.second();
        const std::size_t packed =
            static_cast<std::size_t>(a.mods.bits()) | (static_cast<std::size_t>(a.key) << 4U) |
            (static_cast<std::size_t>(b.mods.bits()) << 12U) | (static_cast<std::size_t>(b.key) << 16U) |
            (static_cast<std::size_t>(s.length()) << 24U) | (static_cast<std::size_t>(s.gesture()) << 26U);
        return std::hash<std::size_t>{}(packed);
    }
};
