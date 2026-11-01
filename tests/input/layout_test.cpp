// SPDX-License-Identifier: Apache-2.0
//
// SimulatedLayouts and LayoutMapping: the simulated layout tables (validated against the
// Microsoft layout charts when they were transcribed; the per-layout fact checks below
// repeat a few cells), LayoutTable::make validation, the layout-aware Code-to-Key policy
// of ADR 0016 decision 3, labels and reachability, LettersByLabel mode, and the reader
// logic (buildLayoutTable) over a scripted ILayoutQuery.

#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include "input/support/golden.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <array>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace frost::input {
namespace {

constexpr std::array kLayouts = {SimulatedLayoutId::EnUs, SimulatedLayoutId::DeDe, SimulatedLayoutId::PlPl,
                                 SimulatedLayoutId::FrFr};

std::string charText(char32_t c, bool dead) {
    if (c == 0) {
        return "-";
    }
    std::string out;
    appendUtf8(out, c);
    if (dead) {
        out += "(dead)";
    }
    return out;
}

// A text dump of a table: per writing code its levels and logical key, then per typing
// key its preferred code and label. The goldens compare these dumps.
std::string describe(const LayoutTable& table) {
    std::string out =
        std::format("# {} ({}). Consistency golden: levels from the simulated tables, logical keys "
                    "from the ADR 0016 policy.\ncodes\n",
                    table.id(), table.mappingMode() == MappingMode::FullTable ? "FullTable" : "LettersByLabel");
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
        const auto code = static_cast<Code>(i);
        const KeyLevels l = table.levels(code);
        out +=
            std::format("{:<14}{:<10}{:<10}{:<10}{:<10}-> {}\n", codeName(code), charText(l.base, l.dead(Level::Base)),
                        charText(l.shift, l.dead(Level::Shift)), charText(l.altGr, l.dead(Level::AltGr)),
                        charText(l.altGrShift, l.dead(Level::AltGrShift)), keyName(table.logicalKey(code)));
    }
    out += "keys\n";
    for (std::size_t k = index(Key::A); k <= index(Key::Space); ++k) {
        const auto key = static_cast<Key>(k);
        if (!table.reachable(key)) {
            out += std::format("{:<14}unreachable\n", keyName(key));
            continue;
        }
        out += std::format("{:<14}code={} label={}\n", keyName(key), codeName(table.codeFor(key)),
                           charText(table.labelFor(key), false));
    }
    return out;
}

std::string goldenName(SimulatedLayoutId id) { return std::format("layout-mapping-{}.txt", simulatedLayoutName(id)); }

TEST(SimulatedLayouts, MakeValidates) {
    const std::array good = {std::pair{Code::KeyA, KeyLevels{U'a', U'A', 0, 0, 0, 0}}};
    EXPECT_TRUE(LayoutTable::make("xx-XX:00000000", MappingMode::FullTable, good, {}).has_value());

    const auto expectInvalid = [](std::string_view id, std::span<const std::pair<Code, KeyLevels>> keys,
                                  std::span<const Composition> compositions, std::string_view what) {
        auto table = LayoutTable::make(id, MappingMode::FullTable, keys, compositions);
        ASSERT_FALSE(table.has_value()) << what;
        EXPECT_EQ(table.error().code(), ErrorCode::InvalidArgument) << what;
    };
    expectInvalid("an-id-that-is-24-bytes!!", good, {}, "id longer than 23 bytes");
    EXPECT_TRUE(LayoutTable::make("an-id-that-is-23-bytes!", MappingMode::FullTable, good, {}).has_value());
    const std::array notWriting = {std::pair{Code::F1, KeyLevels{U'a', 0, 0, 0, 0, 0}}};
    expectInvalid("x", notWriting, {}, "non-writing code");
    const std::array duplicate = {std::pair{Code::KeyA, KeyLevels{U'a', 0, 0, 0, 0, 0}},
                                  std::pair{Code::KeyA, KeyLevels{U'b', 0, 0, 0, 0, 0}}};
    expectInvalid("x", duplicate, {}, "duplicate code");
    const std::array surrogate = {std::pair{Code::KeyA, KeyLevels{0xD800, 0, 0, 0, 0, 0}}};
    expectInvalid("x", surrogate, {}, "surrogate");
    const std::array tooLarge = {std::pair{Code::KeyA, KeyLevels{0x110000, 0, 0, 0, 0, 0}}};
    expectInvalid("x", tooLarge, {}, "above U+10FFFF");
    const std::array ligatureOnZero = {std::pair{Code::KeyA, KeyLevels{U'a', 0, 0, 0, 0, kDeadShift}}};
    expectInvalid("x", ligatureOnZero, {}, "ligature bit on a zero character");
    const std::array deadOnZero = {std::pair{Code::KeyA, KeyLevels{U'a', 0, 0, 0, kDeadAltGr, 0}}};
    expectInvalid("x", deadOnZero, {}, "dead bit on a zero character");
    const std::array highMask = {std::pair{Code::KeyA, KeyLevels{U'a', U'A', U'b', U'B', 0x10, 0}}};
    expectInvalid("x", highMask, {}, "mask bit above the four levels");
    const std::array unsorted = {Composition{U'^', U'e', U'ê'}, Composition{U'^', U'a', U'â'}};
    expectInvalid("x", good, unsorted, "compositions out of order");
    const std::array repeated = {Composition{U'^', U'a', U'â'}, Composition{U'^', U'a', U'â'}};
    expectInvalid("x", good, repeated, "repeated composition");
    const std::array zero = {Composition{U'^', 0, U'â'}};
    expectInvalid("x", good, zero, "zero composition base");
}

