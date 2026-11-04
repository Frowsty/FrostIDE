// SPDX-License-Identifier: Apache-2.0
//
// FrostStyle tests (offscreen): the palette against the compiled theme blobs, the metrics and
// size hints against UiScale at several zooms and both densities, the focus indicator on every
// button kind, the theme keys of menus, combo boxes and trees, and IslandFrame's inactive
// border.

#include "frost/io/path.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/glacial_logo.hpp"
#include "frost/ui/island_frame.hpp"
#include "frost/ui/model/contrast.hpp"
#include "frost/ui/model/theme_blob.hpp"

#include "support/appearance_fixture.hpp"
#include "support/golden.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolButton>
#include <QTreeView>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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

// An opaque pixel as an Rgba.
Rgba toRgba(QRgb pixel) { return Rgba{0xff000000U | (pixel & 0x00ffffffU)}; }

// `top` composited over the opaque `bottom` (source-over).
QColor over(const QColor& top, const QColor& bottom) {
    const double a = top.alphaF();
    const auto mix = [a](int t, int b) { return static_cast<int>(std::lround(t * a + b * (1.0 - a))); };
    return QColor(mix(top.red(), bottom.red()), mix(top.green(), bottom.green()), mix(top.blue(), bottom.blue()));
}

// Opaque colours within `tolerance` per channel.
::testing::AssertionResult closeTo(const QColor& actual, const QColor& expected, int tolerance = 2) {
    const int diff = std::max({std::abs(actual.red() - expected.red()), std::abs(actual.green() - expected.green()),
                               std::abs(actual.blue() - expected.blue())});
    if (diff <= tolerance) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << actual.name().toStdString() << " is not " << expected.name().toStdString();
}

// The pixels of `area` in `image` within `tolerance` per channel of the opaque `color`.
int countNear(const QImage& image, const QRect& area, const QColor& color, int tolerance = 2) {
    int count = 0;
    const QRect clipped = area.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            count += closeTo(image.pixelColor(x, y), color, tolerance) ? 1 : 0;
        }
    }
    return count;
}

// An ARGB32_Premultiplied image of `size` logical pixels at `dpr`, filled with the opaque `fill`.
QImage canvas(QSize size, qreal dpr, const QColor& fill) {
    QImage image(QSize(qRound(size.width() * dpr), qRound(size.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(fill);
    return image;
}

/*
 * The pixels of `area` in `image` that lie on the line from `from` to `to`, at least `minT` of
 * the way (within `tolerance` per channel of it): text or a shape drawn in `to` over `from`,
 * antialiased. A pixel of any other colour does not count.
 */
int countBlends(const QImage& image, const QRect& area, const QColor& from, const QColor& to, double minT,
                int tolerance = 40) {
    const double d[3] = {static_cast<double>(to.red() - from.red()), static_cast<double>(to.green() - from.green()),
                         static_cast<double>(to.blue() - from.blue())};
    const double length2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    int count = 0;
    const QRect clipped = area.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QRgb p = image.pixel(x, y);
            const double v[3] = {static_cast<double>(qRed(p) - from.red()),
                                 static_cast<double>(qGreen(p) - from.green()),
                                 static_cast<double>(qBlue(p) - from.blue())};
            const double t = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / length2;
            if (t < minT || t > 1.1) {
                continue;
            }
            const double residual =
                std::max({std::abs(v[0] - t * d[0]), std::abs(v[1] - t * d[1]), std::abs(v[2] - t * d[2])});
            count += residual <= tolerance ? 1 : 0;
        }
    }
    return count;
}

// A button panel (PE_PanelButtonCommand) of `size` logical pixels at `dpr` over Island.background.
QImage renderButtonPanel(const FrostStyle& style, QSize size, qreal dpr, bool isDefault, QStyle::State state) {
    QImage image = canvas(size, dpr, style.color(ColorKey::IslandBackground));
    QPainter painter(&image);
    QStyleOptionButton option;
    option.rect = QRect(QPoint(0, 0), size);
    option.state = QStyle::State_Enabled | QStyle::State_Raised | state;
    option.features = isDefault ? QStyleOptionButton::DefaultButton : QStyleOptionButton::None;
    option.palette = style.standardPalette();
    style.drawPrimitive(QStyle::PE_PanelButtonCommand, &option, &painter, nullptr);
    painter.end();
    return image;
}

TEST(FrostStyle, FocusIndicatorVisibleOnEveryButton) {
    /*
     * WCAG 2.2 SC 1.4.11 for the focus indicator: the pixels that change when a button gains
     * focus change by at least 3:1, along at least a one-device-pixel line as long as the
     * button's straight edges. Glacier Light's default fill is the focus colour itself and the
     * dark themes' is close to it, so this needs the two-colour indicator of drawButtonPanel.
     */
    const QSize size(96, 28);
    for (const std::string_view id : kThemes) {
        const auto theme = loadInstalledTheme(id);
        ASSERT_TRUE(theme);
        const FrostStyle style(theme, {}, nullptr, 1);
        const int radius = style.metricPx(MetricKey::ButtonArc);
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            for (const bool isDefault : {false, true}) {
                for (const bool hover : {false, true}) {
                    SCOPED_TRACE(testing::Message() << id << " at " << dpr << (isDefault ? " default" : " normal")
                                                    << (hover ? " hovered" : " resting"));
                    const QStyle::State state = hover ? QStyle::State_MouseOver : QStyle::State_None;
                    const QImage rest = renderButtonPanel(style, size, dpr, isDefault, state);
                    const QImage focused =
                        renderButtonPanel(style, size, dpr, isDefault, state | QStyle::State_HasFocus);
                    int changed = 0;
                    for (int y = 0; y < rest.height(); ++y) {
                        for (int x = 0; x < rest.width(); ++x) {
                            changed +=
                                contrastRatio(toRgba(rest.pixel(x, y)), toRgba(focused.pixel(x, y))) >= 3.0 ? 1 : 0;
                        }
                    }
                    const double straight =
                        2.0 * ((size.width() - 2.0 * radius) + (size.height() - 2.0 * radius)) * dpr;
                    EXPECT_GE(changed, 0.9 * straight) << "pixels changing by 3:1 or more";
                }
            }
        }
    }
}

