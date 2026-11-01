// SPDX-License-Identifier: Apache-2.0
//
// SVG preparation and rasterisation for frost-icon-baker (ADR 0015 decision 7): the stroke-width
// normalisation and currentColor rewrite (QXmlStreamReader/Writer, never regex), and rendering
// with QSvgRenderer restricted to SVG Tiny 1.2 features with animations off. Qt Svg is used here
// and nowhere else in FrostIDE (the qt-svg lint rule and licensecheck's icon-baker context).
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/model/icon_atlas_format.hpp"

#include <QByteArray>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace frost::tools::baker {

// The largest SVG source accepted, in bytes.
inline constexpr qsizetype kMaxSvgBytes = 64 * 1024;

/*
 * Rewrites `svg` for baking at `designSize` logical pixels:
 *   - requires a root <svg> element with a square viewBox "minX minY size size" (size > 0);
 *   - refuses <text> elements anywhere, DTDs and entity references;
 *   - with `strokePx`, sets the root's stroke-width to strokePx x viewBox size / designSize
 *     (Lucide's absoluteStrokeWidth formula, ISC: a stroke strokePx device-independent pixels
 *     wide at the design size); the root must declare stroke-width and no child may;
 *   - with `tint`, replaces every "currentColor" attribute value with "#000000", so the mask's
 *     coverage is the alpha channel.
 * Errors: InvalidArgument naming the problem. Pure; O(size).
 */
[[nodiscard]] Result<QByteArray> prepareSvg(const QByteArray& svg, std::optional<double> strokePx,
                                            std::uint8_t designSize, bool tint);

/*
 * Renders `svg` (already prepared) into a devicePx x devicePx cell: a transparent
 * ARGB32-premultiplied image with antialiasing, converted to an Alpha8 mask when `alphaMask`.
 * Uses QtSvg::Tiny12FeaturesOnly | DisableSMILAnimations | DisableCSSAnimations. Any message in a
 * "qt.svg" logging category while loading or rendering is an error, and so is an invalid
 * document. Needs a QGuiApplication; GUI thread only.
 * Errors: InvalidArgument with the renderer's messages.
 */
[[nodiscard]] Result<ui::AtlasCell> rasterizeSvg(const QByteArray& svg, std::uint16_t devicePx, bool alphaMask);

// Installs the Qt message handler that collects "qt.svg" messages for rasterizeSvg (and prints
// every message to stderr). Call once from main() before the first rasterizeSvg.
void installSvgMessageHandler(QStringList* sink) noexcept;

} // namespace frost::tools::baker
