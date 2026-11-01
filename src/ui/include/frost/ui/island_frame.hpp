// SPDX-License-Identifier: Apache-2.0
//
// The two island widgets m1/shell-frame builds the main window from (PLAN.md section 2,
// "Theming": rounded islands on a window background that stays visible between them).
#pragma once

#include <QBoxLayout>
#include <QFrame>
#include <QWidget>

namespace frost::ui {

/*
 * Paints MainWindow.background and lays out its islands in a box layout whose margins and
 * spacing are Island.gap (at the current density and zoom). Re-reads the metric on every style
 * change. Colours and metrics come from FrostStyle::current(); without one it paints the
 * palette's Window colour with no gap. GUI thread only.
 */
class IslandHost : public QWidget {
    Q_OBJECT

public:
    explicit IslandHost(QBoxLayout::Direction direction = QBoxLayout::LeftToRight, QWidget* parent = nullptr);

    // The layout the islands go into (owned by this widget).
    [[nodiscard]] QBoxLayout* islandLayout() const noexcept { return m_layout; }
    // Adds `island` with `stretch` (QBoxLayout::addWidget).
    void addIsland(QWidget* island, int stretch = 0);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void applyMetrics();

    QBoxLayout* m_layout;
};

/*
 * A rounded rectangle of Island.background with an Island.arc radius and, when its alpha is not
 * zero, an Island.borderColor hairline. While the window is inactive the border's alpha is
 * multiplied by Window.inactiveAlpha (repainted on QEvent::ActivationChange). Put a layout or
 * children inside as with any QFrame. GUI thread only.
 */
class IslandFrame : public QFrame {
    Q_OBJECT

public:
    explicit IslandFrame(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
};

} // namespace frost::ui
