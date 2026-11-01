// SPDX-License-Identifier: Apache-2.0
//
// Screenshot goldens (ADR 0015 decision 11): in-process rendering at an explicit device pixel
// ratio, the comparator, and the golden files under tests/ui/goldens/<os>/.
#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <string>

namespace frost::ui::test {

// "windows" or "linux": goldens are per OS.
[[nodiscard]] QString goldenOs();

// Renders `widget` with QWidget::render into a transparent ARGB32_Premultiplied image of
// size x dpr device pixels with setDevicePixelRatio(dpr).
[[nodiscard]] QImage renderAt(QWidget& widget, qreal dpr);

struct Comparison {
    bool sizesMatch = false;
    int maxChannelDiff = 0; // the largest per-channel difference
    int pixelsOverTolerance = 0;
    [[nodiscard]] bool passes() const;
    [[nodiscard]] std::string describe() const;
};

// Channel differences up to this pass; up to kMaxOutlierPixels pixels may exceed it.
inline constexpr int kChannelTolerance = 2;
inline constexpr int kMaxOutlierPixels = 16;

/*
 * Compares two images as ARGB32 (not premultiplied): the sizes must match; every channel within
 * kChannelTolerance passes; otherwise at most kMaxOutlierPixels pixels may differ by more.
 */
[[nodiscard]] Comparison compareImages(const QImage& actual, const QImage& expected);

/*
 * Checks `actual` against <FROST_SOURCE_DIR>/tests/ui/goldens/<os>/<file>. On a mismatch it
 * writes <build>/ui-screenshots/<os>/<file stem>.{actual,diff}.png and fails the test with a
 * summary. A missing golden fails with "run with FROST_UPDATE_GOLDENS=1". With
 * FROST_UPDATE_GOLDENS=1 a failing golden is rewritten into FROST_GOLDEN_DIR (default: the
 * source tree's goldens/<os>), the "golden-updated" property is recorded, and the test FAILS so
 * update mode can never green a run.
 */
void expectMatchesGolden(const QImage& actual, const QString& file);

// The golden image for `file`, or a null image.
[[nodiscard]] QImage loadGolden(const QString& file);

} // namespace frost::ui::test