TEST(SimulatedLayouts, EveryPrintableAsciiCharacterIsProducible) {
    for (const SimulatedLayoutId id : kLayouts) {
        const LayoutTable& table = simulatedLayout(id);
        std::array<bool, 128> produced{};
        produced[U' '] = true; // The Space key.
        for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
            const KeyLevels l = table.levels(static_cast<Code>(i));
            for (unsigned level = 0; level < 4; ++level) {
                const char32_t c = l.at(static_cast<Level>(level));
                if (c == 0) {
                    continue;
                }
                if (l.dead(static_cast<Level>(level))) {
                    const auto spacing = table.compose(c, U' ');
                    ASSERT_TRUE(spacing.has_value()) << simulatedLayoutName(id) << ": dead key without Space";
                    if (*spacing < 128) {
                        produced[*spacing] = true;
                    }
                } else if (c < 128) {
                    produced[c] = true;
                }
            }
        }
        for (char32_t c = 0x20; c <= 0x7E; ++c) {
            EXPECT_TRUE(produced[c]) << simulatedLayoutName(id) << " cannot type '" << static_cast<char>(c) << "'";
        }
    }
}

TEST(SimulatedLayouts, LatinLettersAtBase) {
    for (const SimulatedLayoutId id : kLayouts) {
        const LayoutTable& table = simulatedLayout(id);
        std::array<bool, 26> letters{};
        for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
            const char32_t c = table.levels(static_cast<Code>(i)).base;
            if (c >= U'a' && c <= U'z') {
                letters[c - U'a'] = true;
            }
        }
        for (std::size_t l = 0; l < 26; ++l) {
            EXPECT_TRUE(letters[l]) << simulatedLayoutName(id) << " lacks '" << static_cast<char>('a' + l) << "'";
        }
    }
}

TEST(SimulatedLayouts, CompositionsSortedUniqueAndComposable) {
    for (const SimulatedLayoutId id : kLayouts) {
        const LayoutTable& table = simulatedLayout(id);
        const auto compositions = table.compositions();
        for (std::size_t i = 1; i < compositions.size(); ++i) {
            const Composition& a = compositions[i - 1];
            const Composition& b = compositions[i];
            EXPECT_TRUE(a.dead < b.dead || (a.dead == b.dead && a.base < b.base))
                << simulatedLayoutName(id) << " " << i;
        }
        for (const Composition& c : compositions) {
            EXPECT_EQ(table.compose(c.dead, c.base), c.composed);
        }
        EXPECT_FALSE(table.compose(U'^', U'x').has_value());
        EXPECT_FALSE(table.compose(0, 0).has_value());
    }
    EXPECT_TRUE(simulatedLayout(SimulatedLayoutId::EnUs).compositions().empty());
    EXPECT_EQ(simulatedLayout(SimulatedLayoutId::DeDe).compose(U'^', U'e'), U'ê');
    EXPECT_EQ(simulatedLayout(SimulatedLayoutId::FrFr).compose(U'~', U'n'), U'ñ');
    EXPECT_EQ(simulatedLayout(SimulatedLayoutId::PlPl).compose(U'~', U'a'), U'ą');
}

