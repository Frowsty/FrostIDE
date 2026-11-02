// SPDX-License-Identifier: Apache-2.0
//
// toDisplayText through frost::i18n (ADR 0038): modifier and named-key labels are looked up in the
// "frost::input::keys" context, while layout labels, digits and F-keys stay as they are.

#include "frost/core/i18n.hpp"
#include "frost/input/key.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace frost::input {
namespace {

// A German translation of a few key names.
class GermanKeys final : public i18n::ITranslator {
public:
    [[nodiscard]] std::optional<std::string> translate(std::string_view context,
                                                       std::string_view source) const override {
        if (context != "frost::input::keys") {
            return std::nullopt;
        }
        if (source == "Ctrl") {
            return "Strg";
        }
        if (source == "Shift") {
            return "Umschalt";
        }
        if (source == "Delete") {
            return "Entf";
        }
        if (source == "Double Shift") {
            return "Zweimal Umschalt";
        }
        return std::nullopt;
    }
};

std::string display(std::string_view text, SimulatedLayoutId layout = SimulatedLayoutId::DeDe) {
    const Result<Sequence> sequence = parseSequence(text, SyntaxMode::Strict);
    EXPECT_TRUE(sequence.has_value()) << text;
    return sequence ? toDisplayText(*sequence, simulatedLayout(layout), KeymapPlatform::Windows) : std::string();
}

TEST(DisplayTextI18n, UsesTheTranslatorForKeyNames) {
    const GermanKeys german;
    i18n::setTranslator(&german);
    EXPECT_EQ(display("Ctrl+Slash"), "Strg+-"); // The de-DE label of the Slash key is '-'.
    EXPECT_EQ(display("Ctrl+Shift+Delete"), "Strg+Umschalt+Entf");
    EXPECT_EQ(display("Alt+Digit1"), "Alt+1"); // Untranslated names fall back to the source.
    EXPECT_EQ(display("Shift+F6"), "Umschalt+F6");
    EXPECT_EQ(display("DoubleShift"), "Zweimal Umschalt");
    i18n::setTranslator(nullptr);
    EXPECT_EQ(display("Ctrl+Slash"), "Ctrl+-");
    EXPECT_EQ(display("Ctrl+Slash", SimulatedLayoutId::EnUs), "Ctrl+/");
}

} // namespace
} // namespace frost::input
