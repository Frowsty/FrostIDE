// SPDX-License-Identifier: Apache-2.0
//
// The language-feature widgets (frost/ui/lang/lang_widgets.hpp): kind badges, Markdown to rich
// text, the hint popup, the completion lookup with its documentation panel, parameter info and
// quick doc. The panels (find usages, structure) are in lang_widgets_panels.cpp and the symbol,
// rename, choice, quick definition and hierarchy popups in lang_widgets_popups.cpp (FND-12a).

#include "frost/ui/lang/lang_widgets.hpp"

#include "lang_widgets_internal.hpp"
#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

constexpr int kMaxCompletionRows = 12;
constexpr int kCompletionWidth = 460;
constexpr int kDocumentationWidth = 340;

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// Kind groups share a badge colour; mid-saturation hues that read on light and dark themes.
enum class Hue : std::uint8_t { Type, Callable, Member, Value, Enum, Other, Snippet };

QColor colorOf(Hue hue) {
    switch (hue) {
    case Hue::Type: return QColor(0x3b, 0x82, 0xc4);
    case Hue::Callable: return QColor(0x9a, 0x5c, 0xc6);
    case Hue::Member: return QColor(0xc8, 0x78, 0x2a);
    case Hue::Value: return QColor(0x2a, 0x9a, 0x8c);
    case Hue::Enum: return QColor(0xb0, 0x8f, 0x1e);
    case Hue::Snippet: return QColor(0x4c, 0x9a, 0x3c);
    case Hue::Other: break;
    }
    return QColor(0x80, 0x80, 0x80);
}

QIcon badge(QChar letter, Hue hue) {
    constexpr int kSize = 16;
    constexpr qreal kDpr = 2.0;
    QPixmap pixmap(static_cast<int>(kSize * kDpr), static_cast<int>(kSize * kDpr));
    pixmap.setDevicePixelRatio(kDpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colorOf(hue));
    painter.drawEllipse(QRectF(1, 1, kSize - 2, kSize - 2));
    QFont font = QApplication::font();
    font.setPixelSize(10);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(0, 0, kSize, kSize), Qt::AlignCenter, QString(letter));
    painter.end();
    return QIcon(pixmap);
}

// Delegate for completion rows: icon and label on the left, the detail right-aligned in the
// placeholder colour, deprecated items struck out.
class CompletionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString detail = index.data(kDetailRole).toString();
        opt.text.clear();
        QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        const QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget).adjusted(2, 0, -4, 0);
        const bool selected = (opt.state & QStyle::State_Selected) != 0;
        painter->save();
        const QFontMetrics metrics(opt.font);
        const QString label = index.data(Qt::DisplayRole).toString();
        const int labelWidth = std::min(metrics.horizontalAdvance(label), text.width());
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(QRect(text.left(), text.top(), labelWidth, text.height()), Qt::AlignVCenter | Qt::AlignLeft,
                          metrics.elidedText(label, Qt::ElideRight, labelWidth));
        const int room = text.width() - labelWidth - 16;
        if (!detail.isEmpty() && room > 24) {
            painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
            painter->drawText(QRect(text.right() - room, text.top(), room, text.height()),
                              Qt::AlignVCenter | Qt::AlignRight, metrics.elidedText(detail, Qt::ElideLeft, room));
        }
        painter->restore();
    }
};

} // namespace