TEST(FrostStyle, FocusedPushButtonDrawsOneRing) {
    /*
     * A real QPushButton holding focus (shown on the offscreen platform) paints through
     * CE_PushButton, which may also ask for PE_FrameFocusRect at SE_PushButtonFocusRect.
     * drawButtonPanel already draws the ring on the button's outer edge, so no focus-coloured
     * pixel may appear inside it: scanned on the middle row and on a column, both clear of the
     * centred label. The default button also shows its inner Button.default.foreground line.
     */
    ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("offscreen")) << "this test shows a widget";
    AppearanceSettings settings;
    settings.theme = "glacier-dark";
    const auto installed = test::install(settings);
    ASSERT_TRUE(installed);
    const FrostStyle& style = *FrostStyle::current();
    const QColor focus = style.color(ColorKey::ComponentFocusColor);
    const int ring = style.metricPx(MetricKey::ComponentFocusWidth);
    for (const bool isDefault : {false, true}) {
        SCOPED_TRACE(isDefault ? "default" : "normal");
        QPushButton button(QStringLiteral("Wwww"));
        button.setDefault(isDefault);
        button.setFocusPolicy(Qt::StrongFocus);
        button.resize(120, 28);
        button.show();
        button.activateWindow();
        button.setFocus(Qt::TabFocusReason);
        QElapsedTimer timer;
        timer.start();
        while (!button.hasFocus() && timer.elapsed() < 2000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        ASSERT_TRUE(button.hasFocus());
        const QImage image = test::renderAt(button, 1.0);
        // The offscreen cursor may rest on the button, which then draws hovered.
        const QColor fill =
            style.color(button.underMouse() ? ColorKey::ButtonHoverBackground : ColorKey::ButtonBackground);
        ASSERT_TRUE(closeTo(image.pixelColor(button.width() / 2, 0), focus)) << "the outer ring";
        EXPECT_TRUE(closeTo(image.pixelColor(button.width() / 2, ring),
                            isDefault ? style.color(ColorKey::ButtonDefaultForeground) : fill))
            << "just inside the ring";
        int inside = 0;
        const int middle = button.height() / 2;
        for (int x = ring; x < 30; ++x) {
            inside += closeTo(image.pixelColor(x, middle), focus, 40) ? 1 : 0;
            inside += closeTo(image.pixelColor(button.width() - 1 - x, middle), focus, 40) ? 1 : 0;
        }
        for (int y = ring; y < button.height() - ring; ++y) {
            inside += closeTo(image.pixelColor(16, y), focus, 40) ? 1 : 0;
        }
        EXPECT_EQ(inside, 0) << "a second focus ring inside the first";
    }
}

