// SPDX-License-Identifier: Apache-2.0
//
// FrostStyle: FrostIDE's QStyle, a QProxyStyle over Qt's built-in Fusion that takes its colours
// and metrics from one compiled Theme at one UiScale (ADR 0015 decision 1). The palette is set
// wholesale (polish(QPalette&)) because the platform palette otherwise wins; Qt style sheets are
// not used.
#pragma once

#include "frost/ui/icon_atlas_set.hpp"
#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/ui_scale.hpp"

#include <QIcon>
#include <QPalette>
#include <QProxyStyle>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace frost::ui {

/*
 * One immutable appearance: a theme, a density and zoom, and the shared icon atlases.
 * AppearanceManager creates a new FrostStyle for every change (generation + 1) and hands it to
 * QApplication::setStyle, which owns it and deletes it on the next setStyle(); the base Fusion
 * style is owned by the proxy. Everything runs on the GUI thread.
 *
 * Mapping (ADR 0015 decision 1):
 *   standardPalette()  every role of the Active, Inactive and Disabled groups from the theme;
 *   pixelMetric()      icon sizes, scroll bar width, focus margins, button margin, layout
 *                      spacing and margins, all through UiScale, and Fusion's check and radio
 *                      indicator sizes times the zoom;
 *   sizeFromContents() heights at least Button.height, TextField.height, List/Tree.rowHeight,
 *                      Menu.itemHeight and TabbedPane.tabHeight (larger content keeps its size);
 *   drawPrimitive()    buttons (a focused default button adds a Button.default.foreground line
 *                      inside its focus ring), line edits, focus rings, check boxes, radio
 *                      buttons, item-view rows, the menu panel (Menu.background) and the
 *                      tab-widget frame;
 *   drawControl()      the tab shape (an accent underline), the default button's label colour
 *                      (Button.default.foreground), menu items (Menu.foreground and
 *                      Menu.selection*), the combo label (ComboBox.foreground), selected tree text
 *                      (Tree.selectionForeground) and icon-only tool buttons' icons (QIcon::paint
 *                      at the painter's scale); everything else is Fusion's;
 *   drawComplexControl() the combo box frame (a text field's, in ComboBox.background and
 *                      ComboBox.borderColor) and its arrow (ComboBox.foreground).
 * Keys no drawing path reads yet are listed as reserved in theme_registry.hpp.
 * Focus rings show whenever a widget has focus, whether it came from the keyboard or a click
 * (as in IntelliJ, and WCAG 2.4.7 Focus Visible): State_KeyboardFocusChange is not consulted. A
 * push button's ring is drawn once, by its panel (FrostStyle.FocusedPushButtonDrawsOneRing).
 * Animations are off (SH_Widget_Animation_Duration 0, decision 13). Every drawing path reads the
 * device pixel ratio from the painter's device transform (also correct under QWidget::render,
 * which redirects to the widget) and snaps shape edges to device pixels.
 */
class FrostStyle final : public QProxyStyle {
    Q_OBJECT

public:
    // Builds the palette: O(keys), well under 1 ms. Budget: B1.
    FrostStyle(std::shared_ptr<const Theme> theme, UiScale scale, std::shared_ptr<const IconAtlasSet> icons,
               std::uint64_t generation);
    ~FrostStyle() override;

    // The application's FrostStyle, or nullptr when another style is active.
    [[nodiscard]] static FrostStyle* current() noexcept;

    // Valid for this style's lifetime, that is until the next appearanceChanged().
    [[nodiscard]] const Theme& theme() const noexcept { return *m_theme; }
    [[nodiscard]] std::shared_ptr<const Theme> themePtr() const noexcept { return m_theme; }
    [[nodiscard]] UiScale scale() const noexcept { return m_scale; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] const std::shared_ptr<const IconAtlasSet>& icons() const noexcept { return m_icons; }

    // A theme metric in logical pixels at this style's density and zoom.
    [[nodiscard]] int metricPx(MetricKey key) const noexcept { return m_scale.metricPx(*m_theme, key); }
    // A theme colour as a QColor.
    [[nodiscard]] QColor color(ColorKey key) const noexcept;

    // A QIcon over FrostIconEngine for `id`, or a null QIcon for an id the bake lacks.
    [[nodiscard]] QIcon icon(std::string_view id, IconSlot slot = IconSlot::Small,
                             std::optional<IconRole> roleOverride = std::nullopt) const;

    [[nodiscard]] QPalette standardPalette() const override;
    void polish(QPalette& palette) override;
    void polish(QWidget* widget) override;
    void unpolish(QWidget* widget) override;
    using QProxyStyle::polish;
    using QProxyStyle::unpolish;

    [[nodiscard]] int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                                  const QWidget* widget = nullptr) const override;
    [[nodiscard]] QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                         const QWidget* widget) const override;
    [[nodiscard]] int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                                QStyleHintReturn* returnData = nullptr) const override;
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                            const QWidget* widget = nullptr) const override;

private:
    [[nodiscard]] QPalette buildPalette() const;
    void drawButtonPanel(const QStyleOption* option, QPainter* painter) const;
    // A text field's frame (and, with `fill`, its fill); combo boxes pass their own keys.
    void drawLineEditFrame(const QStyleOption* option, QPainter* painter, bool fill,
                           ColorKey background = ColorKey::TextFieldBackground,
                           ColorKey borderKey = ColorKey::TextFieldBorderColor) const;
    void drawFocusRing(const QRectF& rect, qreal radius, QPainter* painter) const;
    void drawCheckIndicator(const QStyleOption* option, QPainter* painter, bool radio) const;
    void drawItemPanel(const QStyleOption* option, QPainter* painter, const QWidget* widget, bool row) const;
    void drawDownArrow(const QRect& rect, const QColor& color, QPainter* painter) const;

    std::shared_ptr<const Theme> m_theme;
    UiScale m_scale;
    std::shared_ptr<const IconAtlasSet> m_icons;
    std::uint64_t m_generation;
    QPalette m_palette;
};

} // namespace frost::ui