QIcon kindIcon(fl::CompletionItemKind kind) {
    using K = fl::CompletionItemKind;
    switch (kind) {
    case K::Method: return badge(u'm', Hue::Callable);
    case K::Function: return badge(u'f', Hue::Callable);
    case K::Constructor: return badge(u'c', Hue::Callable);
    case K::Field: return badge(u'F', Hue::Member);
    case K::Property: return badge(u'P', Hue::Member);
    case K::Variable: return badge(u'v', Hue::Value);
    case K::Value: return badge(u'V', Hue::Value);
    case K::Constant: return badge(u'K', Hue::Value);
    case K::Class: return badge(u'C', Hue::Type);
    case K::Struct: return badge(u'S', Hue::Type);
    case K::Interface: return badge(u'I', Hue::Type);
    case K::TypeParameter: return badge(u'T', Hue::Type);
    case K::Enum: return badge(u'E', Hue::Enum);
    case K::EnumMember: return badge(u'e', Hue::Enum);
    case K::Snippet: return badge(u's', Hue::Snippet);
    case K::Keyword: return badge(u'k', Hue::Other);
    case K::Module: return badge(u'N', Hue::Other);
    case K::File:
    case K::Folder: return badge(u'D', Hue::Other);
    case K::Operator: return badge(u'o', Hue::Other);
    case K::Text:
    case K::Unit:
    case K::Color:
    case K::Reference:
    case K::Event: break;
    }
    return badge(u't', Hue::Other);
}

QIcon kindIcon(fl::SymbolKind kind) {
    using K = fl::SymbolKind;
    switch (kind) {
    case K::Method: return badge(u'm', Hue::Callable);
    case K::Function: return badge(u'f', Hue::Callable);
    case K::Constructor: return badge(u'c', Hue::Callable);
    case K::Operator: return badge(u'o', Hue::Callable);
    case K::Field: return badge(u'F', Hue::Member);
    case K::Property: return badge(u'P', Hue::Member);
    case K::Variable: return badge(u'v', Hue::Value);
    case K::Constant: return badge(u'K', Hue::Value);
    case K::Class: return badge(u'C', Hue::Type);
    case K::Struct: return badge(u'S', Hue::Type);
    case K::Interface: return badge(u'I', Hue::Type);
    case K::TypeParameter: return badge(u'T', Hue::Type);
    case K::Enum: return badge(u'E', Hue::Enum);
    case K::EnumMember: return badge(u'e', Hue::Enum);
    case K::Namespace:
    case K::Module:
    case K::Package: return badge(u'N', Hue::Other);
    case K::File: return badge(u'D', Hue::Other);
    case K::String:
    case K::Number:
    case K::Boolean:
    case K::Array:
    case K::Object:
    case K::Key:
    case K::Null:
    case K::Event: break;
    }
    return badge(u'?', Hue::Other);
}

QString markupToHtml(const fl::Markup& markup) {
    const QString text = qs(markup.text);
    if (markup.markdown) {
#if QT_CONFIG(textmarkdownreader)
        QTextDocument document;
        document.setMarkdown(text);
        return document.toHtml();
#endif
    }
    return QStringLiteral("<p style=\"white-space:pre-wrap\">%1</p>").arg(text.toHtmlEscaped());
}

// --- HintPopup ---

HintPopup::HintPopup(QWidget* parent) : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::Box);
}

void HintPopup::showAt(const QRect& anchor, bool above) {
    adjustSize();
    placeNear(this, anchor, above);
    show();
    raise();
}

// --- CompletionPopup ---

CompletionPopup::CompletionPopup(QWidget* parent) : HintPopup(parent) {
    setObjectName(QStringLiteral("completionPopup"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(1);
    auto* leftPane = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    m_list = new QListWidget(leftPane);
    m_list->setObjectName(QStringLiteral("completionList"));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setItemDelegate(new CompletionDelegate(m_list));
    m_list->setFixedWidth(kCompletionWidth);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* footer = new QLabel(QStringLiteral("Enter to insert  •  Tab to replace  •  Esc to close"), leftPane);
    footer->setObjectName(QStringLiteral("completionFooter"));
    footer->setContentsMargins(8, 3, 8, 3);
    QFont footerFont = footer->font();
    if (footerFont.pointSizeF() > 8.0) {
        footerFont.setPointSizeF(footerFont.pointSizeF() * 0.86);
    }
    footer->setFont(footerFont);
    leftLayout->addWidget(m_list);
    leftLayout->addWidget(footer);
    m_documentation = new QTextBrowser(this);
    m_documentation->setObjectName(QStringLiteral("completionDocumentation"));
    m_documentation->setFocusPolicy(Qt::NoFocus);
    m_documentation->setFrameShape(QFrame::NoFrame);
    m_documentation->setFixedWidth(kDocumentationWidth);
    m_documentation->hide();
    layout->addWidget(leftPane);
    layout->addWidget(m_documentation, 0, Qt::AlignTop);
    // A click chooses the row; a double-click commits it.
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && m_model.selected() != static_cast<std::size_t>(row)) {
            m_model.select(static_cast<std::size_t>(row));
            updateDocumentation();
            if (auto item = m_model.selectedItemIndex()) {
                emit selectionChanged(*item);
            }
        }
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        if (auto item = m_model.selectedItemIndex()) {
            emit commitRequested(*item, false);
        }
    });
}

