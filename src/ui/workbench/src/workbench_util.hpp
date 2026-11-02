// SPDX-License-Identifier: Apache-2.0
//
// Small helpers shared by the workbench's sources (private to frost::ui_workbench): UTF-8
// conversion, native path display, the themed icon lookup that tolerates a missing FrostStyle, and
// the workbench's i18n context.
#pragma once

#include "frost/ui/frost_style.hpp"
#include "frost/ui/tr.hpp"

#include <QDir>
#include <QIcon>
#include <QString>

#include <string>
#include <string_view>

namespace frost::ui::workbench_detail {

// The i18n context of the workbench's own strings.
inline constexpr std::string_view kContext = "frost::ui::Workbench";

// The workbench's translated text for `source`.
[[nodiscard]] inline QString text(std::string_view source) { return qtr(kContext, source); }

[[nodiscard]] inline QString fromUtf8(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] inline std::string toUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// `path` (UTF-8) with the platform's separators, for display.
[[nodiscard]] inline QString nativePath(std::string_view path) { return QDir::toNativeSeparators(fromUtf8(path)); }

// The current theme's icon `id`, or a null icon without a FrostStyle (tests, the self-test).
[[nodiscard]] inline QIcon themedIcon(std::string_view id) {
    const FrostStyle* style = FrostStyle::current();
    return style != nullptr ? style->icon(id) : QIcon();
}

// The icon of a file in the trees and popups: code files get nodes.file-code.
[[nodiscard]] QIcon fileIcon(std::string_view name);

} // namespace frost::ui::workbench_detail
