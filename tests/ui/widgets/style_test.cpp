// SPDX-License-Identifier: Apache-2.0
//
// FrostStyle tests (offscreen): the palette against the compiled theme blobs, and the metrics
// and size hints against UiScale at several zooms and both densities.

#include "frost/io/path.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include "support/appearance_fixture.hpp"
#include <QApplication>
#include <QComboBox>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTreeView>
#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace frost::ui {
namespace {

constexpr std::string_view kThemes[] = {"glacier-dark", "glacier-light", "glacier-classic", "glacier-high-contrast"};

std::shared_ptr<const Theme> loadInstalledTheme(std::string_view id) {
    const std::string path = *io::joinPath(*io::joinPath(test::resourceDir(), "themes"), std::string(id) + ".ftheme");
    auto theme = loadThemeFile(io::realFileSystem(), path, id);
    EXPECT_TRUE(theme) << theme.error().describe();
    return theme ? std::make_shared<const Theme>(std::move(*theme)) : nullptr;
}

QColor qc(const Theme& theme, ColorKey key) { return QColor::fromRgba(theme.color(key).argb); }

TEST(FrostStyle, PaletteMatchesTheme) {
    // A palette set before install() must not survive it.
    QApplication::setPalette(QPalette(Qt::red));
    for (const std::string_view id : kThemes) {
        SCOPED_TRACE(id);
        AppearanceSettings settings;
        settings.theme = std::string(id);
        const auto installed = test::install(settings);
        ASSERT_TRUE(installed);
        // The oracle is the blob as decoded independently of the manager.
        const auto theme = loadInstalledTheme(id);
        ASSERT_TRUE(theme);
        const QPalette palette = QApplication::palette();
        const QColor border = qc(*theme, ColorKey::ComponentBorderColor);
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            SCOPED_TRACE(static_cast<int>(group));
            const bool disabled = group == QPalette::Disabled;
            const std::pair<QPalette::ColorRole, ColorKey> roles[] = {
                {QPalette::Window, ColorKey::PanelBackground},
                {QPalette::WindowText, disabled ? ColorKey::LabelDisabledForeground : ColorKey::LabelForeground},
                {QPalette::Base, ColorKey::TextFieldBackground},
                {QPalette::AlternateBase, ColorKey::ListHoverBackground},
                {QPalette::Text, disabled ? ColorKey::LabelDisabledForeground : ColorKey::TextFieldForeground},
                {QPalette::Button, ColorKey::ButtonBackground},
                {QPalette::ButtonText, disabled ? ColorKey::LabelDisabledForeground : ColorKey::ButtonForeground},
                {QPalette::BrightText, ColorKey::LabelForeground},
                {QPalette::Highlight, group == QPalette::Inactive ? ColorKey::ListSelectionInactiveBackground
                                                                  : ColorKey::ListSelectionBackground},
                {QPalette::HighlightedText, ColorKey::ListSelectionForeground},
                {QPalette::Link, ColorKey::LinkForeground},
                {QPalette::LinkVisited, ColorKey::LinkForeground},
                {QPalette::PlaceholderText, ColorKey::TextFieldPlaceholderForeground},
                {QPalette::ToolTipBase, ColorKey::ToolTipBackground},
                {QPalette::ToolTipText, ColorKey::ToolTipForeground},
                {QPalette::Accent, ColorKey::ComponentFocusColor},
            };
            for (const auto& [role, key] : roles) {
                EXPECT_EQ(palette.color(group, role), qc(*theme, key)) << info(key).name << " role " << role;
            }
            EXPECT_EQ(palette.color(group, QPalette::Mid), border);
            EXPECT_EQ(palette.color(group, QPalette::Light), border.lighter(150));
            EXPECT_EQ(palette.color(group, QPalette::Midlight), border.lighter(125));
            EXPECT_EQ(palette.color(group, QPalette::Dark), border.darker(150));
            EXPECT_EQ(palette.color(group, QPalette::Shadow), border.darker(200));
        }
        // polish(QPalette&) assigns the whole palette.
        QPalette polished(Qt::green);
        FrostStyle::current()->polish(polished);
        EXPECT_EQ(polished, palette);
    }
}

