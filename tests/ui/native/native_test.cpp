// SPDX-License-Identifier: Apache-2.0
//
// Native-platform tests (Windows only, QT_QPA_PLATFORM=windows; ADR 0015 decision 1). The
// windows platform plugin applies its own palette and per-class fonts; these tests prove the
// theme's palette and the bundled UI font win over them. No widget is ever shown.

#include "frost/ui/frost_style.hpp"

#include "support/appearance_fixture.hpp"
#include <QApplication>
#include <QFontInfo>
#include <QHeaderView>
#include <QMenu>
#include <QScreen>
#include <QStatusBar>
#include <QTabBar>
#include <QToolTip>
#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace frost::ui {
namespace {

void requireWindowsPlatform() {
    ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("windows"))
        << "ui_native_tests runs with QT_QPA_PLATFORM=windows";
}

TEST(NativePalette, ThemeWinsOverPlatform) {
    requireWindowsPlatform();
    for (const char* id : {"glacier-light", "glacier-dark"}) {
        SCOPED_TRACE(id);
        AppearanceSettings settings;
        settings.theme = id;
        const auto installed = test::install(settings);
        ASSERT_TRUE(installed);
        QApplication::processEvents();
        const QPalette expected = FrostStyle::current()->standardPalette();
        const QPalette actual = QApplication::palette();
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            for (int role = 0; role < QPalette::NColorRoles; ++role) {
                const auto r = static_cast<QPalette::ColorRole>(role);
                if (r == QPalette::NoRole) {
                    continue;
                }
                EXPECT_EQ(actual.color(group, r), expected.color(group, r)) << "group " << group << " role " << role;
            }
        }
        // And against the theme directly, for a few roles whatever the OS scheme.
        const Theme& theme = installed->manager->theme();
        EXPECT_EQ(actual.color(QPalette::Active, QPalette::Window),
                  QColor::fromRgba(theme.color(ColorKey::PanelBackground).argb));
        EXPECT_EQ(actual.color(QPalette::Active, QPalette::Text),
                  QColor::fromRgba(theme.color(ColorKey::TextFieldForeground).argb));
        EXPECT_EQ(actual.color(QPalette::Active, QPalette::Highlight),
                  QColor::fromRgba(theme.color(ColorKey::ListSelectionBackground).argb));
    }
}

TEST(NativeFonts, ClassFontsFollowAppFont) {
    requireWindowsPlatform();
    const auto installed = test::install({});
    ASSERT_TRUE(installed);
    ASSERT_FALSE(installed->report.uiFamilies.isEmpty());
    const QString family = installed->report.uiFamilies.front();
    const double uiPx = installed->manager->scale().fontPx(installed->manager->theme(), MetricKey::FontUiSize);
    QMenu menu;
    QTabBar tabs;
    QHeaderView header(Qt::Horizontal);
    QStatusBar status;
    const std::pair<const char*, QFont> fonts[] = {
        {"QMenu", menu.font()},         {"QToolTip", QToolTip::font()}, {"QTabBar", tabs.font()},
        {"QHeaderView", header.font()}, {"QStatusBar", status.font()},
    };
    for (const auto& [name, font] : fonts) {
        SCOPED_TRACE(name);
        const QFontInfo info(font);
        EXPECT_EQ(info.family(), family);
        // QFontInfo reports the point size; convert at the screen's logical DPI.
        const double px = info.pointSizeF() * QGuiApplication::primaryScreen()->logicalDotsPerInch() / 72.0;
        EXPECT_NEAR(px, uiPx, 0.5);
    }
}

} // namespace
} // namespace frost::ui
