// SPDX-License-Identifier: Apache-2.0
//
// Bundled font tests (offscreen): Inter is registered at install and resolves with its
// weights, JetBrains Mono registers only on ensureEditorFonts() and is fixed pitch, a missing UI
// font is a warning rather than a failure, and the Qt OS appearance source smoke test.

#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/fonts.hpp"
#include "frost/ui/qt_os_appearance.hpp"

#include "support/appearance_fixture.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QFontInfo>
#include <gtest/gtest.h>

#include <string>

namespace frost::ui {
namespace {

AppearanceSettings withTheme(std::string_view id) {
    AppearanceSettings settings;
    settings.theme = std::string(id);
    return settings;
}

TEST(Fonts, UiFamilyRegistered) {
    const auto installed = test::install(withTheme("glacier-dark"));
    ASSERT_TRUE(installed);
    ASSERT_FALSE(installed->report.uiFamilies.isEmpty());
    const QString family = installed->report.uiFamilies.front();
    EXPECT_TRUE(family.startsWith(QStringLiteral("Inter"))) << family.toStdString();
    EXPECT_EQ(QFontInfo(QApplication::font()).family(), family);
    for (const QFont::Weight weight : {QFont::Normal, QFont::Medium, QFont::DemiBold, QFont::Bold}) {
        SCOPED_TRACE(static_cast<int>(weight));
        QFont font(family);
        font.setWeight(weight);
        const QFontInfo info(font);
        EXPECT_EQ(info.family(), family);
        EXPECT_EQ(info.weight(), static_cast<int>(weight));
    }
}

TEST(Fonts, EditorFontsLazyAndFixedPitch) {
    // Install does not read the editor fonts: with them absent it succeeds without a warning,
    // and only ensureEditorFonts() reports them missing.
    {
        auto mem = test::mirrorResources({"fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"});
        ASSERT_TRUE(mem);
        const auto installed = test::install(withTheme("glacier-dark"), &mem->fs, mem->root);
        ASSERT_TRUE(installed);
        EXPECT_TRUE(installed->report.warnings.empty());
        const auto editor = installed->manager->ensureEditorFonts();
        ASSERT_FALSE(editor);
        EXPECT_EQ(editor.error().code(), ErrorCode::NotFound);
    }
    auto mem = test::mirrorResources();
    ASSERT_TRUE(mem);
    const auto installed = test::install(withTheme("glacier-dark"), &mem->fs, mem->root);
    ASSERT_TRUE(installed);
    const auto editor = installed->manager->ensureEditorFonts();
    ASSERT_TRUE(editor) << editor.error().describe();
    EXPECT_TRUE(editor->primaryFamily.startsWith(QStringLiteral("JetBrains Mono")))
        << editor->primaryFamily.toStdString();
    for (const char* expectedFamily : {"Fira Code", "Cascadia Code", "Cascadia Mono", "Source Code Pro",
                                       "IBM Plex Mono", "Inconsolata", "Intel One Mono", "CommitMono",
                                       "Fantasque Sans Mono"}) {
        EXPECT_TRUE(editor->families.contains(QString::fromUtf8(expectedFamily))) << expectedFamily;
    }
    QFont mono(editor->primaryFamily);
    mono.setPixelSize(13);
    const QFontInfo info(mono);
    EXPECT_EQ(info.family(), editor->primaryFamily);
    EXPECT_TRUE(info.fixedPitch());
    for (const bool italic : {false, true}) {
        QFont styled = mono;
        styled.setItalic(italic);
        styled.setBold(true);
        EXPECT_EQ(QFontInfo(styled).italic(), italic);
        EXPECT_EQ(QFontInfo(styled).weight(), static_cast<int>(QFont::Bold));
    }
    // The result is cached: a later read failure does not matter.
    mem->fs.injectFault(io::FsOp::Read, mem->path("fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"),
                        ErrorCode::IoError);
    const auto again = installed->manager->ensureEditorFonts();
    ASSERT_TRUE(again);
    EXPECT_EQ(again->primaryFamily, editor->primaryFamily);
}

TEST(Fonts, MissingFontIsNonFatalWarning) {
    // The process shares one QApplication: start from no application fonts.
    QFontDatabase::removeAllApplicationFonts();
    auto mem = test::mirrorResources({"fonts/inter/Inter-Regular.ttf"});
    ASSERT_TRUE(mem);
    const auto installed = test::install(withTheme("glacier-dark"), &mem->fs, mem->root);
    ASSERT_TRUE(installed);
    EXPECT_TRUE(installed->report.uiFamilies.isEmpty());
    ASSERT_EQ(installed->report.warnings.size(), 1U);
    const std::string& warning = installed->report.warnings[0];
    EXPECT_NE(warning.find("UI fonts"), std::string::npos) << warning;
    EXPECT_NE(warning.find("Inter-Regular.ttf"), std::string::npos) << warning;
    // The theme and icons are installed regardless.
    EXPECT_EQ(installed->report.themeId, "glacier-dark");
    EXPECT_TRUE(installed->manager->icons()->contains("actions.search"));
    // A corrupt file is Corrupt, through the registration function directly.
    ASSERT_TRUE(mem->fs.writeFileAtomic(mem->path("fonts/inter/Inter-Regular.ttf"), "not a font"));
    const auto corrupt = registerBundledFonts(mem->fs, mem->path("fonts/inter"), FontSet::Ui);
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().code(), ErrorCode::Corrupt);
}

TEST(Fonts, FailedRegistrationLeavesNothingRegistered) {
    // From no application fonts: any family that appears below was registered by this test.
    QFontDatabase::removeAllApplicationFonts();
    const QStringList before = QFontDatabase::families();
    ASSERT_FALSE(before.contains(QStringLiteral("JetBrains Mono"))) << "JetBrains Mono must not be a system font here";
    auto mem = test::mirrorResources();
    ASSERT_TRUE(mem);
    // The last editor file fails to read three times, after the first three were registered.
    mem->fs.injectFault(io::FsOp::Read, mem->path("fonts/jetbrains-mono/JetBrainsMono-BoldItalic.ttf"),
                        ErrorCode::IoError, 3);
    const auto installed = test::install(withTheme("glacier-dark"), &mem->fs, mem->root);
    ASSERT_TRUE(installed);
    const QStringList afterInstall = QFontDatabase::families();
    for (int attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        const auto failed = installed->manager->ensureEditorFonts();
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().code(), ErrorCode::IoError);
        EXPECT_EQ(QFontDatabase::families(), afterInstall) << "a failed call leaves no font registered";
    }
    // The next call reads every file and registers the set.
    const auto editor = installed->manager->ensureEditorFonts();
    ASSERT_TRUE(editor) << editor.error().describe();
    EXPECT_TRUE(QFontDatabase::families().contains(editor->primaryFamily));
}

TEST(QtOsAppearanceSource, Smoke) {
    QtOsAppearanceSource source;
    const OsAppearance first = source.current();
    // Unknown is a valid answer (Linux without a session bus); it must at least be stable.
    EXPECT_EQ(source.current(), first);
    int calls = 0;
    {
        const ListenerToken token = source.subscribe([&](OsAppearance) { ++calls; });
        source.refresh(); // nothing changed, so nothing is notified
        EXPECT_EQ(calls, 0);
    }
    source.refresh();
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(source.current(), first);
}

} // namespace
} // namespace frost::ui
