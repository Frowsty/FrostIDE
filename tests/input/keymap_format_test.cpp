// SPDX-License-Identifier: Apache-2.0
//
// KeymapFormat: the keymap format v1 parser and formatter (keymap.hpp): a golden of the
// canonical form, every error with its line and column, the accepted variations, section
// resolution, the size caps (input, canonical form, sequences per line) and the
// format/parse round-trip property.

#include "frost/input/keymap.hpp"

#include "input/support/golden.hpp"
#include "input/support/test_actions.hpp"
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace frost::input {
namespace {

constexpr std::string_view kSample = "# A hand-written keymap.\n"
                                     "\n"
                                     "frost-keymap 1\n"
                                     "  # indented comment\n"
                                     "id sample-1\n"
                                     "name Sample keymap\n"
                                     "[default]\n"
                                     "editor.copy = Ctrl+C | Ctrl+Insert\n"
                                     "editor.paste=Ctrl+V\n"
                                     "vcs.commit\t=\tCtrl+K\n"
                                     "editor.commentBlock = Ctrl+K, Ctrl+C|Ctrl+Shift+Slash   \n"
                                     "search.everywhere = DoubleShift\n"
                                     "agent.terminal =\n"
                                     "[macos]\n"
                                     "editor.copy = Meta+C\n"
                                     "tools.macOnly = Meta+F1\n"
                                     "[linux]\n"
                                     "vcs.commit = Ctrl+Alt+K\n";

Keymap parsed(std::string_view text) {
    auto keymap = parseKeymap(text);
    EXPECT_TRUE(keymap.has_value()) << keymap.error().message();
    return keymap ? std::move(*keymap) : Keymap{};
}

std::string bindingsText(const std::vector<Binding>& bindings) {
    std::string out;
    for (const Binding& b : bindings) {
        out += b.actionId + " =";
        for (std::size_t i = 0; i < b.sequences.size(); ++i) {
            out += (i == 0 ? " " : " | ") + formatSequence(b.sequences[i]);
        }
        out += '\n';
    }
    return out;
}

TEST(KeymapFormat, CanonicalGolden) {
    const Keymap keymap = parsed(kSample);
    EXPECT_EQ(keymap.id(), "sample-1");
    EXPECT_EQ(keymap.name(), "Sample keymap");
    EXPECT_TRUE(keymap.hasSection(KeymapSection::Default));
    EXPECT_FALSE(keymap.hasSection(KeymapSection::Windows));
    EXPECT_TRUE(keymap.hasSection(KeymapSection::MacOS));
    test::expectGolden("keymap-format-sample.txt", formatKeymap(keymap));
}

TEST(KeymapFormat, Resolution) {
    const Keymap keymap = parsed(kSample);
    EXPECT_EQ(bindingsText(keymap.bindings(KeymapPlatform::Windows)),
              "editor.copy = Ctrl+C | Ctrl+Insert\n"
              "editor.paste = Ctrl+V\n"
              "vcs.commit = Ctrl+K\n"
              "editor.commentBlock = Ctrl+K, Ctrl+C | Ctrl+Shift+Slash\n"
              "search.everywhere = DoubleShift\n"
              "agent.terminal =\n");
    EXPECT_EQ(bindingsText(keymap.bindings(KeymapPlatform::Linux)),
              "editor.copy = Ctrl+C | Ctrl+Insert\n"
              "editor.paste = Ctrl+V\n"
              "vcs.commit = Ctrl+Alt+K\n"
              "editor.commentBlock = Ctrl+K, Ctrl+C | Ctrl+Shift+Slash\n"
              "search.everywhere = DoubleShift\n"
              "agent.terminal =\n");
    // macOS: listed lines win, the rest swap Ctrl and Meta, gestures stay, and
    // macOS-only actions follow in file order.
    EXPECT_EQ(bindingsText(keymap.bindings(KeymapPlatform::MacOS)),
              "editor.copy = Meta+C\n"
              "editor.paste = Meta+V\n"
              "vcs.commit = Meta+K\n"
              "editor.commentBlock = Meta+K, Meta+C | Shift+Meta+Slash\n"
              "search.everywhere = DoubleShift\n"
              "agent.terminal =\n"
              "tools.macOnly = Meta+F1\n");
}

TEST(KeymapFormat, AcceptedVariations) {
    const std::string lf = "frost-keymap 1\nid a\nname A\n[default]\neditor.copy = Ctrl+C | Ctrl+Insert\n";
    const Keymap reference = parsed(lf);
    std::string crlf;
    for (const char c : lf) {
        if (c == '\n') {
            crlf += '\r';
        }
        crlf += c;
    }
    EXPECT_EQ(parsed(crlf), reference);
    EXPECT_EQ(parsed("\xEF\xBB\xBF" + lf), reference);
    EXPECT_EQ(parsed("frost-keymap 1\nid a\nname A\n[default]\neditor.copy\t =  Ctrl+C\t|\tCtrl+Insert \t"), reference);
    EXPECT_EQ(parsed("frost-keymap 1  \nid a\nname A\n\n\n[default]\n# c\n   \neditor.copy = Ctrl+C |Ctrl+Insert\n"),
              reference);
    EXPECT_EQ(parsed("frost-keymap 1\nid a\nname A\n").section(KeymapSection::Default).size(), 0U);
    EXPECT_EQ(parsed("frost-keymap 1\nname A\nid a\n").id(), "a") << "directives in any order";
    EXPECT_EQ(parsed("frost-keymap 1\nid a\nname A title with spaces\n").name(), "A title with spaces");
}

struct FormatError {
    std::string_view what;
    std::string text;
    ErrorCode code;
    std::string_view message; // A prefix of the error message.
};

TEST(KeymapFormat, Errors) {
    const std::string head = "frost-keymap 1\nid a\nname A\n";
    const FormatError cases[] = {
        {"missing header", "id a\nname A\n", ErrorCode::Corrupt, "keymap line 1, column 1: expected the header"},
        {"empty input", "", ErrorCode::Corrupt, "keymap line 1, column 1: missing the header"},
        {"only comments", "# x\n\n", ErrorCode::Corrupt, "keymap line 3, column 1: missing the header"},
        {"duplicate header", head + "frost-keymap 1\n", ErrorCode::Corrupt,
         "keymap line 4, column 1: duplicate header"},
        {"version 2", "frost-keymap 2\n", ErrorCode::Unsupported, "keymap line 1: unsupported format version '2'"},
        {"bad version text", "frost-keymap one\n", ErrorCode::Corrupt, "keymap line 1, column 1: expected the header"},
        {"bad id", "frost-keymap 1\nid Bad_Id\nname A\n", ErrorCode::Corrupt,
         "keymap line 2, column 4: keymap ids match"},
        {"duplicate id", "frost-keymap 1\nid a\nid b\n", ErrorCode::Corrupt, "keymap line 3, column 1: duplicate id"},
        {"empty name", "frost-keymap 1\nid a\nname \n", ErrorCode::Corrupt,
         "keymap line 3, column 5: a keymap name is 1-128 bytes"},
        {"long name", "frost-keymap 1\nid a\nname " + std::string(129, 'n') + "\n", ErrorCode::Corrupt,
         "keymap line 3, column 6: a keymap name is 1-128 bytes"},
        {"missing id", "frost-keymap 1\nname A\n[default]\n", ErrorCode::Corrupt,
         "keymap line 3, column 1: missing id directive"},
        {"missing name at end", "frost-keymap 1\nid a\n", ErrorCode::Corrupt, "keymap line 3, column 1: missing name"},
        {"duplicate action", head + "[default]\neditor.copy = Ctrl+C\neditor.copy = Ctrl+Insert\n", ErrorCode::Corrupt,
         "keymap line 6, column 1: action 'editor.copy' appears twice"},
        {"duplicate sequence", head + "[default]\neditor.copy = Ctrl+C | Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 24: duplicate sequence 'Ctrl+C'"},
        {"unknown section", head + "[mac]\n", ErrorCode::Corrupt, "keymap line 4, column 2: unknown section 'mac'"},
        {"unclosed section", head + "[default\n", ErrorCode::Corrupt, "keymap line 4, column 8: expected ']'"},
        {"duplicated section", head + "[linux]\n[linux]\n", ErrorCode::Corrupt,
         "keymap line 5, column 1: duplicate section [linux]"},
        {"directive after a section", head + "[default]\nname B\n", ErrorCode::Corrupt,
         "keymap line 5, column 1: directive after a section"},
        {"binding before a section", head + "editor.copy = Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 4, column 1: binding outside a section"},
        {"bad action id", head + "[default]\nEditor.copy = Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 1: invalid action id 'Editor.copy'"},
        {"single-segment action id", head + "[default]\ncopy = Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 1: invalid action id 'copy'"},
        {"missing equals", head + "[default]\neditor.copy Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 13: expected '='"},
        {"bad sequence", head + "[default]\neditor.copy = Ctrl+Foo\n", ErrorCode::Corrupt,
         "keymap line 5, column 15: column 6: unknown key 'Foo'"},
        {"typing key", head + "[default]\neditor.copy = Shift+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 15: column 1: a binding on C"},
        {"empty alternative", head + "[default]\neditor.copy = Ctrl+C | \n", ErrorCode::Corrupt,
         "keymap line 5, column 23: empty sequence"},
        {"indented binding", head + "[default]\n  editor.copy = Ctrl+C\n", ErrorCode::Corrupt,
         "keymap line 5, column 1: unexpected indentation"},
        {"trailing comment", head + "[default]\neditor.copy = Ctrl+C # copy\n", ErrorCode::Corrupt,
         "keymap line 5, column 15: column 7: unexpected character ' '"},
        {"too-long line", head + "[default]\neditor.copy = Ctrl+C" + std::string(4100, ' ') + "\n", ErrorCode::Corrupt,
         "keymap line 5, column 4097: line longer than 4096 bytes"},
        {"invalid UTF-8", head + "name \xC3\x28\n", ErrorCode::Corrupt, "keymap line 4, column 6: invalid UTF-8"},
        {"overlong UTF-8", "frost-keymap 1\nid a\nname \xC0\xAF\n", ErrorCode::Corrupt,
         "keymap line 3, column 6: invalid UTF-8"},
        {"surrogate UTF-8", "frost-keymap 1\nid a\nname \xED\xA0\x80\n", ErrorCode::Corrupt,
         "keymap line 3, column 6: invalid UTF-8"},
        {"NUL byte", head + std::string("[default]\nx", 11) + std::string(1, '\0') + "\n", ErrorCode::Corrupt,
         "keymap line 5, column 2: NUL byte"},
        {"control byte", head + "[default\x01]\n", ErrorCode::Corrupt, "keymap line 4, column 9: control character"},
        {"lone CR", head + "[default]\r\r\n", ErrorCode::Corrupt, "keymap line 4, column 10: control character"},
    };
    for (const FormatError& c : cases) {
        auto keymap = parseKeymap(c.text);
        ASSERT_FALSE(keymap.has_value()) << c.what;
        EXPECT_EQ(keymap.error().code(), c.code) << c.what << ": " << keymap.error().message();
        EXPECT_TRUE(keymap.error().message().starts_with(c.message)) << c.what << ": " << keymap.error().message();
    }
}

TEST(KeymapFormat, SizeCap) {
    std::string text = "frost-keymap 1\nid a\nname A\n[default]\n";
    while (text.size() <= (std::size_t{1} << 20)) {
        text += "# filler filler filler filler filler filler filler filler filler filler filler filler\n";
    }
    auto keymap = parseKeymap(text);
    ASSERT_FALSE(keymap.has_value());
    EXPECT_EQ(keymap.error().code(), ErrorCode::OutOfRange);
    text.resize(std::size_t{1} << 20);
    text.back() = '\n';
    EXPECT_TRUE(parseKeymap(text).has_value()) << "exactly 1 MiB is accepted";
}

// The distinct Strict function-key sequences "<mods>+F<n>", in a fixed order.
std::vector<std::string> functionKeySequences() {
    std::vector<std::string> out;
    for (unsigned bits = 0; bits < 16; ++bits) {
        for (int n = 1; n <= 24; ++n) {
            out.push_back(formatSequence(
                Sequence::of(Stroke{Modifiers::fromBits(static_cast<std::uint8_t>(bits)),
                                    static_cast<Key>(index(Key::F1) + static_cast<std::size_t>(n - 1))})));
        }
    }
    return out;
}

// An action lists at most kMaxSequencesPerBinding sequences, so a compact line that fits
// 4096 bytes cannot format into a line that does not (the canonical form adds spacing).
TEST(KeymapFormat, SequencesPerBindingAreCapped) {
    const std::vector<std::string> sequences = functionKeySequences();
    const auto keymapWith = [&](std::size_t count, std::string_view separator) {
        std::string line = "a.b=";
        for (std::size_t i = 0; i < count; ++i) {
            line += (i == 0 ? "" : std::string(separator)) + sequences[i];
        }
        return "frost-keymap 1\nid x\nname X\n[default]\n" + line + "\n";
    };
    const auto eight = parseKeymap(keymapWith(kMaxSequencesPerBinding, "|"));
    ASSERT_TRUE(eight.has_value()) << eight.error().message();
    EXPECT_EQ(eight->section(KeymapSection::Default)[0].sequences.size(), kMaxSequencesPerBinding);
    const auto nine = parseKeymap(keymapWith(kMaxSequencesPerBinding + 1, " | "));
    ASSERT_FALSE(nine.has_value());
    EXPECT_EQ(nine.error().code(), ErrorCode::Corrupt);
    EXPECT_TRUE(nine.error().message().starts_with("keymap line 5, column ")) << nine.error().message();
    EXPECT_NE(nine.error().message().find("more than 8 sequences"), std::string::npos) << nine.error().message();

    // The review's case: one compact 4094-byte line of 348 sequences.
    std::string line = "a.b=";
    std::size_t count = 0;
    while (count < sequences.size() && line.size() + 1 + sequences[count].size() <= 4094) {
        line += (count == 0 ? "" : "|") + sequences[count];
        ++count;
    }
    ASSERT_GT(count, kMaxSequencesPerBinding);
    const auto compact = parseKeymap("frost-keymap 1\nid x\nname X\n[default]\n" + line + "\n");
    ASSERT_FALSE(compact.has_value());
    EXPECT_EQ(compact.error().code(), ErrorCode::Corrupt);
}

// The longest canonical line the parser can accept still fits the 4096-byte line limit:
// a 64-byte id and kMaxSequencesPerBinding of the longest two-stroke sequences.
TEST(KeymapFormat, CanonicalLinesFitTheLineLimit) {
    std::size_t longestStroke = 0;
    Stroke longest{};
    for (unsigned bits = 0; bits < 16; ++bits) {
        for (std::size_t k = 1; k < kKeyCount; ++k) {
            const Stroke stroke{Modifiers::fromBits(static_cast<std::uint8_t>(bits)), static_cast<Key>(k)};
            const std::string text = formatSequence(Sequence::of(stroke, stroke));
            if (text.size() > longestStroke) {
                longestStroke = text.size();
                longest = stroke;
            }
        }
    }
    const std::string id = "a." + std::string(62, 'b');
    ASSERT_TRUE(isValidActionId(id));
    std::string line = id + " =";
    // "<id> =", then " " + the first sequence and " | " + each further one.
    const std::size_t bound =
        line.size() + 1 + kMaxSequencesPerBinding * longestStroke + (kMaxSequencesPerBinding - 1) * 3;
    EXPECT_LE(bound, 4096U) << "longest sequence " << longestStroke << " bytes";
    // A real line of that shape: the longest first stroke, distinct second strokes.
    for (std::size_t i = 0; i < kMaxSequencesPerBinding; ++i) {
        const Stroke second{longest.mods, static_cast<Key>(1 + i)};
        line += (i == 0 ? " " : " | ") + formatSequence(Sequence::of(longest, second));
    }
    EXPECT_LE(line.size(), bound);
    const auto keymap = parseKeymap("frost-keymap 1\nid x\nname X\n[default]\n" + line + "\n");
    ASSERT_TRUE(keymap.has_value()) << keymap.error().message();
    const std::string formatted = formatKeymap(*keymap);
    const auto again = parseKeymap(formatted);
    ASSERT_TRUE(again.has_value()) << again.error().message();
    EXPECT_EQ(*again, *keymap);
}

// A compact file within 1 MiB whose canonical form is not is rejected, so the round trip
// holds for every keymap the parser returns; one whose canonical form fits is accepted.
TEST(KeymapFormat, CanonicalFormFitsTheSizeCap) {
    const auto compactKeymap = [](std::size_t limit) {
        std::string text = "frost-keymap 1\nid x\nname X\n[default]\n";
        for (std::size_t i = 0;; ++i) {
            std::string line = "a.b" + std::to_string(i) + "=Ctrl+F1|Ctrl+F2\n";
            if (text.size() + line.size() > limit) {
                break;
            }
            text += line;
        }
        return text;
    };
    const std::string big = compactKeymap(std::size_t{1} << 20);
    ASSERT_LE(big.size(), std::size_t{1} << 20);
    const auto rejected = parseKeymap(big);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(rejected.error().message().find("canonical form"), std::string::npos) << rejected.error().message();

    // 80% of the cap: the canonical form's 4 more bytes per line of about 25 keep it inside.
    const std::string fits = compactKeymap((std::size_t{1} << 20) * 80 / 100);
    const auto accepted = parseKeymap(fits);
    ASSERT_TRUE(accepted.has_value()) << accepted.error().message();
    const std::string formatted = formatKeymap(*accepted);
    EXPECT_GT(formatted.size(), fits.size());
    EXPECT_LE(formatted.size(), std::size_t{1} << 20);
    const auto again = parseKeymap(formatted);
    ASSERT_TRUE(again.has_value()) << again.error().message();
    EXPECT_EQ(*again, *accepted);
}

// Many bindings with ids in descending order (the worst case of a sorted-vector insert;
// InputPerf.KeymapParseDescendingIds times it) parse, and a late duplicate is found.
TEST(KeymapFormat, ManyBindingsInDescendingOrder) {
    constexpr int kCount = 60000;
    std::string text = "frost-keymap 1\nid x\nname X\n[default]\n";
    for (int i = kCount; i > 0; --i) {
        text += std::format("a.x{:06} =\n", i);
    }
    const auto keymap = parseKeymap(text);
    ASSERT_TRUE(keymap.has_value()) << keymap.error().message();
    EXPECT_EQ(keymap->section(KeymapSection::Default).size(), static_cast<std::size_t>(kCount));
    // A duplicate far from its first occurrence is still found, with its line.
    text += "a.x000001 =\n";
    const auto duplicate = parseKeymap(text);
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_TRUE(duplicate.error().message().starts_with(std::format("keymap line {}, column 1: action 'a.x000001' "
                                                                    "appears twice",
                                                                    kCount + 5)))
        << duplicate.error().message();
}

// A random valid keymap: random sections, action ids and Strict sequences.
Keymap randomKeymap(std::mt19937_64& rng) {
    std::uniform_int_distribution<int> small(0, 6);
    std::uniform_int_distribution<unsigned> mods(0, 15);
    std::uniform_int_distribution<std::size_t> keys(1, kKeyCount - 1);
    std::string text = "frost-keymap 1\nid k" + std::to_string(rng() % 1000) + "\nname Random keymap\n";
    const std::array<std::string_view, 4> sections = {"default", "windows", "linux", "macos"};
    for (const std::string_view section : sections) {
        if (small(rng) < 2) {
            continue;
        }
        text += "[" + std::string(section) + "]\n";
        const int actions = small(rng);
        for (int a = 0; a < actions; ++a) {
            text +=
                "group" + std::to_string(a) + ".action" + std::to_string(rng() % 50) + "x" + std::to_string(a) + " =";
            const int count = small(rng) % 4;
            std::vector<Sequence> used;
            for (int s = 0; s < count; ++s) {
                Sequence sequence;
                if (small(rng) == 0) {
                    sequence = Sequence::of(rng() % 2 == 0 ? Gesture::DoubleShift : Gesture::DoubleCtrl);
                } else {
                    Stroke first;
                    do {
                        first = Stroke{Modifiers::fromBits(static_cast<std::uint8_t>(mods(rng))),
                                       static_cast<Key>(keys(rng))};
                    } while (isTypingKey(first.key) && !first.mods.hasCommandModifier());
                    sequence =
                        small(rng) < 2
                            ? Sequence::of(first, Stroke{Modifiers::fromBits(static_cast<std::uint8_t>(mods(rng))),
                                                         static_cast<Key>(keys(rng))})
                            : Sequence::of(first);
                }
                if (std::ranges::find(used, sequence) != used.end()) {
                    continue;
                }
                used.push_back(sequence);
                text += (used.size() == 1 ? " " : " | ") + formatSequence(sequence);
            }
            text += "\n";
        }
    }
    auto keymap = parseKeymap(text);
    EXPECT_TRUE(keymap.has_value()) << keymap.error().message() << "\n" << text;
    return keymap ? std::move(*keymap) : Keymap{};
}

TEST(KeymapFormat, FormatIsAFixpointAndRoundTrips) {
    std::mt19937_64 rng(0x6B6579u);
    for (int i = 0; i < 1000; ++i) {
        const Keymap keymap = randomKeymap(rng);
        const std::string formatted = formatKeymap(keymap);
        auto again = parseKeymap(formatted);
        ASSERT_TRUE(again.has_value()) << again.error().message() << "\n" << formatted;
        EXPECT_EQ(*again, keymap) << formatted;
        EXPECT_EQ(formatKeymap(*again), formatted);
    }
}

TEST(KeymapFormat, BuiltinKeymapLookup) {
    ASSERT_EQ(builtinKeymapIds().size(), 2U);
    EXPECT_EQ(builtinKeymapIds()[0], "intellij");
    EXPECT_EQ(builtinKeymapIds()[1], "visual-studio");
    EXPECT_FALSE(builtinKeymapText("intellij").empty());
    EXPECT_FALSE(builtinKeymapText("visual-studio").empty());
    EXPECT_TRUE(builtinKeymapText("vs").empty());
    auto missing = builtinKeymap("vs");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

} // namespace
} // namespace frost::input
