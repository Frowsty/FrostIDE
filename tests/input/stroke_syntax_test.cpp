// SPDX-License-Identifier: Apache-2.0
//
// StrokeSyntax: the canonical (Strict) and Lenient sequence syntax of key.hpp, the
// scan-code and evdev tables, and the key-model helpers.

#include "frost/input/key.hpp"
#include "frost/input/keymap.hpp"

#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

namespace frost::input {
namespace {

Stroke stroke(Modifiers mods, Key key) { return Stroke{mods, key}; }

std::string strictOf(std::string_view text, SyntaxMode mode) {
    auto parsed = parseSequence(text, mode);
    return parsed ? formatSequence(*parsed) : "error: " + parsed.error().message();
}

TEST(StrokeSyntax, EveryStrictStrokeRoundTrips) {
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (unsigned bits = 0; bits < 16; ++bits) {
        const Modifiers mods = Modifiers::fromBits(static_cast<std::uint8_t>(bits));
        for (std::size_t k = 1; k < kKeyCount; ++k) {
            const Sequence s = Sequence::of(stroke(mods, static_cast<Key>(k)));
            const std::string text = formatSequence(s);
            auto parsed = parseSequence(text, SyntaxMode::Strict);
            if (isTypingKey(static_cast<Key>(k)) && !mods.hasCommandModifier()) {
                EXPECT_FALSE(parsed.has_value()) << text << " would swallow typing";
                ++rejected;
            } else {
                ASSERT_TRUE(parsed.has_value()) << text << ": " << parsed.error().message();
                EXPECT_EQ(*parsed, s) << text;
                ++accepted;
            }
        }
    }
    // 48 typing keys rejected with no modifier and with Shift alone.
    EXPECT_EQ(rejected, 2U * 48U);
    EXPECT_EQ(accepted + rejected, 16U * (kKeyCount - 1));
}

TEST(StrokeSyntax, RandomTwoStrokeSequencesRoundTrip) {
    std::mt19937_64 rng(0x1D6u);
    std::uniform_int_distribution<unsigned> modDist(0, 15);
    std::uniform_int_distribution<std::size_t> keyDist(1, kKeyCount - 1);
    std::size_t bareSeconds = 0;
    for (int i = 0; i < 10'000; ++i) {
        Stroke first;
        do {
            first =
                stroke(Modifiers::fromBits(static_cast<std::uint8_t>(modDist(rng))), static_cast<Key>(keyDist(rng)));
        } while (isTypingKey(first.key) && !first.mods.hasCommandModifier());
        const Stroke second =
            stroke(Modifiers::fromBits(static_cast<std::uint8_t>(modDist(rng))), static_cast<Key>(keyDist(rng)));
        if (second.mods.empty() && isTypingKey(second.key)) {
            ++bareSeconds;
        }
        const Sequence s = Sequence::of(first, second);
        const std::string text = formatSequence(s);
        auto parsed = parseSequence(text, SyntaxMode::Strict);
        ASSERT_TRUE(parsed.has_value()) << text << ": " << parsed.error().message();
        EXPECT_EQ(*parsed, s) << text;
    }
    EXPECT_GT(bareSeconds, 0U) << "the sample covers bare second strokes";
}

TEST(StrokeSyntax, GesturesRoundTrip) {
    for (const Gesture g : {Gesture::DoubleShift, Gesture::DoubleCtrl}) {
        const Sequence s = Sequence::of(g);
        EXPECT_TRUE(s.isGesture());
        EXPECT_EQ(s.length(), 0);
        auto parsed = parseSequence(formatSequence(s), SyntaxMode::Strict);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, s);
    }
    EXPECT_EQ(formatSequence(Sequence::of(Gesture::DoubleShift)), "DoubleShift");
    EXPECT_EQ(formatSequence(Sequence()), "");
    EXPECT_FALSE(Sequence().valid());
}

TEST(StrokeSyntax, BareSecondStrokeIsAccepted) {
    auto parsed = parseSequence("Ctrl+K, C", SyntaxMode::Strict);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    EXPECT_EQ(*parsed, Sequence::of(stroke(Modifier::Ctrl, Key::K), stroke({}, Key::C)));
    EXPECT_EQ(formatSequence(*parsed), "Ctrl+K, C");
}

TEST(StrokeSyntax, NamedKeysNeedNoModifier) {
    for (const std::string_view text : {"F5", "Escape", "Delete", "Left", "PageDown", "NumpadAdd", "Shift+F10",
                                        "Shift+Insert", "Enter", "Tab", "Backspace"}) {
        auto parsed = parseSequence(text, SyntaxMode::Strict);
        ASSERT_TRUE(parsed.has_value()) << text;
        EXPECT_EQ(formatSequence(*parsed), text);
    }
}

TEST(StrokeSyntax, EveryBuiltinLineIsCanonical) {
    const std::string_view text = builtinKeymapText("intellij");
    ASSERT_FALSE(text.empty());
    auto keymap = parseKeymap(text);
    ASSERT_TRUE(keymap.has_value()) << keymap.error().message();
    const std::string formatted = formatKeymap(*keymap);
    std::set<std::string> canonical;
    for (std::size_t start = 0; start < formatted.size();) {
        const std::size_t end = formatted.find('\n', start);
        canonical.insert(formatted.substr(start, end - start));
        start = end + 1;
    }
    std::size_t bindings = 0;
    for (std::size_t start = 0; start < text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string line(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == '[' || line.find(" =") == std::string::npos) {
            continue;
        }
        ++bindings;
        EXPECT_TRUE(canonical.contains(line)) << "not canonical: " << line;
    }
    EXPECT_GT(bindings, 200U);
}

struct LenientCase {
    std::string_view input;
    std::string_view canonical;
};

TEST(StrokeSyntax, LenientAliasesAndOrders) {
    const LenientCase cases[] = {
        {"ctrl+shift+a", "Ctrl+Shift+A"},
        {"Shift+Ctrl+A", "Ctrl+Shift+A"},
        {"shift ctrl a", "Ctrl+Shift+A"},
        {"CTRL-ALT-DEL", "Ctrl+Alt+Delete"},
        {"control+alt+del", "Ctrl+Alt+Delete"},
        {"cmd+s", "Meta+S"},
        {"command+s", "Meta+S"},
        {"win+e", "Meta+E"},
        {"super+e", "Meta+E"},
        {"option+left", "Alt+Left"},
        {"ctrl+ctrl+a", "Ctrl+A"},
        {"ctrl plus", "Ctrl+Equal"},
        {"ctrl++", "Ctrl+Equal"},
        {"ctrl+-", "Ctrl+Minus"},
        {"ctrl--", "Ctrl+Minus"},
        {"ctrl - a", "Ctrl+A"},
        {"alt+1", "Alt+Digit1"},
        {"ctrl+shift+=", "Ctrl+Shift+Equal"},
        {"ctrl+,", "Ctrl+Comma"},
        {"ctrl+comma", "Ctrl+Comma"},
        {"ctrl+/", "Ctrl+Slash"},
        {"ctrl+[", "Ctrl+BracketLeft"},
        {"alt+`", "Alt+Backquote"},
        {"esc", "Escape"},
        {"return", "Enter"},
        {"pgup", "PageUp"},
        {"pgdn", "PageDown"},
        {"ins", "Insert"},
        {"menu", "ContextMenu"},
        {"num /", "NumpadDivide"},
        {"ctrl+num+", "Ctrl+NumpadAdd"},
        {"num-", "NumpadSubtract"},
        {"num*", "NumpadMultiply"},
        {"num.", "NumpadDecimal"},
        {"num 7", "Numpad7"},
        {"num7", "Numpad7"},
        {"ctrl+numpaddivide", "Ctrl+NumpadDivide"},
        {"a", "A"},
        {"f5", "F5"},
        {"shift+f6", "Shift+F6"},
        {"  ctrl + s  ", "Ctrl+S"},
        {"ctrl+k, ctrl+c", "Ctrl+K, Ctrl+C"},
        {"ctrl+k,c", "Ctrl+K, C"},
        {"Ctrl+K, C", "Ctrl+K, C"},
        {"doubleshift", "DoubleShift"},
        {"DoubleCtrl", "DoubleCtrl"},
        {"+", "Equal"},
        {"-", "Minus"},
    };
    for (const LenientCase& c : cases) {
        EXPECT_EQ(strictOf(c.input, SyntaxMode::Lenient), c.canonical) << "input: '" << c.input << "'";
    }
}

TEST(StrokeSyntax, LenientErrors) {
    for (const std::string_view text : {"", "   ", "ctrl", "ctrl+shift", "ctrl+foo", "a b", "ctrl+k, ",
                                        "ctrl+doubleshift", "a, b, c", "ctrl+@", "ctrl+++"}) {
        EXPECT_FALSE(parseSequence(text, SyntaxMode::Lenient).has_value()) << "input: '" << text << "'";
    }
}

struct StrictError {
    std::string_view input;
    std::string_view prefix; // "column N: reason..."
};

TEST(StrokeSyntax, StrictErrorsCarryColumns) {
    const StrictError cases[] = {
        {"", "column 1: empty sequence"},
        {"Ctrl+Ctrl+A", "column 6: duplicate modifier Ctrl"},
        {"Shift+Ctrl+A", "column 7: modifiers must be in the order"},
        {"Ctrl+Foo", "column 6: unknown key 'Foo'"},
        {"ctrl+a", "column 1: unknown key 'ctrl'"},
        {"Ctrl+K, Ctrl+C, Ctrl+D", "column 17: a sequence has at most two strokes"},
        {"Ctrl", "column 1: a stroke needs a key after its modifiers"},
        {"Ctrl+", "column 6: missing key name"},
        {"Ctrl+DoubleShift", "column 6: a gesture cannot be combined"},
        {"DoubleShift, Ctrl+A", "column 1: a gesture cannot be combined"},
        {"A", "column 1: a binding on A without Ctrl, Alt or Meta would swallow typing"},
        {"Shift+A", "column 1: a binding on A without"},
        {"Space", "column 1: a binding on Space without"},
        {"Shift+Digit1", "column 1: a binding on Digit1 without"},
        {"Ctrl+K, Shift+A, Ctrl+B", "column 18: a sequence has at most two strokes"},
        {"Ctrl+K,Ctrl+C", "column 7: a comma must be followed by exactly one space"},
        {"Ctrl+K,  Ctrl+C", "column 7: a comma must be followed by exactly one space"},
        {"Ctrl+K, ", "column 7: a comma must be followed by exactly one space"},
        {"Ctrl+A ", "column 7: unexpected character ' '"},
        {"Ctrl+A+B", "column 7: the key must come last"},
        {" Ctrl+A", "column 1: unexpected character ' '"},
        {"Ctrl+A;", "column 7: unexpected character ';'"},
    };
    for (const StrictError& c : cases) {
        auto parsed = parseSequence(c.input, SyntaxMode::Strict);
        ASSERT_FALSE(parsed.has_value()) << "accepted: '" << c.input << "'";
        EXPECT_EQ(parsed.error().code(), ErrorCode::InvalidArgument);
        EXPECT_TRUE(parsed.error().message().starts_with(c.prefix))
            << "input '" << c.input << "': " << parsed.error().message();
    }
}

TEST(StrokeSyntax, ScanCodeTablesAreBijective) {
    std::unordered_set<std::uint16_t> windows;
    std::unordered_set<std::uint16_t> evdev;
    for (std::size_t i = 1; i < kCodeCount; ++i) {
        const auto code = static_cast<Code>(i);
        const std::uint16_t scan = windowsScanCodeFor(code);
        const std::uint16_t ev = evdevFor(code);
        ASSERT_NE(scan, 0) << codeName(code);
        ASSERT_NE(ev, 0) << codeName(code);
        EXPECT_TRUE(windows.insert(scan).second) << codeName(code);
        EXPECT_TRUE(evdev.insert(ev).second) << codeName(code);
        EXPECT_EQ(codeFromWindowsScanCode(scan), code) << codeName(code);
        EXPECT_EQ(codeFromEvdev(ev), code) << codeName(code);
    }
}

TEST(StrokeSyntax, ScanCodeTraps) {
    EXPECT_EQ(codeFromWindowsScanCode(0x145), Code::NumLock);
    EXPECT_EQ(codeFromWindowsScanCode(0x045), Code::Pause);
    EXPECT_EQ(codeFromWindowsScanCode(0x146), Code::Pause);       // Ctrl+Pause (Break).
    EXPECT_EQ(codeFromWindowsScanCode(0x054), Code::PrintScreen); // Alt+PrintScreen (SysRq).
    EXPECT_EQ(codeFromWindowsScanCode(0x137), Code::PrintScreen);
    EXPECT_EQ(codeFromWindowsScanCode(0x01C), Code::Enter);
    EXPECT_EQ(codeFromWindowsScanCode(0x11C), Code::NumpadEnter);
    EXPECT_EQ(codeFromWindowsScanCode(0x038), Code::AltLeft);
    EXPECT_EQ(codeFromWindowsScanCode(0x138), Code::AltRight);
    EXPECT_EQ(codeFromWindowsScanCode(0x047), Code::Numpad7);
    EXPECT_EQ(codeFromWindowsScanCode(0x147), Code::Home);
    EXPECT_EQ(codeFromWindowsScanCode(0x056), Code::IntlBackslash);
    EXPECT_EQ(codeFromWindowsScanCode(0x000), Code::Unknown);
    EXPECT_EQ(codeFromWindowsScanCode(0x1FF), Code::Unknown);
    EXPECT_EQ(codeFromWindowsScanCode(0x200), Code::Unknown);
    EXPECT_EQ(codeFromWindowsScanCode(0xFFFF), Code::Unknown);
    EXPECT_EQ(codeFromEvdev(0), Code::Unknown);
    EXPECT_EQ(codeFromEvdev(30), Code::KeyA);
    EXPECT_EQ(codeFromEvdev(86), Code::IntlBackslash);
    EXPECT_EQ(codeFromEvdev(255), Code::Unknown);
    EXPECT_EQ(codeFromEvdev(0xFFFF), Code::Unknown);
}

TEST(StrokeSyntax, UsCharactersAndNames) {
    EXPECT_EQ(usBaseChar(Code::KeyA), 'a');
    EXPECT_EQ(usBaseChar(Code::Digit0), '0');
    EXPECT_EQ(usBaseChar(Code::Backquote), '`');
    EXPECT_EQ(usBaseChar(Code::Slash), '/');
    EXPECT_EQ(usBaseChar(Code::IntlBackslash), 0);
    EXPECT_EQ(usBaseChar(Code::Space), 0);
    for (std::size_t k = index(Key::A); k < index(Key::Space); ++k) {
        const auto key = static_cast<Key>(k);
        const char c = usCharForKey(key);
        ASSERT_NE(c, 0) << keyName(key);
        EXPECT_EQ(keyForUsChar(static_cast<unsigned char>(c)), key) << keyName(key);
    }
    EXPECT_EQ(keyForUsChar(U'Q'), Key::Q);
    EXPECT_EQ(keyForUsChar(U'+'), Key::Equal);
    EXPECT_EQ(keyForUsChar(U' '), Key::None);
    EXPECT_EQ(keyForUsChar(U'ü'), Key::None);
    EXPECT_EQ(usCharForKey(Key::Space), ' ');
    EXPECT_EQ(usCharForKey(Key::F1), 0);
    EXPECT_EQ(codeName(Code::ArrowLeft), "ArrowLeft");
    EXPECT_EQ(keyName(Key::Left), "Left");
    EXPECT_EQ(kWritingKeyCount, 50U);
}

TEST(StrokeSyntax, SimpleUpperAndUtf8) {
    EXPECT_EQ(simpleUpper(U'a'), U'A');
    EXPECT_EQ(simpleUpper(U'ü'), U'Ü');
    EXPECT_EQ(simpleUpper(U'÷'), U'÷');
    EXPECT_EQ(simpleUpper(U'ł'), U'Ł');
    EXPECT_EQ(simpleUpper(U'ß'), U'ß');
    EXPECT_EQ(simpleUpper(U'1'), U'1');
    std::string out;
    appendUtf8(out, U'A');
    appendUtf8(out, U'Ü');
    appendUtf8(out, U'€');
    appendUtf8(out, U'\U0001F600');
    appendUtf8(out, static_cast<char32_t>(0xD800));
    EXPECT_EQ(out, "A\xC3\x9C\xE2\x82\xAC\xF0\x9F\x98\x80\xEF\xBF\xBD");
}

TEST(StrokeSyntax, SequenceHashAgreesWithEquality) {
    const std::hash<Sequence> hash;
    const Sequence a = Sequence::of(stroke(Modifier::Ctrl, Key::K), stroke(Modifier::Ctrl, Key::C));
    const Sequence b = *parseSequence("Ctrl+K, Ctrl+C", SyntaxMode::Strict);
    EXPECT_EQ(a, b);
    EXPECT_EQ(hash(a), hash(b));
    EXPECT_NE(Sequence::of(stroke(Modifier::Ctrl, Key::K)), a);
    EXPECT_NE(Sequence::of(Gesture::DoubleShift), Sequence::of(Gesture::DoubleCtrl));
}

// Regression: the key list's X-macro parameter once shared the name X with Key::X, which
// renamed that enumerator and its name; every letter key is named by its letter.
TEST(StrokeSyntax, LetterKeysAreNamedByTheirLetter) {
    for (std::size_t i = 0; i < 26; ++i) {
        const auto key = static_cast<Key>(index(Key::A) + i);
        const std::string expected(1, static_cast<char>('A' + i));
        EXPECT_EQ(keyName(key), expected);
        EXPECT_EQ(codeName(static_cast<Code>(index(Code::KeyA) + i)), "Key" + expected);
    }
    EXPECT_EQ(keyName(Key::X), "X");
    const auto parsed = parseSequence("Ctrl+X", SyntaxMode::Strict);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, Sequence::of(Stroke{Modifier::Ctrl, Key::X}));
}

} // namespace
} // namespace frost::input
