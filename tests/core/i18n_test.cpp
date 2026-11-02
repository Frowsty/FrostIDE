// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::i18n (frost/core/i18n.hpp): the identity default, an installed translator and
// pseudo-localization, including the parts it must keep (placeholders, mnemonics, shortcuts).

#include "frost/core/i18n.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace {

using frost::i18n::pseudoLocalize;
using frost::i18n::tr;

// Translates one (context, source) pair; everything else stays English.
class OneWordTranslator final : public frost::i18n::ITranslator {
public:
    [[nodiscard]] std::optional<std::string> translate(std::string_view context,
                                                       std::string_view source) const override {
        if (context == "Shell" && source == "File") {
            return std::string("Datei");
        }
        return std::nullopt;
    }
};

// Restores the process-wide switches after each test.
class I18n : public ::testing::Test {
protected:
    void TearDown() override {
        frost::i18n::setTranslator(nullptr);
        frost::i18n::setPseudoLocalization(false);
    }
};

TEST_F(I18n, ReturnsTheSourceByDefault) {
    EXPECT_EQ(tr("Shell", "Open File..."), "Open File...");
    EXPECT_EQ(frost::i18n::translator(), nullptr);
    EXPECT_FALSE(frost::i18n::pseudoLocalization());
}

TEST_F(I18n, UsesTheInstalledTranslatorPerContext) {
    const OneWordTranslator translator;
    frost::i18n::setTranslator(&translator);
    EXPECT_EQ(tr("Shell", "File"), "Datei");
    EXPECT_EQ(tr("Other", "File"), "File");
    EXPECT_EQ(tr("Shell", "Edit"), "Edit");
}

TEST_F(I18n, PseudoLocalizationAccentsAndPads) {
    const std::string text = pseudoLocalize("File");
    // [ + 4 accented letters + space + 2 padding + ].
    EXPECT_EQ(text, "[\xC6\x91\xC3\xAD\xC4\xBA\xC3\xA9 ~~]");
    frost::i18n::setPseudoLocalization(true);
    EXPECT_EQ(tr("Shell", "File"), text);
}

TEST_F(I18n, PseudoLocalizationKeepsPlaceholdersMnemonicsAndShortcuts) {
    const std::string text = pseudoLocalize("&Open %1 of %n\tCtrl+O");
    EXPECT_NE(text.find("&O"), std::string::npos) << text;
    EXPECT_NE(text.find("%1"), std::string::npos) << text;
    EXPECT_NE(text.find("%n"), std::string::npos) << text;
    EXPECT_TRUE(text.ends_with("]\tCtrl+O")) << text;
    EXPECT_TRUE(text.starts_with("[")) << text;
}

TEST_F(I18n, PseudoLocalizationOfEmptyTextStillMarksIt) { EXPECT_EQ(pseudoLocalize(""), "[ ~]"); }

} // namespace