TEST(SimulatedLayouts, ChartFacts) {
    const LayoutTable& de = simulatedLayout(SimulatedLayoutId::DeDe);
    EXPECT_EQ(de.levels(Code::Digit7).shift, U'/');
    EXPECT_EQ(de.levels(Code::KeyQ).altGr, U'@');
    EXPECT_EQ(de.levels(Code::KeyY).base, U'z');
    EXPECT_TRUE(de.levels(Code::Backquote).dead(Level::Base));
    EXPECT_TRUE(de.levels(Code::Equal).dead(Level::Shift));
    EXPECT_EQ(de.levels(Code::KeyL).altGr, 0U) << "Windows German has no AltGr+L";
    EXPECT_TRUE(de.hasAltGr());

    const LayoutTable& fr = simulatedLayout(SimulatedLayoutId::FrFr);
    EXPECT_EQ(fr.levels(Code::Digit1).base, U'&');
    EXPECT_EQ(fr.levels(Code::Digit1).shift, U'1');
    EXPECT_EQ(fr.levels(Code::Digit0).altGr, U'@');
    EXPECT_TRUE(fr.levels(Code::Digit2).dead(Level::AltGr));
    EXPECT_TRUE(fr.levels(Code::BracketLeft).dead(Level::Base));

    const LayoutTable& pl = simulatedLayout(SimulatedLayoutId::PlPl);
    EXPECT_EQ(pl.levels(Code::KeyL).altGr, U'ł');
    EXPECT_EQ(pl.levels(Code::KeyL).altGrShift, U'Ł');
    EXPECT_EQ(pl.levels(Code::KeyU).altGr, U'€');
    EXPECT_TRUE(pl.levels(Code::Backquote).dead(Level::Shift));
    EXPECT_FALSE(pl.levels(Code::Backquote).dead(Level::Base));

    const LayoutTable& us = simulatedLayout(SimulatedLayoutId::EnUs);
    EXPECT_EQ(us.levels(Code::IntlBackslash).base, U'\\');
    EXPECT_EQ(us.levels(Code::IntlBackslash).shift, U'|');
    EXPECT_FALSE(us.hasAltGr());
    EXPECT_EQ(us.id(), "en-US:00000409");
    EXPECT_EQ(linuxM1Layout().mappingMode(), MappingMode::LettersByLabel);
}

TEST(LayoutMapping, Goldens) {
    for (const SimulatedLayoutId id : kLayouts) {
        SCOPED_TRACE(simulatedLayoutName(id));
        test::expectGolden(goldenName(id), describe(simulatedLayout(id)));
    }
}

TEST(LayoutMapping, LinuxM1GoldenWithHintsFromEachTruth) {
    std::string out = "# linuxM1Layout() (LettersByLabel) with the keyCharHint each truth table's base letter "
                      "gives. Consistency golden.\n";
    for (const SimulatedLayoutId id : kLayouts) {
        const LayoutTable& truth = simulatedLayout(id);
        out += std::format("truth {}\n", simulatedLayoutName(id));
        for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlYen); ++i) {
            const auto code = static_cast<Code>(i);
            const char32_t base = truth.levels(code).base;
            const char32_t hint = base >= U'a' && base <= U'z' && !truth.levels(code).dead(Level::Base) ? base : 0;
            out += std::format("  {:<14}-> {}\n", codeName(code), keyName(linuxM1Layout().logicalKey(code, hint)));
        }
    }
    test::expectGolden("layout-mapping-linux-m1.txt", out);
}

