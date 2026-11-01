// SPDX-License-Identifier: Apache-2.0
//
// FrostIconEngine: the QIconEngine behind every FrostIDE icon. It holds the shared atlas set and
// an icon id, and draws through IconAtlasSet::pixmap with the current FrostStyle's theme and
// density, so a theme switch or compact toggle re-tints and re-sizes existing QIcons
// (ADR 0015 decision 7).
#pragma once

#include "frost/ui/icon_atlas_set.hpp"

#include <QIconEngine>

#include <memory>
#include <optional>
#include <string>

namespace frost::ui {

/*
 * Paints one icon id. The dpr always comes from the painter (the scale of its device transform,
 * which is the target's ratio also under QWidget::render), never from a screen; the icon's
 * origin is snapped to a device pixel. The theme and density come from
 * FrostStyle::current(); without one the icon is tinted opaque black at design size 16.
 * GUI thread only. Budget: B5 (a cache hit is one lookup plus drawPixmap).
 */
class FrostIconEngine final : public QIconEngine {
public:
    FrostIconEngine(std::shared_ptr<const IconAtlasSet> icons, std::string id, IconSlot slot,
                    std::optional<IconRole> roleOverride);

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override;
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override;
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override;
    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override;
    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override;
    [[nodiscard]] QIconEngine* clone() const override;
    [[nodiscard]] QString key() const override;
    bool isNull() override;
    QString iconName() override;

private:
    [[nodiscard]] QPixmap render(int logicalPx, qreal dpr, QIcon::Mode mode) const;

    std::shared_ptr<const IconAtlasSet> m_icons;
    std::string m_id;
    IconSlot m_slot;
    std::optional<IconRole> m_roleOverride;
};

} // namespace frost::ui
