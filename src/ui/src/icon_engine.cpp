// SPDX-License-Identifier: Apache-2.0
//
// FrostIconEngine (icon_engine.hpp).

#include "frost/ui/icon_engine.hpp"

#include "frost/ui/frost_style.hpp"

#include "device_pixels.hpp"
#include <QPaintDevice>
#include <QPainter>

#include <algorithm>
#include <utility>

namespace frost::ui {

FrostIconEngine::FrostIconEngine(std::shared_ptr<const IconAtlasSet> icons, std::string id, IconSlot slot,
                                 std::optional<IconRole> roleOverride)
    : m_icons(std::move(icons)), m_id(std::move(id)), m_slot(slot), m_roleOverride(roleOverride) {}

QPixmap FrostIconEngine::render(int logicalPx, qreal dpr, QIcon::Mode mode) const {
    if (!m_icons) {
        return {};
    }
    const FrostStyle* style = FrostStyle::current();
    const Theme* theme = style != nullptr ? &style->theme() : nullptr;
    const Density density = style != nullptr ? style->scale().density : Density::Normal;
    // Active draws like Normal (QIcon's documented fallback).
    const QIcon::Mode effective = mode == QIcon::Active ? QIcon::Normal : mode;
    return m_icons->pixmap(m_id, m_slot, logicalPx, dpr, effective, theme, density, m_roleOverride);
}

void FrostIconEngine::paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) {
    // Budget: B5.
    const qreal dpr = detail::devicePixelScale(*painter);
    const int logicalPx = std::min(rect.width(), rect.height());
    const QPixmap pm = render(logicalPx, dpr, mode);
    if (pm.isNull()) {
        return;
    }
    // Centred in `rect` with its origin on a device pixel, so every pixmap pixel lands on one
    // device pixel; the pixmap carries its dpr, so it covers logicalPx logical pixels.
    const QPointF origin = detail::snapToDevicePixels(
        *painter, QPointF(rect.x() + (rect.width() - logicalPx) / 2.0, rect.y() + (rect.height() - logicalPx) / 2.0));
    painter->drawPixmap(origin, pm);
}

QPixmap FrostIconEngine::pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) {
    return scaledPixmap(size, mode, state, 1.0);
}

QPixmap FrostIconEngine::scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) {
    return render(std::min(size.width(), size.height()), scale, mode);
}

QSize FrostIconEngine::actualSize(const QSize& size, QIcon::Mode, QIcon::State) {
    const int side = std::min(size.width(), size.height());
    return {side, side};
}

QList<QSize> FrostIconEngine::availableSizes(QIcon::Mode, QIcon::State) {
    QList<QSize> sizes;
    if (m_icons) {
        for (const std::uint8_t size : m_icons->designSizesOf(m_id)) {
            sizes.push_back(QSize(size, size));
        }
    }
    return sizes;
}

QIconEngine* FrostIconEngine::clone() const { return new FrostIconEngine(m_icons, m_id, m_slot, m_roleOverride); }

QString FrostIconEngine::key() const { return QStringLiteral("frost"); }

bool FrostIconEngine::isNull() { return !m_icons || !m_icons->contains(m_id); }

QString FrostIconEngine::iconName() { return QString::fromUtf8(m_id.data(), static_cast<qsizetype>(m_id.size())); }

} // namespace frost::ui
