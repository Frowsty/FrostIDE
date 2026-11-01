// SPDX-License-Identifier: Apache-2.0
//
// SamplePanel: the 480x320 widget panel the screenshot goldens and the HiDPI cross-check render
// (ADR 0015 decision 11). An IslandHost with a 28 px top band and two islands holding the
// controls FrostStyle draws, plus a StateGallery strip for hover, pressed and focus states that
// a still widget never shows. Reused by m1/shell-frame and m1/editor-view.
#pragma once

#include "frost/ui/island_frame.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QTreeView>
#include <QWidget>

#include <memory>

namespace frost::ui::test {

// The logical size of the panel and its top band.
inline constexpr int kPanelWidth = 480;
inline constexpr int kPanelHeight = 320;
inline constexpr int kTopBand = 28;

/*
 * Draws, left to right in cells of equal width: a push-button panel hovered, pressed and
 * focused, then a line-edit frame focused, through style()->drawPrimitive.
 */
class StateGallery final : public QWidget {
public:
    explicit StateGallery(QWidget* parent = nullptr);
    // The rectangle (logical, gallery coordinates) of cell `index` (0..3).
    [[nodiscard]] QRect cell(int index) const;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// The panel and the widgets the structural samples look at. `root` owns everything.
struct SamplePanel {
    std::unique_ptr<IslandHost> root;
    IslandFrame* left = nullptr;
    IslandFrame* right = nullptr;
    QPushButton* defaultButton = nullptr;
    QLineEdit* text = nullptr;
    QCheckBox* checked = nullptr;
    QRadioButton* checkedRadio = nullptr;
    QTreeView* tree = nullptr;
    QToolButton* disabledTool = nullptr;
    QLabel* label = nullptr;
    QLabel* monoLabel = nullptr;
    StateGallery* gallery = nullptr;
};

/*
 * Builds the panel for the current FrostStyle (colours of the secondary label and icons are
 * taken from it), resizes it to 480x320, shows it and processes events so layout and polish
 * have run. On the offscreen platform it is shown and activated (Active palette group); on any
 * other platform it is WA_DontShowOnScreen and inactive. `monoFamily` is the JetBrains Mono
 * family for the mono label.
 */
[[nodiscard]] SamplePanel makeSamplePanel(const QString& monoFamily);

} // namespace frost::ui::test