TEST(FrostStyle, MenuComboAndTreeKeysAreDrawn) {
    // Distinct values for the keys the style draws (and that the palette does not carry), so
    // each sample can only come from its own key.
    const auto base = loadInstalledTheme("glacier-dark");
    ASSERT_TRUE(base);
    ThemeData data = base->data();
    const auto set = [&data](ColorKey key, QRgb value) {
        data.colors[static_cast<std::size_t>(key)] = Rgba{0xff000000U | value};
    };
    set(ColorKey::MenuBackground, 0x203A60);
    set(ColorKey::MenuForeground, 0xF0D020);
    set(ColorKey::MenuSelectionBackground, 0x8A1F6A);
    set(ColorKey::MenuSelectionForeground, 0x20F0E0);
    set(ColorKey::ComboBoxBackground, 0x1F4A1F);
    set(ColorKey::ComboBoxForeground, 0xFF9A30);
    set(ColorKey::ComboBoxBorderColor, 0xC0F040);
    set(ColorKey::TreeSelectionBackground, 0x5A2A10);
    set(ColorKey::TreeSelectionForeground, 0x80FF40);
    auto theme = Theme::fromData(data);
    ASSERT_TRUE(theme) << theme.error().describe();
    const FrostStyle style(std::make_shared<const Theme>(std::move(*theme)), {}, nullptr, 1);
    const QColor island = style.color(ColorKey::IslandBackground);
    constexpr int kMinTextPixels = 10;

    {
        SCOPED_TRACE("menu");
        QMenu menu;
        const QSize size(160, 26);
        for (const bool selected : {false, true}) {
            SCOPED_TRACE(selected ? "selected" : "normal");
            QImage image = canvas(size, 1.0, island);
            QStyleOptionMenuItem item;
            item.initFrom(&menu);
            item.palette = style.standardPalette();
            item.rect = QRect(QPoint(0, 0), size);
            item.menuItemType = QStyleOptionMenuItem::Normal;
            item.text = QStringLiteral("Wwww Wwww");
            item.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_None);
            {
                QPainter painter(&image);
                QStyleOption panel;
                panel.initFrom(&menu);
                panel.rect = item.rect;
                style.drawPrimitive(QStyle::PE_PanelMenu, &panel, &painter, &menu);
                EXPECT_TRUE(closeTo(image.pixelColor(2, 2), style.color(ColorKey::MenuBackground)))
                    << "PE_PanelMenu fills with Menu.background";
                style.drawControl(QStyle::CE_MenuItem, &item, &painter, &menu);
            }
            const QColor fill = style.color(selected ? ColorKey::MenuSelectionBackground : ColorKey::MenuBackground);
            const QColor text = style.color(selected ? ColorKey::MenuSelectionForeground : ColorKey::MenuForeground);
            EXPECT_GE(countNear(image, image.rect(), fill), size.width() * size.height() / 2) << "the item's fill";
            EXPECT_GE(countBlends(image, image.rect(), fill, text, 0.5), kMinTextPixels) << "the item's text";
        }
    }
    {
        SCOPED_TRACE("combo box");
        QComboBox combo;
        const QSize size(140, 28);
        QImage image = canvas(size, 1.0, island);
        QStyleOptionComboBox option;
        option.initFrom(&combo);
        option.palette = style.standardPalette();
        option.rect = QRect(QPoint(0, 0), size);
        option.state = QStyle::State_Enabled;
        option.currentText = QStringLiteral("Wwww Wwww");
        option.editable = false;
        option.frame = true;
        option.subControls = QStyle::SC_All;
        {
            QPainter painter(&image);
            style.drawComplexControl(QStyle::CC_ComboBox, &option, &painter, &combo);
            style.drawControl(QStyle::CE_ComboBoxLabel, &option, &painter, &combo);
        }
        const QColor background = style.color(ColorKey::ComboBoxBackground);
        const QColor foreground = style.color(ColorKey::ComboBoxForeground);
        EXPECT_TRUE(closeTo(image.pixelColor(size.width() / 2, 0), style.color(ColorKey::ComboBoxBorderColor)))
            << "border";
        EXPECT_TRUE(closeTo(image.pixelColor(1, size.height() / 2), background)) << "fill";
        const QRect field = style.subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, &combo);
        const QRect arrow = style.subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, &combo);
        EXPECT_GE(countBlends(image, field, background, foreground, 0.5), kMinTextPixels) << "label";
        EXPECT_GE(countBlends(image, arrow, background, foreground, 0.9, 8), 4) << "arrow";
    }
    {
        SCOPED_TRACE("tree");
        QTreeView tree;
        const QSize size(160, 24);
        const QColor background = style.color(ColorKey::TreeSelectionBackground);
        const QColor explicitText(0xFF, 0x30, 0x30);
        for (const bool explicitColor : {false, true}) {
            SCOPED_TRACE(explicitColor ? "explicit HighlightedText" : "style HighlightedText");
            QImage image = canvas(size, 1.0, island);
            QStyleOptionViewItem item;
            item.initFrom(&tree);
            item.widget = &tree;
            item.palette = style.standardPalette();
            if (explicitColor) {
                item.palette.setColor(QPalette::HighlightedText, explicitText);
            }
            item.rect = QRect(QPoint(0, 0), size);
            item.state = QStyle::State_Enabled | QStyle::State_Selected | QStyle::State_Active;
            item.features = QStyleOptionViewItem::HasDisplay;
            item.text = QStringLiteral("Wwww Wwww");
            item.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
            item.showDecorationSelected = true;
            {
                QPainter painter(&image);
                style.drawControl(QStyle::CE_ItemViewItem, &item, &painter, &tree);
            }
            EXPECT_TRUE(closeTo(image.pixelColor(size.width() - 3, size.height() / 2), background)) << "selection";
            const QColor text = explicitColor ? explicitText : style.color(ColorKey::TreeSelectionForeground);
            EXPECT_GE(countBlends(image, image.rect(), background, text, 0.5), kMinTextPixels) << "text";
        }
    }
}