TEST(LayoutMapping, EnUsIsTheIdentity) {
    const LayoutTable& us = simulatedLayout(SimulatedLayoutId::EnUs);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::Slash); ++i) {
        const auto code = static_cast<Code>(i);
        EXPECT_EQ(us.logicalKey(code), keyForUsChar(static_cast<unsigned char>(usBaseChar(code)))) << codeName(code);
    }
    EXPECT_EQ(us.logicalKey(Code::IntlBackslash), Key::Backslash);
    EXPECT_EQ(us.logicalKey(Code::IntlRo), Key::None);
    EXPECT_EQ(us.logicalKey(Code::IntlYen), Key::None);
    for (std::size_t k = index(Key::A); k <= index(Key::Space); ++k) {
        EXPECT_TRUE(us.reachable(static_cast<Key>(k))) << keyName(static_cast<Key>(k));
    }
    // pl-PL (Programmers) is US at base and Shift.
    const LayoutTable& pl = simulatedLayout(SimulatedLayoutId::PlPl);
    for (std::size_t i = 0; i < kCodeCount; ++i) {
        EXPECT_EQ(pl.logicalKey(static_cast<Code>(i)), us.logicalKey(static_cast<Code>(i))) << i;
    }
}

TEST(LayoutMapping, NonWritingKeysMapToThemselves) {
    const LayoutTable& de = simulatedLayout(SimulatedLayoutId::DeDe);
    EXPECT_EQ(de.logicalKey(Code::Space), Key::Space);
    EXPECT_EQ(de.logicalKey(Code::ArrowLeft), Key::Left);
    EXPECT_EQ(de.logicalKey(Code::ArrowDown), Key::Down);
    EXPECT_EQ(de.logicalKey(Code::F1), Key::F1);
    EXPECT_EQ(de.logicalKey(Code::F24), Key::F24);
    EXPECT_EQ(de.logicalKey(Code::Numpad5), Key::Numpad5);
    EXPECT_EQ(de.logicalKey(Code::NumpadDivide), Key::NumpadDivide);
    EXPECT_EQ(de.logicalKey(Code::NumpadEnter), Key::Enter);
    EXPECT_EQ(de.logicalKey(Code::PrintScreen), Key::PrintScreen);
    EXPECT_EQ(de.logicalKey(Code::ContextMenu), Key::ContextMenu);
    for (const Code c : {Code::Unknown, Code::CapsLock, Code::NumLock, Code::ScrollLock, Code::ShiftLeft,
                         Code::ControlRight, Code::AltRight, Code::MetaLeft}) {
        EXPECT_EQ(de.logicalKey(c), Key::None) << codeName(c);
    }
    EXPECT_EQ(de.logicalKey(static_cast<Code>(250)), Key::None);
}

TEST(LayoutMapping, GermanFacts) {
    const LayoutTable& de = simulatedLayout(SimulatedLayoutId::DeDe);
    EXPECT_EQ(de.logicalKey(Code::KeyY), Key::Z);
    EXPECT_EQ(de.logicalKey(Code::KeyZ), Key::Y);
    EXPECT_EQ(de.logicalKey(Code::Comma), Key::Comma);
    EXPECT_EQ(de.logicalKey(Code::Period), Key::Period);
    EXPECT_EQ(de.logicalKey(Code::Slash), Key::Minus);
    EXPECT_EQ(de.logicalKey(Code::Minus), Key::Minus);
    EXPECT_EQ(de.logicalKey(Code::Equal), Key::Equal);
    EXPECT_EQ(de.logicalKey(Code::Backquote), Key::Backquote);
    EXPECT_EQ(de.logicalKey(Code::BracketLeft), Key::BracketLeft);
    EXPECT_EQ(de.logicalKey(Code::BracketRight), Key::BracketRight);
    EXPECT_EQ(de.logicalKey(Code::Backslash), Key::Backslash);
    EXPECT_EQ(de.logicalKey(Code::Semicolon), Key::Semicolon);
    EXPECT_EQ(de.logicalKey(Code::Quote), Key::Quote);
    EXPECT_EQ(de.logicalKey(Code::IntlBackslash), Key::None);
    for (std::size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(de.logicalKey(static_cast<Code>(index(Code::Digit0) + i)), static_cast<Key>(index(Key::Digit0) + i));
    }
    EXPECT_FALSE(de.reachable(Key::Slash));
    EXPECT_EQ(de.labelFor(Key::Minus), U'-');
    EXPECT_EQ(de.codeFor(Key::Minus), Code::Slash);
    EXPECT_EQ(de.codeFor(Key::Z), Code::KeyY);
    EXPECT_EQ(de.labelFor(Key::BracketLeft), U'ü');
    EXPECT_EQ(de.labelFor(Key::Digit7), U'7');
    EXPECT_EQ(de.labelFor(Key::Slash), 0U);
    EXPECT_EQ(de.codeFor(Key::Slash), Code::Unknown);
    EXPECT_FALSE(de.reachable(Key::None));
}

