// SPDX-License-Identifier: Apache-2.0
//
// FrostStyle (frost_style.hpp): the palette, metrics and the drawing overrides. Written against
// Qt's public QStyle/QProxyStyle documentation only (clean room: no Qt style sources consulted).

#include "frost/ui/frost_style.hpp"

#include "frost/ui/icon_engine.hpp"

#include "device_pixels.hpp"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QTreeView>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace frost::ui {

namespace {

QColor toQColor(Rgba c) { return QColor::fromRgba(c.argb); }

qreal devicePixelRatioOf(const QPainter* painter) { return detail::devicePixelScale(*painter); }

// `devicePx` whole device pixels expressed in logical units (at least one device pixel), so
// lines stay crisp at every scale.
qreal deviceWidth(const QPainter* painter, qreal logical) {
    const qreal dpr = devicePixelRatioOf(painter);
    return std::max<qreal>(1.0, std::round(logical * dpr)) / dpr;
}

// Strokes a rounded rectangle whose outer edge is `rect` snapped to device pixels, `width` wide
// (a whole number of device pixels from deviceWidth), so the stroke covers whole pixels.
void strokeRounded(QPainter* painter, const QRectF& logicalRect, qreal radius, const QColor& color, qreal width) {
    if (color.alpha() == 0 || width <= 0.0) {
        return;
    }
    const QRectF rect = detail::snapToDevicePixels(*painter, logicalRect);
    const qreal half = width / 2.0;
    painter->setPen(QPen(color, width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter->setBrush(Qt::NoBrush);
    const qreal r = std::max<qreal>(0.0, radius - half);
    painter->drawRoundedRect(rect.adjusted(half, half, -half, -half), r, r);
}

// Fills a rounded rectangle whose edges are `rect` snapped to device pixels.
void fillRounded(QPainter* painter, const QRectF& rect, qreal radius, const QColor& color) {
    if (color.alpha() == 0) {
        return;
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRoundedRect(detail::snapToDevicePixels(*painter, rect), radius, radius);
}

constexpr std::array<QPalette::ColorGroup, 3> kGroups{QPalette::Active, QPalette::Inactive, QPalette::Disabled};

// Sets `role` to `color` in the Active and Inactive groups; the Disabled group keeps the
// palette's disabled colours.
void setEnabledGroups(QPalette& palette, QPalette::ColorRole role, const QColor& color) {
    palette.setColor(QPalette::Active, role, color);
    palette.setColor(QPalette::Inactive, role, color);
}

} // namespace

FrostStyle::FrostStyle(std::shared_ptr<const Theme> theme, UiScale scale, std::shared_ptr<const IconAtlasSet> icons,
                       std::uint64_t generation)
    : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))), m_theme(std::move(theme)), m_scale(scale),
      m_icons(std::move(icons)), m_generation(generation) {
    // Budget: B1. One pass over the palette roles.
    m_palette = buildPalette();
    setObjectName(QStringLiteral("frost"));
}

FrostStyle::~FrostStyle() = default;

FrostStyle* FrostStyle::current() noexcept {
    if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) {
        return nullptr;
    }
    return qobject_cast<FrostStyle*>(QApplication::style());
}

QColor FrostStyle::color(ColorKey key) const noexcept { return toQColor(m_theme->color(key)); }

QIcon FrostStyle::icon(std::string_view id, IconSlot slot, std::optional<IconRole> roleOverride) const {
    if (!m_icons || !m_icons->contains(id)) {
        return {};
    }
    return QIcon(new FrostIconEngine(m_icons, std::string(id), slot, roleOverride));
}

