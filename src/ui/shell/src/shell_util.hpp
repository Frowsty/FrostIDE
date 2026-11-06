// SPDX-License-Identifier: Apache-2.0
//
// Small helpers shared by the shell's sources (private to frost::ui_shell): UTF-8 conversion, the
// themed icon lookup that tolerates a missing FrostStyle, and the shell's i18n context.
#pragma once

#include "frost/ui/frost_style.hpp"
#include "frost/ui/tr.hpp"

#include <QColor>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QString>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace frost::ui::shell_detail {

// The i18n context of the shell's own strings.
inline constexpr std::string_view kContext = "frost::ui::Shell";
// The i18n context of action names and descriptions from the registry.
inline constexpr std::string_view kActionsContext = "frost::input::actions";

// The shell's translated text for `source`.
[[nodiscard]] inline QString text(std::string_view source) { return qtr(kContext, source); }

[[nodiscard]] inline QString fromUtf8(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] inline std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// The current theme's icon `id`, or a null icon without a FrostStyle (tests, the self-test).
[[nodiscard]] inline QIcon themedIcon(std::string_view id, std::optional<IconRole> roleOverride = std::nullopt) {
    const FrostStyle* style = FrostStyle::current();
    return style != nullptr ? style->icon(id, IconSlot::Small, roleOverride) : QIcon();
}

// Deterministic JetBrains Rider project avatar palette color.
[[nodiscard]] inline QColor projectAvatarColor(const QString& name) {
    static constexpr std::array<QRgb, 6> kAvatarColors = {
        0xFF3574F0U, // Rider Blue
        0xFF7C4DFFU, // Deep Violet
        0xFF009688U, // Teal Emerald
        0xFFE0565BU, // Coral Red
        0xFFD97724U, // Warm Amber
        0xFF268BD2U, // Ocean Blue
    };
    unsigned int hash = 2166136261U;
    const QByteArray utf8 = name.toUtf8();
    for (char c : utf8) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619U;
    }
    return QColor::fromRgba(kAvatarColors[hash % kAvatarColors.size()]);
}

// Returns the active project's avatar color from the window's MainToolbar project button.
[[nodiscard]] inline QColor activeProjectColor(const QWidget* context) {
    if (context != nullptr) {
        if (const QWidget* top = context->window()) {
            if (const auto* projBtn = top->findChild<QToolButton*>(QStringLiteral("toolbar.project"))) {
                return projectAvatarColor(projBtn->text());
            }
        }
    }
    return QColor(0x35, 0x74, 0xF0);
}

// Paints a smooth, multi-stop elliptical radial color spill into a frosted glass surface.
inline void paintEllipticalGlow(QPainter& painter, QPointF center, qreal radiusX, qreal radiusY, QColor color,
                                int peakAlpha) {
    if (radiusX <= 1.0 || radiusY <= 1.0 || peakAlpha <= 0) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(center);
    painter.scale(radiusX, radiusY);

    QRadialGradient grad(QPointF(0.0, 0.0), 1.0);
    const auto withAlpha = [&](qreal factor) {
        QColor c = color;
        c.setAlpha(std::clamp(static_cast<int>(peakAlpha * factor), 0, 255));
        return c;
    };
    grad.setColorAt(0.00, withAlpha(1.00));
    grad.setColorAt(0.22, withAlpha(0.82));
    grad.setColorAt(0.45, withAlpha(0.50));
    grad.setColorAt(0.68, withAlpha(0.22));
    grad.setColorAt(0.86, withAlpha(0.06));
    grad.setColorAt(1.00, withAlpha(0.00));

    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawRect(QRectF(-1.0, -1.0, 2.0, 2.0));
    painter.restore();
}

// Paints a subtle vertical frosted-glass depth sheen and optional 1px top specular highlight.
inline void paintFrostedGlassSheen(QPainter& painter, const QRect& rect, bool isDark, bool topHighlight = true) {
    if (rect.width() <= 0 || rect.height() <= 0) {
        return;
    }
    QLinearGradient sheen(rect.topLeft(), rect.bottomLeft());
    if (isDark) {
        sheen.setColorAt(0.0, QColor(255, 255, 255, 8));
        sheen.setColorAt(0.5, QColor(255, 255, 255, 2));
        sheen.setColorAt(1.0, QColor(0, 0, 0, 10));
    } else {
        sheen.setColorAt(0.0, QColor(255, 255, 255, 28));
        sheen.setColorAt(0.6, QColor(255, 255, 255, 6));
        sheen.setColorAt(1.0, QColor(0, 0, 0, 6));
    }
    painter.fillRect(rect, sheen);
    if (topHighlight) {
        painter.fillRect(QRect(rect.left(), rect.top(), rect.width(), 1),
                         isDark ? QColor(255, 255, 255, 14) : QColor(255, 255, 255, 60));
    }
}

} // namespace frost::ui::shell_detail
