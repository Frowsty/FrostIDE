// SPDX-License-Identifier: Apache-2.0
//
// The Qt wrapper of frost::i18n (frost/ui/tr.hpp).

#include "frost/ui/tr.hpp"

#include "frost/core/i18n.hpp"

#include <string>

namespace frost::ui {

QString qtr(std::string_view context, std::string_view source) {
    const std::string text = i18n::tr(context, source);
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString escapeMnemonics(const QString& text) {
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return escaped;
}

} // namespace frost::ui