QPalette FrostStyle::buildPalette() const {
    QPalette palette;
    const QColor border = color(ColorKey::ComponentBorderColor);
    for (const QPalette::ColorGroup group : kGroups) {
        const auto set = [&](QPalette::ColorRole role, ColorKey key) { palette.setColor(group, role, color(key)); };
        set(QPalette::Window, ColorKey::PanelBackground);
        set(QPalette::WindowText, ColorKey::LabelForeground);
        set(QPalette::Base, ColorKey::TextFieldBackground);
        set(QPalette::AlternateBase, ColorKey::ListHoverBackground);
        set(QPalette::Text, ColorKey::TextFieldForeground);
        set(QPalette::Button, ColorKey::ButtonBackground);
        set(QPalette::ButtonText, ColorKey::ButtonForeground);
        set(QPalette::BrightText, ColorKey::LabelForeground);
        set(QPalette::Highlight, group == QPalette::Inactive ? ColorKey::ListSelectionInactiveBackground
                                                             : ColorKey::ListSelectionBackground);
        set(QPalette::HighlightedText, ColorKey::ListSelectionForeground);
        set(QPalette::Link, ColorKey::LinkForeground);
        set(QPalette::LinkVisited, ColorKey::LinkForeground);
        set(QPalette::PlaceholderText, ColorKey::TextFieldPlaceholderForeground);
        set(QPalette::ToolTipBase, ColorKey::ToolTipBackground);
        set(QPalette::ToolTipText, ColorKey::ToolTipForeground);
        set(QPalette::Accent, ColorKey::ComponentFocusColor);
        // The bevel roles derive from the component border (fixed factors, ADR 0015 decision 1).
        palette.setColor(group, QPalette::Light, border.lighter(150));
        palette.setColor(group, QPalette::Midlight, border.lighter(125));
        palette.setColor(group, QPalette::Mid, border);
        palette.setColor(group, QPalette::Dark, border.darker(150));
        palette.setColor(group, QPalette::Shadow, border.darker(200));
    }
    const QColor disabled = color(ColorKey::LabelDisabledForeground);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return palette;
}

QPalette FrostStyle::standardPalette() const { return m_palette; }

void FrostStyle::polish(QPalette& palette) {
    // Wholesale: the platform theme's palette would otherwise be preferred.
    palette = m_palette;
}

void FrostStyle::polish(QWidget* widget) {
    QProxyStyle::polish(widget);
    if (qobject_cast<QAbstractButton*>(widget) != nullptr || qobject_cast<QComboBox*>(widget) != nullptr ||
        qobject_cast<QTabBar*>(widget) != nullptr) {
        widget->setAttribute(Qt::WA_Hover, true);
    }
    if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
        view->viewport()->setAttribute(Qt::WA_Hover, true);
    }
}

void FrostStyle::unpolish(QWidget* widget) { QProxyStyle::unpolish(widget); }

int FrostStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const {
    switch (metric) {
    case PM_SmallIconSize:
    case PM_ListViewIconSize:
    case PM_TabBarIconSize:
    case PM_ButtonIconSize: return metricPx(MetricKey::IconSize);
    case PM_ToolBarIconSize: return metricPx(MetricKey::ToolbarIconSize);
    case PM_ScrollBarExtent: return metricPx(MetricKey::ScrollBarWidth);
    case PM_DefaultFrameWidth: return 1;
    case PM_FocusFrameHMargin:
    case PM_FocusFrameVMargin: return metricPx(MetricKey::ComponentFocusWidth);
    case PM_ButtonMargin: return metricPx(MetricKey::ComponentPaddingH);
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing: return metricPx(MetricKey::LayoutSpacing);
    case PM_LayoutLeftMargin:
    case PM_LayoutTopMargin:
    case PM_LayoutRightMargin:
    case PM_LayoutBottomMargin: return metricPx(MetricKey::LayoutMargin);
    // The theme has no indicator size; Fusion's follows the IDE zoom like every logical metric.
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight: return m_scale.px(QProxyStyle::pixelMetric(metric, option, widget));
    default: return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QSize FrostStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                   const QWidget* widget) const {
    QSize result = QProxyStyle::sizeFromContents(type, option, size, widget);
    const auto atLeast = [&result](int height) { result.setHeight(std::max(result.height(), height)); };
    switch (type) {
    case CT_PushButton: atLeast(metricPx(MetricKey::ButtonHeight)); break;
    case CT_LineEdit:
    case CT_ComboBox: atLeast(metricPx(MetricKey::TextFieldHeight)); break;
    case CT_ItemViewItem:
        atLeast(metricPx(qobject_cast<const QTreeView*>(widget) != nullptr ? MetricKey::TreeRowHeight
                                                                           : MetricKey::ListRowHeight));
        break;
    case CT_MenuItem:
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
            item == nullptr || item->menuItemType != QStyleOptionMenuItem::Separator) {
            atLeast(metricPx(MetricKey::MenuItemHeight));
        }
        break;
    case CT_TabBarTab: {
        const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
        const bool vertical =
            tab != nullptr && (tab->shape == QTabBar::RoundedWest || tab->shape == QTabBar::RoundedEast ||
                               tab->shape == QTabBar::TriangularWest || tab->shape == QTabBar::TriangularEast);
        if (vertical) {
            result.setWidth(std::max(result.width(), metricPx(MetricKey::TabbedPaneTabHeight)));
        } else {
            atLeast(metricPx(MetricKey::TabbedPaneTabHeight));
        }
        break;
    }
    default: break;
    }
    return result;
}

