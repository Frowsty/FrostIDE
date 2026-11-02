// SPDX-License-Identifier: Apache-2.0
//
// QtTextShaper (frost/ui/editor/qt_text_shaper.hpp): per-code-point advances of the editor font in
// 1/64 px, with the ASCII table built at construction and a cache for the rest.

#include "frost/ui/editor/qt_text_shaper.hpp"

#include <QString>

#include <algorithm>
#include <cmath>

namespace frost::ui {

text::TextWidth toShaperUnits(double pixels) noexcept {
    if (!(pixels > 0.0)) {
        return 0;
    }
    return static_cast<text::TextWidth>(std::llround(pixels * static_cast<double>(kShaperUnitsPerPixel)));
}

QtTextShaper::QtTextShaper(const QFont& font, std::uint32_t tabSize)
    : m_font(font), m_metrics(font), m_tabSize(std::max<std::uint32_t>(tabSize, 1)) {
    m_cell = std::max<text::TextWidth>(toShaperUnits(m_metrics.horizontalAdvance(QChar(u' '))), 1);
    for (char32_t cp = 0; cp < m_ascii.size(); ++cp) {
        m_ascii[cp] = (cp < 0x20 || cp == 0x7F) ? m_cell : measure(cp);
    }
}

text::TextWidth QtTextShaper::measure(char32_t cp) const noexcept {
    const QString one = QString::fromUcs4(&cp, 1);
    const text::TextWidth measured = toShaperUnits(m_metrics.horizontalAdvance(one));
    if (text::charWidth(cp) == text::CharWidth::Wide) {
        return std::max(measured, 2 * m_cell);
    }
    return measured > 0 ? measured : m_cell;
}

text::TextWidth QtTextShaper::advance(char32_t cp, text::TextWidth x) const noexcept {
    if (cp == U'\t') {
        const text::TextWidth stop = m_cell * static_cast<text::TextWidth>(m_tabSize);
        return stop - (std::max<text::TextWidth>(x, 0) % stop);
    }
    if (cp < m_ascii.size()) {
        return m_ascii[cp];
    }
    if (text::charWidth(cp) == text::CharWidth::Zero) {
        return 0;
    }
    if (const auto it = m_cache.find(cp); it != m_cache.end()) {
        return it->second;
    }
    const text::TextWidth width = measure(cp);
    m_cache.emplace(cp, width);
    return width;
}

} // namespace frost::ui
