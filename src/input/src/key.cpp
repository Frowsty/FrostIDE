// SPDX-License-Identifier: Apache-2.0
//
// The key model's tables (scan codes, evdev codes, names, US characters) and the
// canonical and lenient sequence syntax (key.hpp; ADR 0016 decision 2).

#include "frost/input/key.hpp"

#include "frost/core/assert.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::input {

namespace {

// One physical key: its Code, its Windows 9-bit scan code (extended flag in bit 8) and its
// Linux evdev code. Sources are listed at codeFromWindowsScanCode in key.hpp.
struct ScanEntry {
    Code code;
    std::uint16_t windows;
    std::uint16_t evdev;
};

constexpr std::array kScanTable = {
    ScanEntry{Code::Escape, 0x01, 1},
    ScanEntry{Code::Digit1, 0x02, 2},
    ScanEntry{Code::Digit2, 0x03, 3},
    ScanEntry{Code::Digit3, 0x04, 4},
    ScanEntry{Code::Digit4, 0x05, 5},
    ScanEntry{Code::Digit5, 0x06, 6},
    ScanEntry{Code::Digit6, 0x07, 7},
    ScanEntry{Code::Digit7, 0x08, 8},
    ScanEntry{Code::Digit8, 0x09, 9},
    ScanEntry{Code::Digit9, 0x0A, 10},
    ScanEntry{Code::Digit0, 0x0B, 11},
    ScanEntry{Code::Minus, 0x0C, 12},
    ScanEntry{Code::Equal, 0x0D, 13},
    ScanEntry{Code::Backspace, 0x0E, 14},
    ScanEntry{Code::Tab, 0x0F, 15},
    ScanEntry{Code::KeyQ, 0x10, 16},
    ScanEntry{Code::KeyW, 0x11, 17},
    ScanEntry{Code::KeyE, 0x12, 18},
    ScanEntry{Code::KeyR, 0x13, 19},
    ScanEntry{Code::KeyT, 0x14, 20},
    ScanEntry{Code::KeyY, 0x15, 21},
    ScanEntry{Code::KeyU, 0x16, 22},
    ScanEntry{Code::KeyI, 0x17, 23},
    ScanEntry{Code::KeyO, 0x18, 24},
    ScanEntry{Code::KeyP, 0x19, 25},
    ScanEntry{Code::BracketLeft, 0x1A, 26},
    ScanEntry{Code::BracketRight, 0x1B, 27},
    ScanEntry{Code::Enter, 0x1C, 28},
    ScanEntry{Code::ControlLeft, 0x1D, 29},
    ScanEntry{Code::KeyA, 0x1E, 30},
    ScanEntry{Code::KeyS, 0x1F, 31},
    ScanEntry{Code::KeyD, 0x20, 32},
    ScanEntry{Code::KeyF, 0x21, 33},
    ScanEntry{Code::KeyG, 0x22, 34},
    ScanEntry{Code::KeyH, 0x23, 35},
    ScanEntry{Code::KeyJ, 0x24, 36},
    ScanEntry{Code::KeyK, 0x25, 37},
    ScanEntry{Code::KeyL, 0x26, 38},
    ScanEntry{Code::Semicolon, 0x27, 39},
    ScanEntry{Code::Quote, 0x28, 40},
    ScanEntry{Code::Backquote, 0x29, 41},
    ScanEntry{Code::ShiftLeft, 0x2A, 42},
    ScanEntry{Code::Backslash, 0x2B, 43},
    ScanEntry{Code::KeyZ, 0x2C, 44},
    ScanEntry{Code::KeyX, 0x2D, 45},
    ScanEntry{Code::KeyC, 0x2E, 46},
    ScanEntry{Code::KeyV, 0x2F, 47},
    ScanEntry{Code::KeyB, 0x30, 48},
    ScanEntry{Code::KeyN, 0x31, 49},
    ScanEntry{Code::KeyM, 0x32, 50},
    ScanEntry{Code::Comma, 0x33, 51},
    ScanEntry{Code::Period, 0x34, 52},
    ScanEntry{Code::Slash, 0x35, 53},
    ScanEntry{Code::ShiftRight, 0x36, 54},
    ScanEntry{Code::NumpadMultiply, 0x37, 55},
    ScanEntry{Code::AltLeft, 0x38, 56},
    ScanEntry{Code::Space, 0x39, 57},
    ScanEntry{Code::CapsLock, 0x3A, 58},
    ScanEntry{Code::F1, 0x3B, 59},
    ScanEntry{Code::F2, 0x3C, 60},
    ScanEntry{Code::F3, 0x3D, 61},
    ScanEntry{Code::F4, 0x3E, 62},
    ScanEntry{Code::F5, 0x3F, 63},
    ScanEntry{Code::F6, 0x40, 64},
    ScanEntry{Code::F7, 0x41, 65},
    ScanEntry{Code::F8, 0x42, 66},
    ScanEntry{Code::F9, 0x43, 67},
    ScanEntry{Code::F10, 0x44, 68},
    // NumLock is reported with the extended flag; Pause (E1 1D 45) without it.
    ScanEntry{Code::NumLock, 0x145, 69},
    ScanEntry{Code::ScrollLock, 0x46, 70},
    ScanEntry{Code::Numpad7, 0x47, 71},
    ScanEntry{Code::Numpad8, 0x48, 72},
    ScanEntry{Code::Numpad9, 0x49, 73},
    ScanEntry{Code::NumpadSubtract, 0x4A, 74},
    ScanEntry{Code::Numpad4, 0x4B, 75},
    ScanEntry{Code::Numpad5, 0x4C, 76},
    ScanEntry{Code::Numpad6, 0x4D, 77},
    ScanEntry{Code::NumpadAdd, 0x4E, 78},
    ScanEntry{Code::Numpad1, 0x4F, 79},
    ScanEntry{Code::Numpad2, 0x50, 80},
    ScanEntry{Code::Numpad3, 0x51, 81},
    ScanEntry{Code::Numpad0, 0x52, 82},
    ScanEntry{Code::NumpadDecimal, 0x53, 83},
    ScanEntry{Code::IntlBackslash, 0x56, 86},
    ScanEntry{Code::F11, 0x57, 87},
    ScanEntry{Code::F12, 0x58, 88},
    ScanEntry{Code::IntlRo, 0x73, 89},
    ScanEntry{Code::NumpadEnter, 0x11C, 96},
    ScanEntry{Code::ControlRight, 0x11D, 97},
    ScanEntry{Code::NumpadDivide, 0x135, 98},
    ScanEntry{Code::PrintScreen, 0x137, 99},
    ScanEntry{Code::AltRight, 0x138, 100},
    ScanEntry{Code::Home, 0x147, 102},
    ScanEntry{Code::ArrowUp, 0x148, 103},
    ScanEntry{Code::PageUp, 0x149, 104},
    ScanEntry{Code::ArrowLeft, 0x14B, 105},
    ScanEntry{Code::ArrowRight, 0x14D, 106},
    ScanEntry{Code::End, 0x14F, 107},
    ScanEntry{Code::ArrowDown, 0x150, 108},
    ScanEntry{Code::PageDown, 0x151, 109},
    ScanEntry{Code::Insert, 0x152, 110},
    ScanEntry{Code::Delete, 0x153, 111},
    ScanEntry{Code::Pause, 0x45, 119},
    ScanEntry{Code::IntlYen, 0x7D, 124},
    ScanEntry{Code::MetaLeft, 0x15B, 125},
    ScanEntry{Code::MetaRight, 0x15C, 126},
    ScanEntry{Code::ContextMenu, 0x15D, 127},
    ScanEntry{Code::F13, 0x64, 183},
    ScanEntry{Code::F14, 0x65, 184},
    ScanEntry{Code::F15, 0x66, 185},
    ScanEntry{Code::F16, 0x67, 186},
    ScanEntry{Code::F17, 0x68, 187},
    ScanEntry{Code::F18, 0x69, 188},
    ScanEntry{Code::F19, 0x6A, 189},
    ScanEntry{Code::F20, 0x6B, 190},
    ScanEntry{Code::F21, 0x6C, 191},
    ScanEntry{Code::F22, 0x6D, 192},
    ScanEntry{Code::F23, 0x6E, 193},
    ScanEntry{Code::F24, 0x76, 194},
};

constexpr std::size_t kWindowsScanCount = 512; // 9-bit scan codes.
constexpr std::size_t kEvdevCount = 256;

constexpr std::array<Code, kWindowsScanCount> kFromWindows = [] {
    std::array<Code, kWindowsScanCount> table{};
    for (const ScanEntry& e : kScanTable) {
        table[e.windows] = e.code;
    }
    table[0x146] = Code::Pause;      // Ctrl+Pause (Break).
    table[0x54] = Code::PrintScreen; // Alt+PrintScreen (SysRq).
    return table;
}();

constexpr std::array<Code, kEvdevCount> kFromEvdev = [] {
    std::array<Code, kEvdevCount> table{};
    for (const ScanEntry& e : kScanTable) {
        table[e.evdev] = e.code;
    }
    return table;
}();

constexpr std::array<std::uint16_t, kCodeCount> kToWindows = [] {
    std::array<std::uint16_t, kCodeCount> table{};
    for (const ScanEntry& e : kScanTable) {
        table[index(e.code)] = e.windows;
    }
    return table;
}();

constexpr std::array<std::uint16_t, kCodeCount> kToEvdev = [] {
    std::array<std::uint16_t, kCodeCount> table{};
    for (const ScanEntry& e : kScanTable) {
        table[index(e.code)] = e.evdev;
    }
    return table;
}();

// Every code but Unknown has a physical key in the table, exactly once.
static_assert(kScanTable.size() == kCodeCount - 1, "every Code except Unknown has a scan-table row");

#define FROST_INPUT_NAME_(name) std::string_view(#name),
constexpr std::array<std::string_view, kCodeCount> kCodeNames = {FROST_INPUT_CODE_LIST(FROST_INPUT_NAME_)};
constexpr std::array<std::string_view, kKeyCount> kKeyNames = {FROST_INPUT_KEY_LIST(FROST_INPUT_NAME_)};
#undef FROST_INPUT_NAME_

// US base characters of the writing codes KeyA..IntlYen, in Code order.
constexpr std::string_view kUsWritingChars = "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;'`,./";

// US characters of the typing keys A..Space, in Key order (Space is ' ').
constexpr std::string_view kUsKeyChars = "abcdefghijklmnopqrstuvwxyz0123456789`-=[]\\;',./ ";
static_assert(kUsKeyChars.size() == index(Key::Space) - index(Key::A) + 1, "one character per typing key");

constexpr char asciiLower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

// Equal ignoring ASCII case.
bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

std::unexpected<Error> syntaxError(std::size_t column, std::string_view reason) {
    return fail(ErrorCode::InvalidArgument, std::format("column {}: {}", column, reason));
}

std::optional<Modifier> strictModifier(std::string_view token) noexcept {
    if (token == "Ctrl") {
        return Modifier::Ctrl;
    }
    if (token == "Alt") {
        return Modifier::Alt;
    }
    if (token == "Shift") {
        return Modifier::Shift;
    }
    if (token == "Meta") {
        return Modifier::Meta;
    }
    return std::nullopt;
}

std::optional<Key> strictKey(std::string_view token) noexcept {
    for (std::size_t i = 1; i < kKeyCount; ++i) {
        if (kKeyNames[i] == token) {
            return static_cast<Key>(i);
        }
    }
    return std::nullopt;
}

std::optional<Gesture> gestureNamed(std::string_view token, bool ignoreCase) noexcept {
    const auto same = [&](std::string_view name) { return ignoreCase ? equalsIgnoreCase(token, name) : token == name; };
    if (same("DoubleShift")) {
        return Gesture::DoubleShift;
    }
    if (same("DoubleCtrl")) {
        return Gesture::DoubleCtrl;
    }
    return std::nullopt;
}

bool isTokenChar(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

// The Strict grammar (key.hpp).
Result<Sequence> parseStrict(std::string_view text) {
    if (text.empty()) {
        return syntaxError(1, "empty sequence");
    }
    if (const auto gesture = gestureNamed(text, false)) {
        return Sequence::of(*gesture);
    }
    std::array<Stroke, 2> strokes{};
    std::size_t count = 0;
    std::size_t i = 0;
    while (true) {
        const std::size_t strokeStart = i;
        if (count == 2) {
            return syntaxError(strokeStart + 1, "a sequence has at most two strokes");
        }
        Modifiers mods;
        int lastModifierOrder = -1;
        std::optional<Key> key;
        while (true) {
            const std::size_t tokenStart = i;
            while (i < text.size() && isTokenChar(text[i])) {
                ++i;
            }
            const std::string_view token = text.substr(tokenStart, i - tokenStart);
            if (token.empty()) {
                return syntaxError(tokenStart + 1, i < text.size() ? std::format("unexpected character '{}'", text[i])
                                                                   : std::string("missing key name"));
            }
            const bool more = i < text.size() && text[i] == '+';
            if (const auto modifier = strictModifier(token)) {
                if (!more) {
                    return syntaxError(tokenStart + 1, "a stroke needs a key after its modifiers");
                }
                const int order = std::countr_zero(static_cast<unsigned>(*modifier));
                if (mods.has(*modifier)) {
                    return syntaxError(tokenStart + 1, std::format("duplicate modifier {}", token));
                }
                if (order < lastModifierOrder) {
                    return syntaxError(tokenStart + 1, "modifiers must be in the order Ctrl+Alt+Shift+Meta");
                }
                lastModifierOrder = order;
                mods = mods.with(*modifier);
                ++i; // '+'
                continue;
            }
            if (gestureNamed(token, false)) {
                return syntaxError(tokenStart + 1, "a gesture cannot be combined with modifiers or strokes");
            }
            key = strictKey(token);
            if (!key) {
                return syntaxError(tokenStart + 1, std::format("unknown key '{}'", token));
            }
            if (more) {
                return syntaxError(i + 1, "the key must come last in a stroke");
            }
            break;
        }
        if (count == 0 && isTypingKey(*key) && !mods.hasCommandModifier()) {
            return syntaxError(strokeStart + 1, std::format("a binding on {} without Ctrl, Alt or Meta would swallow "
                                                            "typing",
                                                            keyName(*key)));
        }
        strokes[count++] = Stroke{mods, *key};
        if (i == text.size()) {
            break;
        }
        if (text[i] != ',') {
            return syntaxError(i + 1, std::format("unexpected character '{}'", text[i]));
        }
        if (i + 1 >= text.size() || text[i + 1] != ' ' || i + 2 >= text.size() || text[i + 2] == ' ') {
            return syntaxError(i + 1, "a comma must be followed by exactly one space and a stroke");
        }
        i += 2;
    }
    return count == 1 ? Sequence::of(strokes[0]) : Sequence::of(strokes[0], strokes[1]);
}

// Lenient token interpretation: a modifier, a key, or nothing.
struct LenientToken {
    std::optional<Modifier> modifier;
    std::optional<Key> key;
};

LenientToken lenientWord(std::string_view word) noexcept {
    static constexpr std::array<std::pair<std::string_view, Modifier>, 11> kModifiers = {{
        {"ctrl", Modifier::Ctrl},
        {"control", Modifier::Ctrl},
        {"alt", Modifier::Alt},
        {"option", Modifier::Alt},
        {"opt", Modifier::Alt},
        {"shift", Modifier::Shift},
        {"meta", Modifier::Meta},
        {"cmd", Modifier::Meta},
        {"command", Modifier::Meta},
        {"win", Modifier::Meta},
        {"super", Modifier::Meta},
    }};
    static constexpr std::array<std::pair<std::string_view, Key>, 11> kAliases = {{
        {"esc", Key::Escape},
        {"return", Key::Enter},
        {"del", Key::Delete},
        {"ins", Key::Insert},
        {"pgup", Key::PageUp},
        {"pgdn", Key::PageDown},
        {"pgdown", Key::PageDown},
        {"comma", Key::Comma},
        {"plus", Key::Equal},
        {"menu", Key::ContextMenu},
        {"dot", Key::Period},
    }};
    for (const auto& [name, modifier] : kModifiers) {
        if (equalsIgnoreCase(word, name)) {
            return {modifier, std::nullopt};
        }
    }
    for (const auto& [name, key] : kAliases) {
        if (equalsIgnoreCase(word, name)) {
            return {std::nullopt, key};
        }
    }
    if (word.size() == 1) {
        const Key key = keyForUsChar(static_cast<unsigned char>(word[0]));
        if (key != Key::None) {
            return {std::nullopt, key};
        }
    }
    if (word.size() == 4 && equalsIgnoreCase(word.substr(0, 3), "num") && word[3] >= '0' && word[3] <= '9') {
        return {std::nullopt, static_cast<Key>(index(Key::Numpad0) + static_cast<std::size_t>(word[3] - '0'))};
    }
    for (std::size_t i = 1; i < kKeyCount; ++i) {
        if (equalsIgnoreCase(word, kKeyNames[i])) {
            return {std::nullopt, static_cast<Key>(i)};
        }
    }
    return {};
}

// The numpad key "num" is followed by ('/', '*', '+', '-', '.', or a digit), or None.
Key numpadSuffix(char c) noexcept {
    switch (c) {
    case '/': return Key::NumpadDivide;
    case '*': return Key::NumpadMultiply;
    case '+': return Key::NumpadAdd;
    case '-': return Key::NumpadSubtract;
    case '.': return Key::NumpadDecimal;
    default:
        return c >= '0' && c <= '9' ? static_cast<Key>(index(Key::Numpad0) + static_cast<std::size_t>(c - '0'))
                                    : Key::None;
    }
}

bool isSeparator(char c) noexcept { return c == '+' || c == '-' || c == ' ' || c == '\t'; }
bool isKeyChar(char c) noexcept {
    return c == '`' || c == '=' || c == '[' || c == ']' || c == '\\' || c == ';' || c == '\'' || c == ',' || c == '.' ||
           c == '/';
}

// The Lenient grammar (key.hpp).
Result<Sequence> parseLenient(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
    }
    if (i == text.size()) {
        return syntaxError(1, "empty sequence");
    }
    {
        std::size_t end = text.size();
        while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
            --end;
        }
        if (const auto gesture = gestureNamed(text.substr(i, end - i), true)) {
            return Sequence::of(*gesture);
        }
    }
    std::array<Stroke, 2> strokes{};
    std::size_t count = 0;
    Modifiers mods;
    std::optional<Key> key;
    std::size_t strokeStart = i;
    bool atStrokeStart = true;

