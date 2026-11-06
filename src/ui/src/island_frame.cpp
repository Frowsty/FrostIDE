// SPDX-License-Identifier: Apache-2.0
//
// IslandHost and IslandFrame (island_frame.hpp).

#include "frost/ui/island_frame.hpp"

#include "frost/ui/frost_style.hpp"

#include "device_pixels.hpp"
#include <QEvent>
#include <QPaintDevice>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace frost::ui {

IslandHost::IslandHost(QBoxLayout::Direction direction, QWidget* parent)
    : QWidget(parent), m_layout(new QBoxLayout(direction, this)) {
    applyMetrics();
}

void IslandHost::addIsland(QWidget* island, int stretch) { m_layout->addWidget(island, stretch); }

void IslandHost::applyMetrics() {
    if (objectName() == QStringLiteral("shellCentral")) {
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
        return;
    }
    const FrostStyle* style = FrostStyle::current();
    const int gap = style != nullptr ? style->metricPx(MetricKey::IslandGap) : 0;
    m_layout->setContentsMargins(gap, gap, gap, gap);
    m_layout->setSpacing(gap);
}

void IslandHost::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const FrostStyle* style = FrostStyle::current();
    painter.fillRect(rect(), style != nullptr ? style->color(ColorKey::MainWindowBackground)
                                              : palette().color(QPalette::Window));
}

void IslandHost::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange) {
        applyMetrics();
        update();
    }
    QWidget::changeEvent(event);
}

IslandFrame::IslandFrame(QWidget* parent) : QFrame(parent) { setFrameShape(QFrame::NoFrame); }

void IslandFrame::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const FrostStyle* style = FrostStyle::current();
    if (style == nullptr) {
        painter.fillRect(rect(), palette().color(QPalette::Window));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal radius = style->metricPx(MetricKey::IslandArc);
    const QRectF bounds = detail::snapToDevicePixels(painter, QRectF(rect()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(style->color(ColorKey::IslandBackground));
    painter.drawRoundedRect(bounds, radius, radius);
    QColor border = style->color(ColorKey::IslandBorderColor);
    if (border.alpha() == 0) {
        return;
    }
    if (!isActiveWindow()) {
        border.setAlphaF(border.alphaF() * style->theme().metric(MetricKey::WindowInactiveAlpha));
    }
    const qreal dpr = detail::devicePixelScale(painter);
    const qreal width = std::max<qreal>(1.0, std::round(dpr)) / dpr;
    painter.setPen(QPen(border, width));
    painter.setBrush(Qt::NoBrush);
    const qreal half = width / 2.0;
    painter.drawRoundedRect(bounds.adjusted(half, half, -half, -half), std::max<qreal>(0.0, radius - half),
                            std::max<qreal>(0.0, radius - half));
}

void IslandFrame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange || event->type() == QEvent::StyleChange) {
        update();
    }
    QFrame::changeEvent(event);
}

} // namespace frost::ui