TEST(FrostStyle, MetricsScale) {
    const auto theme = loadInstalledTheme("glacier-dark");
    ASSERT_TRUE(theme);
    std::unique_ptr<QStyle> fusion(QStyleFactory::create(QStringLiteral("Fusion")));
    QTreeView tree;
    QComboBox combo;
    for (const std::uint16_t zoom : std::initializer_list<std::uint16_t>{80, 100, 125, 200}) {
        for (const Density density : {Density::Normal, Density::Compact}) {
            SCOPED_TRACE(testing::Message() << zoom << "% " << (density == Density::Compact ? "compact" : "normal"));
            const UiScale scale{density, zoom};
            FrostStyle style(theme, scale, nullptr, 1);
            const auto px = [&](MetricKey key) { return scale.metricPx(*theme, key); };
            const std::pair<QStyle::PixelMetric, MetricKey> metrics[] = {
                {QStyle::PM_SmallIconSize, MetricKey::IconSize},
                {QStyle::PM_ListViewIconSize, MetricKey::IconSize},
                {QStyle::PM_TabBarIconSize, MetricKey::IconSize},
                {QStyle::PM_ButtonIconSize, MetricKey::IconSize},
                {QStyle::PM_ToolBarIconSize, MetricKey::ToolbarIconSize},
                {QStyle::PM_ScrollBarExtent, MetricKey::ScrollBarWidth},
                {QStyle::PM_FocusFrameHMargin, MetricKey::ComponentFocusWidth},
                {QStyle::PM_FocusFrameVMargin, MetricKey::ComponentFocusWidth},
                {QStyle::PM_ButtonMargin, MetricKey::ComponentPaddingH},
                {QStyle::PM_LayoutHorizontalSpacing, MetricKey::LayoutSpacing},
                {QStyle::PM_LayoutVerticalSpacing, MetricKey::LayoutSpacing},
                {QStyle::PM_LayoutLeftMargin, MetricKey::LayoutMargin},
                {QStyle::PM_LayoutTopMargin, MetricKey::LayoutMargin},
                {QStyle::PM_LayoutRightMargin, MetricKey::LayoutMargin},
                {QStyle::PM_LayoutBottomMargin, MetricKey::LayoutMargin},
            };
            for (const auto& [metric, key] : metrics) {
                EXPECT_EQ(style.pixelMetric(metric), px(key)) << info(key).name;
            }
            EXPECT_EQ(style.pixelMetric(QStyle::PM_DefaultFrameWidth), 1);
            // Fusion's check and radio indicators follow the zoom.
            for (const QStyle::PixelMetric indicator :
                 {QStyle::PM_IndicatorWidth, QStyle::PM_ExclusiveIndicatorHeight}) {
                EXPECT_EQ(style.pixelMetric(indicator), scale.px(fusion->pixelMetric(indicator)));
            }

            const QSize small(10, 4);
            const auto clamped = [&](QStyle::ContentsType type, const QStyleOption& option, const QWidget* widget,
                                     MetricKey key) {
                const int base = fusion->sizeFromContents(type, &option, small, widget).height();
                EXPECT_EQ(style.sizeFromContents(type, &option, small, widget).height(), std::max(base, px(key)))
                    << info(key).name;
            };
            QStyleOptionButton button;
            clamped(QStyle::CT_PushButton, button, nullptr, MetricKey::ButtonHeight);
            QStyleOptionFrame frame;
            clamped(QStyle::CT_LineEdit, frame, nullptr, MetricKey::TextFieldHeight);
            QStyleOptionComboBox comboOption;
            comboOption.initFrom(&combo);
            clamped(QStyle::CT_ComboBox, comboOption, &combo, MetricKey::TextFieldHeight);
            QStyleOptionViewItem item;
            clamped(QStyle::CT_ItemViewItem, item, nullptr, MetricKey::ListRowHeight);
            clamped(QStyle::CT_ItemViewItem, item, &tree, MetricKey::TreeRowHeight);
            QStyleOptionMenuItem menuItem;
            menuItem.menuItemType = QStyleOptionMenuItem::Normal;
            clamped(QStyle::CT_MenuItem, menuItem, nullptr, MetricKey::MenuItemHeight);
            QStyleOptionTab tab;
            clamped(QStyle::CT_TabBarTab, tab, nullptr, MetricKey::TabbedPaneTabHeight);
        }
    }
    // A literal table, independent of UiScale's arithmetic.
    QStyleOptionButton button;
    EXPECT_EQ(FrostStyle(theme, {Density::Normal, 125}, nullptr, 1)
                  .sizeFromContents(QStyle::CT_PushButton, &button, QSize(10, 4), nullptr)
                  .height(),
              35);
    EXPECT_EQ(FrostStyle(theme, {Density::Compact, 125}, nullptr, 1)
                  .sizeFromContents(QStyle::CT_PushButton, &button, QSize(10, 4), nullptr)
                  .height(),
              30);
    EXPECT_EQ(FrostStyle(theme, {Density::Normal, 100}, nullptr, 1).pixelMetric(QStyle::PM_LayoutLeftMargin), 8);
    EXPECT_EQ(FrostStyle(theme, {Density::Normal, 100}, nullptr, 1).styleHint(QStyle::SH_Widget_Animation_Duration), 0);
}

TEST(FrostStyle, BordersSnapToDevicePixels) {
    const auto theme = loadInstalledTheme("glacier-light");
    ASSERT_TRUE(theme);
    FrostStyle style(theme, {}, nullptr, 1);
    // At dpr 1.5 a one-logical-pixel offset puts the panel's left edge at device x 1.5.
    QImage image(30, 45, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(1.5);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        EXPECT_EQ(painter.deviceTransform().m11(), 1.5) << "deviceTransform includes the dpr";
        painter.translate(1.0, 1.0);
        QStyleOptionButton option;
        option.rect = QRect(0, 0, 16, 24); // tall enough for a straight edge between the 9 px arcs
        option.state = QStyle::State_Enabled | QStyle::State_Raised;
        style.drawPrimitive(QStyle::PE_PanelButtonCommand, &option, &painter, nullptr);
    }
    // Snapped: the edge moves to x 2 and the 1 px border becomes 2 whole device pixels.
    const int y = 20;
    EXPECT_EQ(qAlpha(image.pixel(1, y)), 0) << "nothing left of the snapped edge";
    EXPECT_EQ(qAlpha(image.pixel(2, y)), 255);
    EXPECT_EQ(image.pixel(2, y), image.pixel(3, y)) << "the border covers whole pixels";
    EXPECT_EQ(QColor::fromRgba(image.pixel(5, y)), qc(*theme, ColorKey::ButtonBackground));
}

} // namespace
} // namespace frost::ui