TEST(LayoutMapping, FrenchFacts) {
    const LayoutTable& fr = simulatedLayout(SimulatedLayoutId::FrFr);
    EXPECT_EQ(fr.logicalKey(Code::KeyQ), Key::A);
    EXPECT_EQ(fr.logicalKey(Code::KeyA), Key::Q);
    EXPECT_EQ(fr.logicalKey(Code::KeyW), Key::Z);
    EXPECT_EQ(fr.logicalKey(Code::KeyZ), Key::W);
    EXPECT_EQ(fr.logicalKey(Code::Semicolon), Key::M);
    EXPECT_EQ(fr.logicalKey(Code::KeyM), Key::Comma);
    EXPECT_EQ(fr.logicalKey(Code::Comma), Key::Semicolon);
    EXPECT_EQ(fr.logicalKey(Code::Equal), Key::Equal);
    EXPECT_EQ(fr.logicalKey(Code::Period), Key::Period);
    EXPECT_EQ(fr.logicalKey(Code::Slash), Key::Slash);
    EXPECT_EQ(fr.logicalKey(Code::Minus), Key::Minus);
    EXPECT_EQ(fr.logicalKey(Code::BracketLeft), Key::BracketLeft);
    EXPECT_EQ(fr.logicalKey(Code::BracketRight), Key::BracketRight);
    EXPECT_EQ(fr.logicalKey(Code::Backslash), Key::Backslash);
    EXPECT_EQ(fr.logicalKey(Code::Quote), Key::Quote);
    EXPECT_EQ(fr.logicalKey(Code::Backquote), Key::Backquote);
    EXPECT_EQ(fr.logicalKey(Code::Digit1), Key::Digit1);
    EXPECT_EQ(fr.logicalKey(Code::IntlBackslash), Key::None);
    for (std::size_t k = index(Key::A); k <= index(Key::Space); ++k) {
        EXPECT_TRUE(fr.reachable(static_cast<Key>(k))) << keyName(static_cast<Key>(k));
    }
    EXPECT_EQ(fr.labelFor(Key::Comma), U',');
    EXPECT_EQ(fr.codeFor(Key::Comma), Code::KeyM);
    EXPECT_EQ(fr.labelFor(Key::Digit1), U'&');
}

// A US table with some cells replaced: the synthetic layouts of the policy tests.
LayoutTable usWith(std::vector<std::pair<Code, KeyLevels>> changes, std::string_view id = "test") {
    std::map<Code, KeyLevels> cells;
    const LayoutTable& us = simulatedLayout(SimulatedLayoutId::EnUs);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::IntlBackslash); ++i) {
        cells[static_cast<Code>(i)] = us.levels(static_cast<Code>(i));
    }
    for (const auto& [code, levels] : changes) {
        cells[code] = levels;
    }
    std::vector<std::pair<Code, KeyLevels>> keys(cells.begin(), cells.end());
    auto table = LayoutTable::make(id, MappingMode::FullTable, keys, {});
    EXPECT_TRUE(table.has_value());
    return table ? *table : us;
}

TEST(LayoutMapping, KeyProducingNothingIsNone) {
    const LayoutTable table = usWith({{Code::KeyQ, KeyLevels{}}});
    EXPECT_EQ(table.logicalKey(Code::KeyQ), Key::None);
    EXPECT_FALSE(table.reachable(Key::Q));
}

