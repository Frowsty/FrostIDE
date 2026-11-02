// SPDX-License-Identifier: Apache-2.0
//
// Small helpers shared by the shell's sources (private to frost::ui_shell): UTF-8 conversion, the
// themed icon lookup that tolerates a missing FrostStyle, and the shell's i18n context.
#pragma once

#include "frost/ui/frost_style.hpp"
#include "frost/ui/tr.hpp"

#include <QIcon>
#include <QString>

#include <string>
#include <string_view>

namespace frost::ui::shell_detail {

// The i18n context of the shell's own strings.
inline constexpr std::string_view kContext = "frost::ui::Shell";
// The i18n context of action names and descriptions from the registry.
inline constexpr std::string_view kActionsContext = "frost::input::actions";

// The shell's translated text for `source`.
[[nodiscard]] inline QString text(std::string_view source) { return qtr(kContext, source); }

[[nodiscard]] inline QString fromUtf8(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] inline std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// The current theme's icon `id`, or a null icon without a FrostStyle (tests, the self-test).
[[nodiscard]] inline QIcon themedIcon(std::string_view id) {
    const FrostStyle* style = FrostStyle::current();
    return style != nullptr ? style->icon(id) : QIcon();
}

} // namespace frost::ui::shell_detail