    const auto addKey = [&](Key k, std::size_t column) -> Result<void> {
        if (key) {
            return syntaxError(column, "a stroke has exactly one key");
        }
        key = k;
        return {};
    };
    const auto finishStroke = [&](std::size_t column) -> Result<void> {
        if (!key) {
            return syntaxError(column, "a stroke needs a key");
        }
        if (count == 2) {
            return syntaxError(strokeStart + 1, "a sequence has at most two strokes");
        }
        strokes[count++] = Stroke{mods, *key};
        mods = Modifiers();
        key.reset();
        return {};
    };

    while (i < text.size()) {
        const char c = text[i];
        if (isSeparator(c)) {
            // A separator run: spaces plus at most two '+'/'-' characters (see key.hpp).
            const std::size_t runStart = i;
            std::string signs;
            while (i < text.size() && isSeparator(text[i])) {
                if (text[i] == '+' || text[i] == '-') {
                    signs.push_back(text[i]);
                }
                ++i;
            }
            const bool atEnd = i == text.size() || (text[i] == ',' && key.has_value());
            if (signs.size() > 2) {
                return syntaxError(runStart + 1, "too many separators");
            }
            std::size_t keySigns = 0;
            if (atStrokeStart) {
                keySigns = signs.size(); // "+" or "-" at a stroke's start is the key itself.
            } else if (signs.size() == 2 || (signs.size() == 1 && atEnd)) {
                keySigns = 1; // "ctrl+-" uses its second sign; a final "ctrl+" uses its separator.
            }
            for (std::size_t s = signs.size() - keySigns; s < signs.size(); ++s) {
                if (auto added = addKey(signs[s] == '+' ? Key::Equal : Key::Minus, runStart + 1); !added) {
                    return std::unexpected(std::move(added.error()));
                }
            }
            atStrokeStart = atStrokeStart && keySigns == 0 && signs.empty();
            continue;
        }
        if (c == ',' && key.has_value()) {
            if (auto finished = finishStroke(i + 1); !finished) {
                return std::unexpected(std::move(finished.error()));
            }
            ++i;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
                ++i;
            }
            if (i == text.size()) {
                return syntaxError(i, "missing stroke after the comma");
            }
            strokeStart = i;
            atStrokeStart = true;
            continue;
        }
        if (isKeyChar(c)) {
            if (auto added = addKey(keyForUsChar(static_cast<unsigned char>(c)), i + 1); !added) {
                return std::unexpected(std::move(added.error()));
            }
            ++i;
            atStrokeStart = false;
            continue;
        }
        if (!isTokenChar(c)) {
            return syntaxError(i + 1, std::format("unexpected character '{}'", c));
        }
        const std::size_t wordStart = i;
        while (i < text.size() && isTokenChar(text[i])) {
            ++i;
        }
        const std::string_view word = text.substr(wordStart, i - wordStart);
        atStrokeStart = false;
        if (equalsIgnoreCase(word, "num") && i < text.size()) {
            // "num/", "num +", "num 7".
            std::size_t next = i;
            if (text[next] == ' ' && next + 1 < text.size()) {
                ++next;
            }
            const Key numpad = numpadSuffix(text[next]);
            if (numpad != Key::None) {
                if (auto added = addKey(numpad, wordStart + 1); !added) {
                    return std::unexpected(std::move(added.error()));
                }
                i = next + 1;
                continue;
            }
        }
        const LenientToken token = lenientWord(word);
        if (token.modifier) {
            mods = mods.with(*token.modifier);
        } else if (token.key) {
            if (auto added = addKey(*token.key, wordStart + 1); !added) {
                return std::unexpected(std::move(added.error()));
            }
        } else if (gestureNamed(word, true)) {
            return syntaxError(wordStart + 1, "a gesture cannot be combined with modifiers or strokes");
        } else {
            return syntaxError(wordStart + 1, std::format("unknown key '{}'", word));
        }
    }
    if (auto finished = finishStroke(text.size()); !finished) {
        return std::unexpected(std::move(finished.error()));
    }
    return count == 1 ? Sequence::of(strokes[0]) : Sequence::of(strokes[0], strokes[1]);
}

} // namespace