TEST(IslandFrame, InactiveWindowFadesTheBorder) {
    ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("offscreen")) << "this test shows a widget";
    AppearanceSettings settings;
    settings.theme = "glacier-light";
    const auto installed = test::install(settings);
    ASSERT_TRUE(installed);
    const FrostStyle& style = *FrostStyle::current();
    const QColor island = style.color(ColorKey::IslandBackground);
    const QColor border = style.color(ColorKey::IslandBorderColor);
    ASSERT_GT(border.alpha(), 0);
    QColor faded = border;
    faded.setAlphaF(border.alphaF() * style.theme().metric(MetricKey::WindowInactiveAlpha));
    ASSERT_FALSE(closeTo(over(faded, island), over(border, island), 8)) << "the two states must be told apart";

    IslandFrame frame;
    frame.resize(80, 60);
    const auto topEdge = [&frame] { return test::renderAt(frame, 1.0).pixelColor(40, 0); };
    ASSERT_FALSE(frame.isActiveWindow());
    EXPECT_TRUE(closeTo(topEdge(), over(faded, island))) << "inactive: border alpha x Window.inactiveAlpha";
    frame.show();
    frame.activateWindow();
    QElapsedTimer timer;
    timer.start();
    while (!frame.isActiveWindow() && timer.elapsed() < 2000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(frame.isActiveWindow());
    EXPECT_TRUE(closeTo(topEdge(), over(border, island))) << "active: the full border";
}

TEST(GlacialLogo, RendersHighContrastCrystalEmblemAndMultiSizeIcon) {
    for (const int size : {16, 24, 32, 40, 64}) {
        const QPixmap pix = glacialLogoPixmap(size, 1.0);
        ASSERT_FALSE(pix.isNull());
        EXPECT_EQ(pix.width(), size);
        EXPECT_EQ(pix.height(), size);
        const QImage img = pix.toImage();
        // Center of the vertical spine shard (around x=0.35, y=0.50) is bright icy cyan/white.
        const QColor spinePixel = img.pixelColor(static_cast<int>(size * 0.35), static_cast<int>(size * 0.50));
        EXPECT_GT(spinePixel.alpha(), 200);
        EXPECT_GT(spinePixel.blue(), 180);
    }
    const QIcon icon = glacialLogoIcon();
    EXPECT_FALSE(icon.isNull());
    EXPECT_FALSE(icon.pixmap(32, 32).isNull());
}

TEST(DialogChrome, InstallsFramelessThemedTitleBarAndCaptionButtons) {
    AppearanceSettings settings;
    settings.theme = "glacier-dark";
    const auto installed = test::install(settings);
    ASSERT_TRUE(installed);

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Settings"));
    dialog.resize(480, 320);
    DialogTitleBar* titleBar = installDialogChrome(&dialog, true);
    ASSERT_NE(titleBar, nullptr);
    EXPECT_EQ(installDialogChrome(&dialog, true), titleBar) << "idempotent on repeated calls";
    EXPECT_TRUE((dialog.windowFlags() & Qt::FramelessWindowHint) != 0);
    EXPECT_NE(titleBar->maximizeButton(), nullptr);
    EXPECT_NE(titleBar->closeButton(), nullptr);
    EXPECT_GT(dialog.contentsMargins().top(), 24);

    dialog.show();
    QApplication::processEvents();
    const QImage rendered = test::renderAt(*titleBar, 1.0);
    ASSERT_FALSE(rendered.isNull());

    titleBar->closeButton()->click();
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
}

} // namespace
} // namespace frost::ui