TEST(LayoutMapping, LetterAreaKeyWithNonLatinCharacter) {
    // Turkish-F-like: KeyW types 'ğ' and no key types 'w', so KeyW keeps its US letter.
    const LayoutTable alone = usWith({{Code::KeyW, KeyLevels{U'ğ', U'Ğ', 0, 0, 0, 0}}});
    EXPECT_EQ(alone.logicalKey(Code::KeyW), Key::W);
    // When another key types 'w', the letter follows that label and KeyW maps to nothing.
    const LayoutTable owned =
        usWith({{Code::KeyW, KeyLevels{U'ğ', U'Ğ', 0, 0, 0, 0}}, {Code::KeyQ, KeyLevels{U'w', U'W', 0, 0, 0, 0}}});
    EXPECT_EQ(owned.logicalKey(Code::KeyQ), Key::W);
    EXPECT_EQ(owned.logicalKey(Code::KeyW), Key::None);
    EXPECT_FALSE(owned.reachable(Key::Q));
    // Punctuation positions with other characters stay positional; Intl keys are None.
    const LayoutTable punct = usWith({{Code::Semicolon, KeyLevels{U'ş', U'Ş', 0, 0, 0, 0}},
                                      {Code::IntlBackslash, KeyLevels{U'<', U'>', 0, 0, 0, 0}}});
    EXPECT_EQ(punct.logicalKey(Code::Semicolon), Key::Semicolon);
    EXPECT_EQ(punct.logicalKey(Code::IntlBackslash), Key::None);
}

TEST(LayoutMapping, NonLatinLayoutIsPositional) {
    // Russian-like: Cyrillic on every letter-area key, so every writing key is positional.
    std::vector<std::pair<Code, KeyLevels>> changes;
    for (std::size_t i = index(Code::KeyA); i <= index(Code::KeyZ); ++i) {
        const auto c = static_cast<char32_t>(0x0430 + (i - index(Code::KeyA)));
        changes.emplace_back(static_cast<Code>(i), KeyLevels{c, static_cast<char32_t>(c - 0x20), 0, 0, 0, 0});
    }
    changes.emplace_back(Code::Comma, KeyLevels{U'б', U'Б', 0, 0, 0, 0});
    changes.emplace_back(Code::Slash, KeyLevels{U'.', U',', 0, 0, 0, 0});
    const LayoutTable table = usWith(changes);
    for (std::size_t i = index(Code::KeyA); i <= index(Code::Slash); ++i) {
        const auto code = static_cast<Code>(i);
        EXPECT_EQ(table.logicalKey(code), keyForUsChar(static_cast<unsigned char>(usBaseChar(code)))) << codeName(code);
    }
    EXPECT_EQ(table.logicalKey(Code::IntlBackslash), Key::None);
}

TEST(LayoutMapping, LettersByLabel) {
    const LayoutTable& m1 = linuxM1Layout();
    EXPECT_EQ(m1.logicalKey(Code::KeyY, U'z'), Key::Z);
    EXPECT_EQ(m1.logicalKey(Code::KeyY, U'Z'), Key::Z);
    EXPECT_EQ(m1.logicalKey(Code::KeyY), Key::Y);
    EXPECT_EQ(m1.logicalKey(Code::Semicolon, U'm'), Key::M);
    EXPECT_EQ(m1.logicalKey(Code::Slash, U'-'), Key::Slash);
    EXPECT_EQ(m1.logicalKey(Code::Slash), Key::Slash);
    EXPECT_EQ(m1.logicalKey(Code::KeyM, U'é'), Key::M);
    EXPECT_EQ(m1.logicalKey(Code::IntlBackslash, U'<'), Key::None);
    EXPECT_EQ(m1.logicalKey(Code::F5, U'a'), Key::F5) << "hints apply to writing keys only";
    EXPECT_EQ(m1.logicalKey(Code::Digit1, U'a'), Key::A) << "a hint letter wins on any writing key";
}

// A scripted query answering from a LayoutTable, with optional overrides per code.
class TableQuery final : public ILayoutQuery {
public:
    explicit TableQuery(const LayoutTable& table) : m_table(table) {}