void CompletionPopup::setList(fl::CompletionList list) {
    m_model.setList(std::move(list));
    refresh();
}

void CompletionPopup::setPrefix(std::string_view prefix) {
    m_model.setPrefix(prefix);
    refresh();
}

void CompletionPopup::replaceItem(std::size_t index, fl::CompletionItem item) {
    m_model.replaceItem(index, std::move(item));
    const auto entries = m_model.entries();
    for (std::size_t row = 0; row < entries.size(); ++row) {
        if (entries[row].item == index) {
            if (QListWidgetItem* rowItem = m_list->item(static_cast<int>(row)); rowItem != nullptr) {
                rowItem->setData(kDetailRole, qs(m_model.items()[index].detail));
            }
        }
    }
    updateDocumentation();
}

void CompletionPopup::refresh() {
    const QSignalBlocker block(m_list);
    m_list->clear();
    for (const fl::CompletionEntry& entry : m_model.entries()) {
        const fl::CompletionItem& item = m_model.items()[entry.item];
        auto* row = new QListWidgetItem(kindIcon(item.kind), qs(item.label));
        row->setData(kDetailRole, qs(item.detail));
        row->setData(kItemRole, static_cast<qulonglong>(entry.item));
        row->setToolTip(qs(fl::toString(item.kind)));
        if (item.deprecated) {
            QFont font = m_list->font();
            font.setStrikeOut(true);
            row->setFont(font);
        }
        m_list->addItem(row);
    }
    const int rows = std::clamp(m_list->count(), 1, kMaxCompletionRows);
    const int rowHeight = std::max(m_list->sizeHintForRow(0), QFontMetrics(m_list->font()).height() + 4);
    m_list->setFixedHeight(rows * rowHeight + 2 * m_list->frameWidth());
    syncSelection();
    updateDocumentation();
    adjustSize();
    if (auto item = m_model.selectedItemIndex()) {
        emit selectionChanged(*item);
    }
}

int CompletionPopup::rowCount() const { return m_list->count(); }

QString CompletionPopup::documentationText() const {
    return m_documentation->isHidden() ? QString() : m_documentation->toPlainText();
}

void CompletionPopup::syncSelection() {
    const QSignalBlocker block(m_list);
    if (auto selected = m_model.selected()) {
        m_list->setCurrentRow(static_cast<int>(*selected));
        m_list->scrollToItem(m_list->currentItem());
    }
}

void CompletionPopup::updateDocumentation() {
    const fl::CompletionItem* item = m_model.selectedItem();
    if (item == nullptr || (item->documentation.text.empty() && item->detail.empty())) {
        m_documentation->hide();
        m_documentation->clear();
        adjustSize();
        return;
    }
    QString html;
    if (!item->detail.empty()) {
        html += QStringLiteral("<p><code>%1</code></p>").arg(qs(item->detail).toHtmlEscaped());
    }
    if (!item->documentation.text.empty()) {
        html += markupToHtml(item->documentation);
    }
    m_documentation->setHtml(html);
    m_documentation->setFixedHeight(m_list->height());
    m_documentation->show();
    adjustSize();
}