int FrostStyle::styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                          QStyleHintReturn* returnData) const {
    switch (hint) {
    case SH_Widget_Animation_Duration: return 0; // decision 13: off until M11's reduced-motion work
    case SH_EtchDisabledText: return 0;
    case SH_ItemView_ShowDecorationSelected: return 1;
    default: return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

void FrostStyle::drawFocusRing(const QRectF& rect, qreal radius, QPainter* painter) const {
    const qreal width = deviceWidth(painter, metricPx(MetricKey::ComponentFocusWidth));
    strokeRounded(painter, rect, radius, color(ColorKey::ComponentFocusColor), width);
}

void FrostStyle::drawButtonPanel(const QStyleOption* option, QPainter* painter) const {
    const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
    const bool isDefault = button != nullptr && (button->features & QStyleOptionButton::DefaultButton) != 0;
    const bool enabled = (option->state & State_Enabled) != 0;
    const bool hover = enabled && (option->state & State_MouseOver) != 0;
    const bool pressed = enabled && (option->state & (State_Sunken | State_On)) != 0;
    ColorKey fill = ColorKey::ButtonBackground;
    ColorKey border = ColorKey::ButtonBorderColor;
    if (!enabled) {
        border = ColorKey::ComponentDisabledBorderColor;
    } else if (isDefault) {
        fill = pressed || hover ? ColorKey::ButtonDefaultHoverBackground : ColorKey::ButtonDefaultBackground;
        border = ColorKey::ButtonDefaultBorderColor;
    } else if (pressed) {
        fill = ColorKey::ButtonPressedBackground;
    } else if (hover) {
        fill = ColorKey::ButtonHoverBackground;
    }
    const QRectF rect(option->rect);
    const qreal radius = metricPx(MetricKey::ButtonArc);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    fillRounded(painter, rect, radius, color(fill));
    strokeRounded(painter, rect, radius, color(border), deviceWidth(painter, 1.0));
    if (enabled && (option->state & State_HasFocus) != 0) {
        drawFocusRing(rect, radius, painter);
        if (isDefault) {
            /*
             * A default button's fill is often the focus colour itself (Glacier Light's accent),
             * so the ring alone would leave the focused button looking unfocused. A
             * Button.default.foreground line just inside the ring makes a two-colour indicator
             * (WCAG 2.2 technique C40); its contrast with the fill, resting or hovered, is the
             * label's Text pair (theme_registry.hpp). Both edges stay on device pixels.
             */
            const qreal ring = deviceWidth(painter, metricPx(MetricKey::ComponentFocusWidth));
            const QRectF outer = detail::snapToDevicePixels(*painter, rect);
            strokeRounded(painter, outer.adjusted(ring, ring, -ring, -ring), std::max<qreal>(0.0, radius - ring),
                          color(ColorKey::ButtonDefaultForeground), deviceWidth(painter, 1.0));
        }
    }
    painter->restore();
}

void FrostStyle::drawLineEditFrame(const QStyleOption* option, QPainter* painter, bool fill, ColorKey background,
                                   ColorKey borderKey) const {
    const bool enabled = (option->state & State_Enabled) != 0;
    const bool focus = enabled && (option->state & State_HasFocus) != 0;
    const QRectF rect(option->rect);
    const qreal radius = metricPx(MetricKey::ComponentArc);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (fill) {
        fillRounded(painter, rect, radius, color(background));
    }
    const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
    if (!fill || frame == nullptr || frame->lineWidth > 0) {
        if (focus) {
            drawFocusRing(rect, radius, painter);
        } else {
            strokeRounded(painter, rect, radius, color(enabled ? borderKey : ColorKey::ComponentDisabledBorderColor),
                          deviceWidth(painter, 1.0));
        }
    }
    painter->restore();
}

void FrostStyle::drawCheckIndicator(const QStyleOption* option, QPainter* painter, bool radio) const {
    const bool enabled = (option->state & State_Enabled) != 0;
    const bool on = (option->state & State_On) != 0;
    const bool partial = !radio && (option->state & State_NoChange) != 0;
    const qreal side = std::min(option->rect.width(), option->rect.height());
    const QRectF box(option->rect.x() + (option->rect.width() - side) / 2.0,
                     option->rect.y() + (option->rect.height() - side) / 2.0, side, side);
    const qreal radius = radio ? side / 2.0 : std::min(side / 4.0, m_scale.px(2.0) * 1.0);
    QColor fill = color(on || partial ? ColorKey::CheckBoxCheckedBackground : ColorKey::CheckBoxBackground);
    QColor border = color(on || partial ? ColorKey::CheckBoxCheckedBackground : ColorKey::CheckBoxBorderColor);
    if (!enabled) {
        border = color(ColorKey::ComponentDisabledBorderColor);
        if (on || partial) {
            fill = border;
        }
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    fillRounded(painter, box, radius, fill);
    strokeRounded(painter, box, radius, border, deviceWidth(painter, 1.0));
    const QColor mark = color(ColorKey::CheckBoxCheckmarkColor);
    if (radio && on) {
        const qreal dot = side * 0.22;
        painter->setPen(Qt::NoPen);
        painter->setBrush(mark);
        painter->drawEllipse(box.center(), dot, dot);
    } else if (on || partial) {
        const qreal width = std::max<qreal>(1.5, side / 8.0);
        painter->setPen(QPen(mark, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        const auto at = [&box](qreal x, qreal y) {
            return QPointF(box.x() + x * box.width(), box.y() + y * box.height());
        };
        if (partial) {
            painter->drawLine(at(0.28, 0.5), at(0.72, 0.5));
        } else {
            QPainterPath path(at(0.25, 0.52));
            path.lineTo(at(0.43, 0.70));
            path.lineTo(at(0.76, 0.32));
            painter->drawPath(path);
        }
    }
    painter->restore();
}

void FrostStyle::drawItemPanel(const QStyleOption* option, QPainter* painter, const QWidget* widget, bool row) const {
    const bool tree = qobject_cast<const QTreeView*>(widget) != nullptr;
    const bool enabled = (option->state & State_Enabled) != 0;
    const bool selected = (option->state & State_Selected) != 0;
    const bool active = (option->state & State_Active) != 0;
    const bool hover = enabled && (option->state & State_MouseOver) != 0;
    QColor fill;
    if (selected) {
        if (active) {
            fill = color(tree ? ColorKey::TreeSelectionBackground : ColorKey::ListSelectionBackground);
        } else {
            fill = color(tree ? ColorKey::TreeSelectionInactiveBackground : ColorKey::ListSelectionInactiveBackground);
        }
    } else if (hover) {
        fill = color(tree ? ColorKey::TreeHoverBackground : ColorKey::ListHoverBackground);
    } else if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
               row && item != nullptr && (item->features & QStyleOptionViewItem::Alternate) != 0) {
        fill = option->palette.color(QPalette::AlternateBase);
    }
    if (fill.isValid() && fill.alpha() > 0) {
        painter->fillRect(option->rect, fill);
    }
}

void FrostStyle::drawDownArrow(const QRect& rect, const QColor& color, QPainter* painter) const {
    // A filled triangle 8 x 4 logical pixels times the zoom, centred, snapped to device pixels.
    const qreal width = m_scale.px(8.0);
    const qreal height = m_scale.px(4.0);
    const QPointF topLeft = detail::snapToDevicePixels(
        *painter, QPointF(rect.x() + (rect.width() - width) / 2.0, rect.y() + (rect.height() - height) / 2.0));
    const QPointF points[] = {topLeft, topLeft + QPointF(width, 0.0), topLeft + QPointF(width / 2.0, height)};
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawPolygon(points, 3);
    painter->restore();
}

void FrostStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                                    const QWidget* widget) const {
    const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option);
    if (control != CC_ComboBox || combo == nullptr) {
        QProxyStyle::drawComplexControl(control, option, painter, widget);
        return;
    }
    // A text field's frame and fill in the ComboBox colours (focus ring included), then the arrow.
    if ((combo->subControls & SC_ComboBoxFrame) != 0 && combo->frame) {
        QStyleOptionFrame frame;
        frame.QStyleOption::operator=(*combo);
        frame.lineWidth = 1;
        drawLineEditFrame(&frame, painter, true, ColorKey::ComboBoxBackground, ColorKey::ComboBoxBorderColor);
    }
    if ((combo->subControls & SC_ComboBoxArrow) != 0) {
        const QRect arrow = subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
        const bool enabled = (combo->state & State_Enabled) != 0;
        drawDownArrow(arrow,
                      enabled ? color(ColorKey::ComboBoxForeground)
                              : combo->palette.color(QPalette::Disabled, QPalette::Text),
                      painter);
    }
}

void FrostStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                               const QWidget* widget) const {
    switch (element) {
    case PE_PanelButtonCommand: drawButtonPanel(option, painter); return;
    case PE_PanelLineEdit: drawLineEditFrame(option, painter, true); return;
    case PE_FrameLineEdit: drawLineEditFrame(option, painter, false); return;
    case PE_FrameFocusRect:
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        drawFocusRing(QRectF(option->rect), metricPx(MetricKey::ComponentArc), painter);
        painter->restore();
        return;
    case PE_IndicatorCheckBox: drawCheckIndicator(option, painter, false); return;
    case PE_IndicatorRadioButton: drawCheckIndicator(option, painter, true); return;
    case PE_PanelItemViewItem: drawItemPanel(option, painter, widget, false); return;
    case PE_PanelItemViewRow: drawItemPanel(option, painter, widget, true); return;
    case PE_PanelMenu: painter->fillRect(option->rect, color(ColorKey::MenuBackground)); return;
    case PE_FrameMenu:
    case PE_FrameTabWidget:
        painter->save();
        if (element == PE_FrameTabWidget) {
            painter->fillRect(option->rect, color(ColorKey::PopupBackground));
        }
        strokeRounded(painter, QRectF(option->rect), 0.0, color(ColorKey::PopupBorderColor), deviceWidth(painter, 1.0));
        painter->restore();
        return;
    default: QProxyStyle::drawPrimitive(element, option, painter, widget); return;
    }
}

void FrostStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                             const QWidget* widget) const {
    if (element == CE_PushButtonLabel) {
        // An enabled default button's label uses Button.default.foreground (its fill is
        // Button.default.background, not the palette's Button), drawn by Fusion.
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
        if (button != nullptr && (button->features & QStyleOptionButton::DefaultButton) != 0 &&
            (button->state & State_Enabled) != 0) {
            QStyleOptionButton copy(*button);
            copy.palette.setColor(QPalette::ButtonText, color(ColorKey::ButtonDefaultForeground));
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
    }
    if (element == CE_MenuItem) {
        // Menus take Menu.* rather than the palette's Text and Highlight roles (which are the
        // TextField and List keys); Fusion draws the item with them.
        if (const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option)) {
            QStyleOptionMenuItem copy(*item);
            for (const QPalette::ColorRole role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) {
                setEnabledGroups(copy.palette, role, color(ColorKey::MenuForeground));
            }
            setEnabledGroups(copy.palette, QPalette::Highlight, color(ColorKey::MenuSelectionBackground));
            setEnabledGroups(copy.palette, QPalette::HighlightedText, color(ColorKey::MenuSelectionForeground));
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
    }
    if (element == CE_ComboBoxLabel) {
        if (const auto* combo = qstyleoption_cast<const QStyleOptionComboBox*>(option)) {
            QStyleOptionComboBox copy(*combo);
            setEnabledGroups(copy.palette, QPalette::Text, color(ColorKey::ComboBoxForeground));
            setEnabledGroups(copy.palette, QPalette::ButtonText, color(ColorKey::ComboBoxForeground));
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
    }
    if (element == CE_ItemViewItem && qobject_cast<const QTreeView*>(widget) != nullptr) {
        /*
         * A selected tree row's text is Tree.selectionForeground (its fill is
         * Tree.selectionBackground, drawItemPanel). Only the style's own HighlightedText is
         * replaced, so a colour that the model or the widget set explicitly is kept.
         */
        if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
            QStyleOptionViewItem copy(*item);
            for (const QPalette::ColorGroup group : kGroups) {
                if (copy.palette.color(group, QPalette::HighlightedText) ==
                    m_palette.color(group, QPalette::HighlightedText)) {
                    copy.palette.setColor(group, QPalette::HighlightedText, color(ColorKey::TreeSelectionForeground));
                }
            }
            QProxyStyle::drawControl(element, &copy, painter, widget);
            return;
        }
    }
    if (element == CE_ToolButtonLabel) {
        /*
         * An icon-only tool button paints its QIcon directly, so the icon engine sees the
         * painter's real scale (Fusion asks for a pixmap at the widget's ratio, which is wrong
         * under QWidget::render) and snaps the icon to device pixels.
         */
        const auto* tool = qstyleoption_cast<const QStyleOptionToolButton*>(option);
        if (tool != nullptr && tool->toolButtonStyle == Qt::ToolButtonIconOnly && !tool->icon.isNull()) {
            const bool enabled = (tool->state & State_Enabled) != 0;
            const bool raisedHover = (tool->state & State_AutoRaise) != 0 && (tool->state & State_MouseOver) != 0;
            const QIcon::Mode mode = !enabled ? QIcon::Disabled : raisedHover ? QIcon::Active : QIcon::Normal;
            const QIcon::State state = (tool->state & State_On) != 0 ? QIcon::On : QIcon::Off;
            const QRect target = QStyle::alignedRect(tool->direction, Qt::AlignCenter, tool->iconSize, tool->rect);
            tool->icon.paint(painter, target, Qt::AlignCenter, mode, state);
            return;
        }
    }
    if (element != CE_TabBarTabShape) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const auto* tab = qstyleoption_cast<const QStyleOptionTab*>(option);
    if (tab == nullptr) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const bool selected = (option->state & State_Selected) != 0;
    const bool hover = (option->state & State_Enabled) != 0 && (option->state & State_MouseOver) != 0;
    const QRectF rect(option->rect);
    painter->save();
    if (hover && !selected) {
        painter->fillRect(rect, color(ColorKey::TabbedPaneHoverBackground));
    }
    if (selected) {
        // A 2 px accent underline on the edge facing the page.
        const qreal t = deviceWidth(painter, m_scale.px(2.0));
        QRectF line;
        switch (tab->shape) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth: line = QRectF(rect.left(), rect.top(), rect.width(), t); break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest: line = QRectF(rect.right() - t, rect.top(), t, rect.height()); break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast: line = QRectF(rect.left(), rect.top(), t, rect.height()); break;
        default: line = QRectF(rect.left(), rect.bottom() - t, rect.width(), t); break;
        }
        painter->fillRect(line, color(ColorKey::TabbedPaneUnderlineColor));
    }
    painter->restore();
}

} // namespace frost::ui
