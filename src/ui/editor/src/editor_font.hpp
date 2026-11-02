// SPDX-License-Identifier: Apache-2.0
//
// Shared editor font policy for shaping and both rendering backends (ADR 0043).
#pragma once

#include <QFont>

namespace frost::ui::detail {

[[nodiscard]] inline QFont editorFont(const QFont& input) {
    QFont font = input;
    font.setStyleStrategy(static_cast<QFont::StyleStrategy>(font.styleStrategy() | QFont::NoSubpixelAntialias));
    return font;
}

} // namespace frost::ui::detail