bool CompletionPopup::handleKey(int key, Qt::KeyboardModifiers modifiers) {
    if (!isVisible() || (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0) {
        return false;
    }
    const auto moved = [this] {
        syncSelection();
        updateDocumentation();
        if (auto item = m_model.selectedItemIndex()) {
            emit selectionChanged(*item);
        }
        return true;
    };
    const int page = std::max(1, std::min(m_list->count(), kMaxCompletionRows) - 1);
    switch (key) {
    case Qt::Key_Up: m_model.moveSelection(-1); return moved();
    case Qt::Key_Down: m_model.moveSelection(1); return moved();
    case Qt::Key_PageUp:
        m_model.select(m_model.selected().value_or(0) > static_cast<std::size_t>(page)
                           ? *m_model.selected() - static_cast<std::size_t>(page)
                           : 0);
        return moved();
    case Qt::Key_PageDown:
        m_model.select(m_model.selected().value_or(0) + static_cast<std::size_t>(page));
        return moved();
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
        if (auto item = m_model.selectedItemIndex()) {
            emit commitRequested(*item, key == Qt::Key_Tab);
            return true;
        }
        return false;
    case Qt::Key_Escape:
        hide();
        emit cancelled();
        return true;
    default: return false;
    }
}

void CompletionPopup::keyPressEvent(QKeyEvent* event) {
    if (!handleKey(event->key(), event->modifiers())) {
        HintPopup::keyPressEvent(event);
    }
}

// --- ParameterInfoPopup ---

ParameterInfoPopup::ParameterInfoPopup(QWidget* parent) : HintPopup(parent), m_label(new QLabel(this)) {
    setObjectName(QStringLiteral("parameterInfoPopup"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    m_label->setTextFormat(Qt::RichText);
    m_label->setWordWrap(false);
    layout->addWidget(m_label);
}

void ParameterInfoPopup::setHelp(const fl::SignatureHelp& help) {
    QString html;
    for (std::size_t i = 0; i < help.signatures.size(); ++i) {
        const fl::SignatureInfo& signature = help.signatures[i];
        const bool active = i == help.activeSignature;
        QString line;
        const auto range = fl::parameterRange(signature, help.activeParameterOf(i));
        if (range && range->second <= signature.label.size()) {
            const std::string_view label = signature.label;
            line = qs(label.substr(0, range->first)).toHtmlEscaped() + QStringLiteral("<b>") +
                   qs(label.substr(range->first, range->second - range->first)).toHtmlEscaped() +
                   QStringLiteral("</b>") + qs(label.substr(range->second)).toHtmlEscaped();
        } else {
            line = qs(signature.label).toHtmlEscaped();
        }
        if (!active) {
            line = QStringLiteral("<span style=\"color:gray\">%1</span>").arg(line);
        }
        html += (i == 0 ? QString() : QStringLiteral("<br>")) + line;
    }
    if (help.activeSignature < help.signatures.size()) {
        const fl::Markup& doc = help.signatures[help.activeSignature].documentation;
        if (!doc.text.empty()) {
            html += QStringLiteral("<hr>") + qs(doc.text).toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"));
        }
    }
    m_label->setText(html);
    adjustSize();
}

QString ParameterInfoPopup::html() const { return m_label->text(); }

// --- QuickDocPopup ---

QuickDocPopup::QuickDocPopup(QWidget* parent) : HintPopup(parent), m_browser(new QTextBrowser(this)) {
    setObjectName(QStringLiteral("quickDocPopup"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    m_browser->setFocusPolicy(Qt::NoFocus);
    m_browser->setOpenLinks(false);
    m_browser->setMinimumSize(360, 80);
    m_browser->setMaximumSize(640, 420);
    layout->addWidget(m_browser);
}

void QuickDocPopup::setMarkup(const fl::Markup& markup) {
    m_browser->setHtml(markupToHtml(markup));
    // Fit the height to the text, within the maximum.
    m_browser->document()->setTextWidth(560);
    const int height = static_cast<int>(m_browser->document()->size().height()) + 12;
    m_browser->setFixedSize(580, std::clamp(height, 80, 420));
    adjustSize();
}

QString QuickDocPopup::plainText() const { return m_browser->toPlainText(); }

} // namespace frost::ui::lang
