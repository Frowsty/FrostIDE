// SPDX-License-Identifier: Apache-2.0
//
// Helpers private to frost::ui_find: the i18n context, UTF-8 conversion, the item roles and the
// delegate that paints a result row (the line with its match highlighted, the file and line on the
// right), shared by the Find in Files popup and the Find tool window.
#pragma once

#include "frost/search/find_in_files.hpp"
#include "frost/ui/find/find_item_roles.hpp"
#include "frost/ui/frost_style.hpp"
#include "frost/ui/tr.hpp"

#include <QColor>
#include <QIcon>
#include <QString>
#include <QStyledItemDelegate>

#include <string>
#include <string_view>

namespace frost::ui::find_detail {

// The i18n context of the find feature's strings.
inline constexpr std::string_view kContext = "frost::ui::Find";

// A translated UI string of the feature (not `text`, which would hide frost::text in this code).
[[nodiscard]] inline QString label(std::string_view source) { return qtr(kContext, source); }

[[nodiscard]] inline QString fromUtf8(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] inline std::string toUtf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// The theme's icon `id` (frost/ui/icon_ids.hpp), or an empty icon without a FrostStyle.
[[nodiscard]] inline QIcon themedIcon(std::string_view id) {
    const FrostStyle* style = FrostStyle::current();
    return style != nullptr ? style->icon(id) : QIcon();
}

// "1 match" / "N matches" and "1 file" / "N files", translated, with the locale's digits.
[[nodiscard]] QString matchesText(std::size_t count);
[[nodiscard]] QString filesText(std::size_t count);

// The item roles under their short names (frost/ui/find/find_item_roles.hpp).
inline constexpr int kFileIndexRole = kFindFileIndexRole;
inline constexpr int kMatchIndexRole = kFindMatchIndexRole;
inline constexpr int kHighlightStartRole = kFindHighlightStartRole;
inline constexpr int kHighlightLengthRole = kFindHighlightLengthRole;
inline constexpr int kDetailRole = kFindDetailRole;

// The display text of a match row (the preview line without its leading whitespace) and the
// match's UTF-16 range in it.
struct RowText {
    QString text;
    int highlightStart = 0;
    int highlightLength = 0;
};
[[nodiscard]] RowText rowText(const search::FindMatch& match);

// The background of a highlighted match: the theme's link colour, translucent, or amber without a
// FrostStyle.
[[nodiscard]] QColor matchBackground();
// The secondary text colour (the detail column).
[[nodiscard]] QColor secondaryForeground(const QPalette& palette);

// Paints rows with the roles above: the usual item background, selection and check box, the text
// with [highlightStart, +highlightLength) on matchBackground() in bold, and the detail on the right.
class MatchItemDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

} // namespace frost::ui::find_detail
