// SPDX-License-Identifier: Apache-2.0
//
// The result-row helpers of frost::ui_find (find_util.hpp): row text, highlight colours and the
// delegate that paints the highlighted match.

#include "find_util.hpp"

#include "frost/ui/frost_style.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QLocale>
#include <QPainter>
#include <QStyle>

#include <algorithm>

namespace frost::ui::find_detail {

namespace {

// The UTF-16 length of `utf8`.
int utf16Length(std::string_view utf8) { return static_cast<int>(fromUtf8(utf8).size()); }

constexpr int kDetailGap = 16;

} // namespace

QString matchesText(std::size_t count) {
    return count == 1 ? label("1 match") : label("%1 matches").arg(QLocale().toString(static_cast<qulonglong>(count)));
}

QString filesText(std::size_t count) {
    return count == 1 ? label("1 file") : label("%1 files").arg(QLocale().toString(static_cast<qulonglong>(count)));
}

RowText rowText(const search::FindMatch& match) {
    const std::string_view line = match.lineText;
    const std::size_t column = std::min<std::size_t>(match.previewColumn, line.size());
    const std::size_t length = std::min<std::size_t>(match.previewLength, line.size() - column);
    // Leading whitespace is dropped, but never past the match.
    std::size_t lead = 0;
    while (lead < column && (line[lead] == ' ' || line[lead] == '\t')) {
        ++lead;
    }
    RowText row;
    QString prefix;
    if (match.previewStart > 0) {
        prefix = QStringLiteral("…"); // The line was cut before the preview.
    }
    row.text = prefix + fromUtf8(line.substr(lead));
    row.highlightStart = static_cast<int>(prefix.size()) + utf16Length(line.substr(lead, column - lead));
    row.highlightLength = utf16Length(line.substr(column, length));
    return row;
}

QColor matchBackground() {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        QColor color = style->color(ColorKey::LinkForeground);
        color.setAlpha(80);
        return color;
    }
    return QColor(255, 200, 0, 110);
}

QColor secondaryForeground(const QPalette& palette) {
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        return style->color(ColorKey::LabelSecondaryForeground);
    }
    return palette.color(QPalette::Disabled, QPalette::Text);
}

void MatchItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    const QString full = item.text;
    item.text.clear();
    const QWidget* widget = item.widget;
    QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    // Background, selection, check box and icon; the text is drawn below.
    style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

    QRect area = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget).adjusted(2, 0, -4, 0);
    const bool selected = (item.state & QStyle::State_Selected) != 0;
    const QColor foreground = item.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    painter->save();
    painter->setClipRect(area);
    painter->setFont(item.font);
    const QFontMetrics metrics(item.font);

    // The detail, right-aligned, takes up to 40% of the row.
    const QString detail = index.data(kDetailRole).toString();
    if (!detail.isEmpty()) {
        const QString shown = metrics.elidedText(detail, Qt::ElideMiddle, area.width() * 2 / 5);
        const int width = metrics.horizontalAdvance(shown);
        painter->setPen(selected ? foreground : secondaryForeground(item.palette));
        painter->drawText(QRect(area.right() - width, area.top(), width, area.height()), Qt::AlignVCenter, shown);
        area.setRight(area.right() - width - kDetailGap);
    }

    const int start = std::clamp(index.data(kHighlightStartRole).toInt(), 0, static_cast<int>(full.size()));
    const int length = std::clamp(index.data(kHighlightLengthRole).toInt(), 0, static_cast<int>(full.size()) - start);
    painter->setPen(foreground);
    int x = area.left();
    const auto draw = [&](const QString& piece, const QFont& font, bool highlight) {
        if (piece.isEmpty() || x >= area.right()) {
            return;
        }
        const QFontMetrics pieceMetrics(font);
        const QString shown = pieceMetrics.elidedText(piece, Qt::ElideRight, area.right() - x);
        const int width = pieceMetrics.horizontalAdvance(shown);
        const QRect box(x, area.top(), width, area.height());
        if (highlight) {
            painter->fillRect(box.adjusted(0, 2, 0, -2), matchBackground());
        }
        painter->setFont(font);
        painter->drawText(box, Qt::AlignVCenter, shown);
        x += width;
    };
    QFont bold = item.font;
    bold.setBold(true);
    draw(full.left(start), item.font, false);
    draw(full.mid(start, length), length > 0 ? bold : item.font, length > 0);
    draw(full.mid(start + length), item.font, false);
    painter->restore();
}

} // namespace frost::ui::find_detail