char usBaseChar(Code code) noexcept {
    if (code < Code::KeyA || code > Code::Slash) {
        return 0;
    }
    return kUsWritingChars[index(code) - index(Code::KeyA)];
}

Key keyForUsChar(char32_t c) noexcept {
    if (c >= U'A' && c <= U'Z') {
        c += 0x20;
    }
    if (c == U'+') {
        return Key::Equal;
    }
    if (c == U' ' || c > 0x7F) {
        return Key::None;
    }
    const std::size_t at = kUsKeyChars.find(static_cast<char>(c));
    return at >= kUsKeyChars.size() - 1 ? Key::None : static_cast<Key>(index(Key::A) + at);
}

char usCharForKey(Key key) noexcept {
    if (!isTypingKey(key)) {
        return 0;
    }
    return kUsKeyChars[index(key) - index(Key::A)];
}

Code codeFromWindowsScanCode(std::uint16_t scanCode) noexcept {
    return scanCode < kWindowsScanCount ? kFromWindows[scanCode] : Code::Unknown;
}

Code codeFromEvdev(std::uint16_t evdev) noexcept { return evdev < kEvdevCount ? kFromEvdev[evdev] : Code::Unknown; }

std::uint16_t windowsScanCodeFor(Code code) noexcept { return index(code) < kCodeCount ? kToWindows[index(code)] : 0; }