    std::uint8_t vkFromScan(std::uint16_t scan) const override {
        const Code code = codeFromWindowsScanCode(scan);
        return code == m_noVk ? 0 : static_cast<std::uint8_t>(index(code));
    }
    int toUnicode(std::uint8_t vk, std::uint16_t scan, Level level, std::span<char32_t, 4> out) const override {
        const auto code = static_cast<Code>(vk);
        EXPECT_EQ(codeFromWindowsScanCode(scan), code);
        if (code == m_ligature && level == Level::Base) {
            out[0] = U'f';
            out[1] = U'f';
            out[2] = U'i';
            return 3;
        }
        if (code == m_supplementary && level == Level::Base) {
            out[0] = U'\U0001F600';
            return 1;
        }
        if (code == m_control && level == Level::Base) {
            out[0] = 0x1B;
            return 1;
        }
        const KeyLevels l = m_table.levels(code);
        const char32_t c = l.at(level);
        if (c == 0) {
            return 0;
        }
        out[0] = c;
        return l.dead(level) ? -1 : 1;
    }
    std::string_view localeName() const override { return m_locale; }
    std::string_view klid() const override { return "00000407"; }

    Code m_noVk = Code::Unknown;
    Code m_ligature = Code::Unknown;
    Code m_supplementary = Code::Unknown;
    Code m_control = Code::Unknown;
    std::string m_locale = "de-DE";

private:
    const LayoutTable& m_table;
};

TEST(LayoutMapping, ReaderReproducesEachSimulatedTable) {
    for (const SimulatedLayoutId id : kLayouts) {
        SCOPED_TRACE(simulatedLayoutName(id));
        const LayoutTable& truth = simulatedLayout(id);
        auto built = buildLayoutTable(TableQuery(truth));
        ASSERT_TRUE(built.has_value()) << built.error().message();
        EXPECT_EQ(built->id(), "de-DE:00000407");
        EXPECT_EQ(built->mappingMode(), MappingMode::FullTable);
        EXPECT_TRUE(built->compositions().empty()) << "the OS composes";
        for (std::size_t i = 0; i < kCodeCount; ++i) {
            const auto code = static_cast<Code>(i);
            EXPECT_EQ(built->levels(code), truth.levels(code)) << codeName(code);
            EXPECT_EQ(built->logicalKey(code), truth.logicalKey(code)) << codeName(code);
        }
        for (std::size_t k = 0; k < kKeyCount; ++k) {
            EXPECT_EQ(built->labelFor(static_cast<Key>(k)), truth.labelFor(static_cast<Key>(k)));
        }
        EXPECT_EQ(built->hasAltGr(), truth.hasAltGr());
    }
}

TEST(LayoutMapping, ReaderFaults) {
    TableQuery query(simulatedLayout(SimulatedLayoutId::DeDe));
    query.m_noVk = Code::KeyQ;
    query.m_ligature = Code::KeyW;
    query.m_supplementary = Code::KeyE;
    query.m_control = Code::KeyR;
    auto built = buildLayoutTable(query);
    ASSERT_TRUE(built.has_value());
    EXPECT_EQ(built->levels(Code::KeyQ), KeyLevels{}) << "no virtual key: left empty";
    EXPECT_EQ(built->levels(Code::KeyW).base, U'f');
    EXPECT_EQ(built->levels(Code::KeyW).ligatureMask, kDeadBase) << "first code point stored, ligature bit set";
    EXPECT_EQ(built->levels(Code::KeyE).base, U'\U0001F600');
    EXPECT_EQ(built->levels(Code::KeyR).base, 0U) << "control characters count as nothing";
    EXPECT_EQ(built->levels(Code::KeyR).shift, U'R');
    EXPECT_TRUE(built->levels(Code::Backquote).dead(Level::Base));
    EXPECT_EQ(built->levels(Code::Backquote).base, U'^');

    query.m_locale = "a-very-long-locale-name-here";
    auto longId = buildLayoutTable(query);
    ASSERT_TRUE(longId.has_value());
    EXPECT_EQ(longId->id(), "a-very-long-lo:00000407");
    EXPECT_EQ(longId->id().size(), 23U);
}

TEST(LayoutMapping, TablesAreTriviallyCopyableValues) {
    static_assert(std::is_trivially_copyable_v<LayoutTable>);
    const LayoutTable copy = simulatedLayout(SimulatedLayoutId::FrFr);
    EXPECT_EQ(copy, simulatedLayout(SimulatedLayoutId::FrFr));
    EXPECT_NE(copy, simulatedLayout(SimulatedLayoutId::DeDe));
}

} // namespace
} // namespace frost::input