std::uint16_t evdevFor(Code code) noexcept { return index(code) < kCodeCount ? kToEvdev[index(code)] : 0; }

std::string_view codeName(Code code) noexcept {
    return index(code) < kCodeCount ? kCodeNames[index(code)] : std::string_view("Code(?)");
}

std::string_view keyName(Key key) noexcept {
    return index(key) < kKeyCount ? kKeyNames[index(key)] : std::string_view("Key(?)");
}

Sequence Sequence::of(Gesture gesture) noexcept {
    FROST_PRE(gesture != Gesture::None);
    Sequence s;
    s.m_gesture = gesture;
    return s;
}

Result<Sequence> parseSequence(std::string_view text, SyntaxMode mode) {
    return mode == SyntaxMode::Strict ? parseStrict(text) : parseLenient(text);
}

std::string formatStroke(Stroke stroke) {
    std::string out;
    if (stroke.mods.has(Modifier::Ctrl)) {
        out += "Ctrl+";
    }
    if (stroke.mods.has(Modifier::Alt)) {
        out += "Alt+";
    }
    if (stroke.mods.has(Modifier::Shift)) {
        out += "Shift+";
    }
    if (stroke.mods.has(Modifier::Meta)) {
        out += "Meta+";
    }
    out += keyName(stroke.key);
    return out;
}

std::string formatSequence(const Sequence& sequence) {
    switch (sequence.gesture()) {
    case Gesture::DoubleShift: return "DoubleShift";
    case Gesture::DoubleCtrl: return "DoubleCtrl";
    case Gesture::None: break;
    }
    if (sequence.length() == 0) {
        return {};
    }
    std::string out = formatStroke(sequence.first());
    if (sequence.length() == 2) {
        out += ", ";
        out += formatStroke(sequence.second());
    }
    return out;
}

void appendUtf8(std::string& out, char32_t c) {
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        c = 0xFFFD;
    }
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

} // namespace frost::input
