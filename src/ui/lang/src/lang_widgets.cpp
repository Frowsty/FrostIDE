// SPDX-License-Identifier: Apache-2.0
//
// The language-feature widgets (frost/ui/lang/lang_widgets.hpp): kind badges, Markdown to rich
// text, the completion lookup with its documentation panel, parameter info, quick doc, the
// find-usages panel and the symbol, rename and choice popups.

#include "frost/ui/lang/lang_widgets.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

constexpr int kDetailRole = Qt::UserRole + 1; // The completion detail drawn right-aligned.
constexpr int kItemRole = Qt::UserRole + 2;   // The model item index of a completion row.
constexpr int kTargetRole = Qt::UserRole + 3; // The (group, usage) or symbol index of a row.
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

// A Qt::Popup frame with a vertical layout; closes on Esc through the subclass' event filter.
void setUpPopupFrame(QFrame* frame) {
    frame->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    frame->setFrameShape(QFrame::Box);
    frame->setAttribute(Qt::WA_DeleteOnClose, false);
}

// Places `widget` (already sized) below `anchor`, or above it when there is no room below,
// inside the anchor's screen.
void placeNear(QWidget* widget, const QRect& anchor, bool above) {
    QScreen* screen = QGuiApplication::screenAt(anchor.center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect area = screen != nullptr ? screen->availableGeometry() : QRect(0, 0, 4096, 4096);
    const QSize size = widget->size();
    int y = above ? anchor.top() - size.height() - 1 : anchor.bottom() + 2;
    if (!above && y + size.height() > area.bottom()) {
        y = anchor.top() - size.height() - 1;
    } else if (above && y < area.top()) {
        y = anchor.bottom() + 2;
    }
    const int x = std::clamp(anchor.left(), area.left(), std::max(area.left(), area.right() - size.width()));
    widget->move(x, std::max(area.top(), y));
}

std::string fileName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

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
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("completionList"));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformItemSizes(true);
    m_list->setItemDelegate(new CompletionDelegate(m_list));
    m_list->setFixedWidth(kCompletionWidth);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_documentation = new QTextBrowser(this);
    m_documentation->setObjectName(QStringLiteral("completionDocumentation"));
    m_documentation->setFocusPolicy(Qt::NoFocus);
    m_documentation->setFixedWidth(kDocumentationWidth);
    m_documentation->hide();
    layout->addWidget(m_list);
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

ParameterInfoPopup::ParameterInfoPopup(QWidget* parent) : HintPopup(parent) {
    setObjectName(QStringLiteral("parameterInfoPopup"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    m_label = new QLabel(this);
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

QuickDocPopup::QuickDocPopup(QWidget* parent) : HintPopup(parent) {
    setObjectName(QStringLiteral("quickDocPopup"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    m_browser = new QTextBrowser(this);
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

// --- FindUsagesPanel ---

FindUsagesPanel::FindUsagesPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("findUsagesPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_title = new QLabel(this);
    m_title->setContentsMargins(6, 2, 6, 2);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setRootIsDecorated(true);
    layout->addWidget(m_title);
    layout->addWidget(m_tree);
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) { onActivated(item); });
}

void FindUsagesPanel::setUsages(const QString& title, std::vector<fl::UsageGroup> groups) {
    m_groups = std::move(groups);
    m_tree->clear();
    m_count = 0;
    for (std::size_t g = 0; g < m_groups.size(); ++g) {
        const fl::UsageGroup& group = m_groups[g];
        auto* file = new QTreeWidgetItem(m_tree);
        file->setText(0, QStringLiteral("%1  %2 %3")
                             .arg(qs(fileName(group.path)))
                             .arg(group.usages.size())
                             .arg(group.usages.size() == 1 ? QStringLiteral("usage") : QStringLiteral("usages")));
        file->setToolTip(0, qs(group.path));
        for (std::size_t u = 0; u < group.usages.size(); ++u) {
            const fl::Usage& usage = group.usages[u];
            auto* row = new QTreeWidgetItem(file);
            row->setText(0, QStringLiteral("%1  %2").arg(usage.line).arg(qs(usage.lineText)));
            row->setData(0, kTargetRole, QVariant::fromValue(QPoint(static_cast<int>(g), static_cast<int>(u))));
            ++m_count;
        }
        file->setExpanded(true);
    }
    m_title->setText(QStringLiteral("%1: %2 %3")
                         .arg(title)
                         .arg(m_count)
                         .arg(m_count == 1 ? QStringLiteral("usage") : QStringLiteral("usages")));
    if (QTreeWidgetItem* first = m_tree->topLevelItem(0); first != nullptr && first->childCount() > 0) {
        m_tree->setCurrentItem(first->child(0));
    }
}

QString FindUsagesPanel::title() const { return m_title->text(); }

void FindUsagesPanel::activate(std::size_t group, std::size_t index) {
    if (group < m_groups.size() && index < m_groups[group].usages.size()) {
        const fl::Location& location = m_groups[group].usages[index].location;
        emit openRequested(qs(location.path), location.range.start);
    }
}

void FindUsagesPanel::onActivated(QTreeWidgetItem* item) {
    const QVariant target = item != nullptr ? item->data(0, kTargetRole) : QVariant();
    if (target.isValid()) {
        const QPoint at = target.toPoint();
        activate(static_cast<std::size_t>(at.x()), static_cast<std::size_t>(at.y()));
    }
}

// --- StructurePanel ---

StructurePanel::StructurePanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("structurePanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("structureSearch"));
    m_search->setPlaceholderText(QStringLiteral("Filter symbols"));
    m_search->setClearButtonEnabled(true);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("structureStatus"));
    m_status->setWordWrap(true);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("structureTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(2);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_search);
    layout->addWidget(m_status);
    layout->addWidget(m_tree, 1);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        const QPoint pos = item->data(0, kTargetRole).toPoint();
        emit activated(qs(m_path),
                       fl::LspPosition{static_cast<std::uint32_t>(pos.x()), static_cast<std::uint32_t>(pos.y())});
    });
    auto* navigate = new QShortcut(QKeySequence(Qt::Key_F4), this);
    navigate->setContext(Qt::WidgetWithChildrenShortcut);
    connect(navigate, &QShortcut::activated, this, [this] {
        if (auto* item = m_tree->currentItem())
            emit m_tree->itemActivated(item, 0);
    });
    setFocusProxy(m_search);
    setStatus(QStringLiteral("Open a file to view its structure."));
}

void StructurePanel::setStatus(const QString& status) {
    m_path.clear();
    m_symbols.clear();
    m_tree->clear();
    m_status->setText(status);
    m_status->show();
}

void StructurePanel::setSymbols(std::string path, std::vector<fl::DocumentSymbol> symbols) {
    m_path = std::move(path);
    m_symbols = std::move(symbols);
    m_status->setText(m_symbols.empty() ? QStringLiteral("No symbols in this file.") : QString());
    m_status->setVisible(m_symbols.empty());
    rebuild();
}

void StructurePanel::rebuild() {
    m_tree->clear();
    const auto shown = fl::filterSymbols(m_symbols, m_search->text().trimmed().toStdString());
    if (!m_path.empty()) {
        const bool empty = shown.empty();
        m_status->setText(m_symbols.empty() ? QStringLiteral("No symbols in this file.")
                                            : QStringLiteral("No matching symbols."));
        m_status->setVisible(empty);
    }
    const auto add = [&](auto& self, const fl::DocumentSymbol& symbol, QTreeWidgetItem* parent) -> void {
        auto* row = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        row->setIcon(0, kindIcon(symbol.kind));
        row->setText(0, qs(symbol.name));
        row->setText(1, qs(symbol.detail));
        row->setData(0, kTargetRole,
                     QPoint(static_cast<int>(symbol.selectionRange.start.line),
                            static_cast<int>(symbol.selectionRange.start.character)));
        for (const auto& child : symbol.children)
            self(self, child, row);
    };
    for (const auto& symbol : shown)
        add(add, symbol, nullptr);
    m_tree->expandAll();
    if (m_tree->topLevelItemCount() > 0)
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
}

// --- SymbolPopup ---

SymbolPopup::SymbolPopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("symbolPopup"));
    setUpPopupFrame(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    m_title = new QLabel(this);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("symbolSearch"));
    m_search->setClearButtonEnabled(true);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->setColumnCount(2);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_title);
    layout->addWidget(m_search);
    layout->addWidget(m_tree);
    resize(560, 420);
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_structureMode) {
            rebuild();
        } else {
            emit queryChanged(text);
        }
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem*, int) { activateCurrent(); });
}

void SymbolPopup::setStructure(QString title, std::string path, std::vector<fl::DocumentSymbol> symbols) {
    m_structureMode = true;
    m_title->setText(title);
    m_path = std::move(path);
    m_symbols = std::move(symbols);
    rebuild();
}

void SymbolPopup::setWorkspaceSymbols(QString title, std::vector<fl::WorkspaceSymbol> symbols) {
    m_structureMode = false;
    m_title->setText(title);
    m_workspaceSymbols = std::move(symbols);
    rebuild();
}

void SymbolPopup::rebuild() {
    m_tree->clear();
    if (m_structureMode) {
        const std::vector<fl::DocumentSymbol> shown =
            fl::filterSymbols(m_symbols, m_search->text().trimmed().toStdString());
        const bool filtering = !m_search->text().trimmed().isEmpty();
        QTreeWidgetItem* firstMatch = nullptr;
        // Rows carry the selection position; the tree keeps the symbol hierarchy.
        const auto add = [&](auto& self, const fl::DocumentSymbol& symbol, QTreeWidgetItem* parent) -> void {
            auto* row = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
            row->setIcon(0, kindIcon(symbol.kind));
            row->setText(0, qs(symbol.name));
            row->setText(1, qs(symbol.detail));
            row->setToolTip(0, qs(fl::toString(symbol.kind)));
            row->setData(0, kTargetRole,
                         QVariant::fromValue(QPoint(static_cast<int>(symbol.selectionRange.start.line),
                                                    static_cast<int>(symbol.selectionRange.start.character))));
            if (filtering && !symbol.matched) {
                row->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
            }
            if (firstMatch == nullptr && (!filtering || symbol.matched)) {
                firstMatch = row;
            }
            for (const fl::DocumentSymbol& child : symbol.children) {
                self(self, child, row);
            }
        };
        for (const fl::DocumentSymbol& symbol : shown) {
            add(add, symbol, nullptr);
        }
        m_tree->expandAll();
        if (firstMatch != nullptr) {
            m_tree->setCurrentItem(firstMatch);
        }
        return;
    }
    for (std::size_t i = 0; i < m_workspaceSymbols.size(); ++i) {
        const fl::WorkspaceSymbol& symbol = m_workspaceSymbols[i];
        auto* row = new QTreeWidgetItem(m_tree);
        row->setIcon(0, kindIcon(symbol.kind));
        row->setText(0, symbol.containerName.empty()
                            ? qs(symbol.name)
                            : QStringLiteral("%1  (%2)").arg(qs(symbol.name), qs(symbol.containerName)));
        row->setText(1, qs(fileName(symbol.location.path)));
        row->setToolTip(1, qs(symbol.location.path));
        row->setData(0, kTargetRole, static_cast<qulonglong>(i));
    }
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
}

void SymbolPopup::popup(QWidget* over) {
    if (over != nullptr) {
        const QPoint centre = over->mapToGlobal(over->rect().center());
        move(centre.x() - width() / 2, std::max(over->mapToGlobal(QPoint(0, 0)).y() + 40, centre.y() - height() / 2));
    }
    show();
    raise();
    activateWindow();
    m_search->setFocus();
}

QString SymbolPopup::query() const { return m_search->text(); }

void SymbolPopup::setQuery(const QString& query) { m_search->setText(query); }

int SymbolPopup::visibleRowCount() const {
    int count = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
        ++count;
    }
    return count;
}

void SymbolPopup::activateCurrent() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr) {
        return;
    }
    const QVariant target = item->data(0, kTargetRole);
    if (m_structureMode) {
        const QPoint at = target.toPoint();
        hide();
        emit activated(qs(m_path),
                       fl::LspPosition{static_cast<std::uint32_t>(at.x()), static_cast<std::uint32_t>(at.y())});
        return;
    }
    const std::size_t index = target.toULongLong();
    if (index < m_workspaceSymbols.size()) {
        const fl::Location location = m_workspaceSymbols[index].location;
        hide();
        emit activated(qs(location.path), location.range.start);
    }
}

bool SymbolPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Escape: hide(); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: activateCurrent(); return true;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown: QCoreApplication::sendEvent(m_tree, event); return true;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

// --- RenameInput ---

RenameInput::RenameInput(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("renameInput"));
    setUpPopupFrame(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    auto* label = new QLabel(QStringLiteral("Rename to:"), this);
    m_field = new QLineEdit(this);
    m_field->setMinimumWidth(260);
    layout->addWidget(label);
    layout->addWidget(m_field);
    m_field->installEventFilter(this);
}

void RenameInput::start(const QString& currentName, const QRect& anchor) {
    m_done = false;
    m_field->setText(currentName);
    m_field->selectAll();
    adjustSize();
    placeNear(this, anchor, false);
    show();
    raise();
    activateWindow();
    m_field->setFocus();
}

bool RenameInput::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_field && event->type() == QEvent::KeyPress && !m_done) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            m_done = true;
            hide();
            emit accepted(m_field->text().trimmed());
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            m_done = true;
            hide();
            emit cancelled();
            return true;
        }
    }
    if (watched == m_field && event->type() == QEvent::Hide && !m_done) {
        m_done = true; // Closed by a click outside.
        emit cancelled();
    }
    return QFrame::eventFilter(watched, event);
}

// --- ChoicePopup ---

ChoicePopup::ChoicePopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("choicePopup"));
    setUpPopupFrame(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    m_title = new QLabel(this);
    m_title->setContentsMargins(4, 2, 4, 2);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("choiceList"));
    layout->addWidget(m_title);
    layout->addWidget(m_list);
    m_list->installEventFilter(this);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
        if (!m_done && m_list->currentRow() >= 0) {
            m_done = true;
            hide();
            emit chosen(static_cast<std::size_t>(m_list->currentRow()));
        }
    });
}

void ChoicePopup::start(const QString& title, const std::vector<QString>& choices, const QRect& anchor,
                        std::vector<bool> preferred) {
    m_done = false;
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    m_list->clear();
    for (std::size_t i = 0; i < choices.size(); ++i) {
        auto* row = new QListWidgetItem(choices[i]);
        if (i < preferred.size() && preferred[i]) {
            QFont font = m_list->font();
            font.setBold(true);
            row->setFont(font);
        }
        m_list->addItem(row);
    }
    m_list->setCurrentRow(choices.empty() ? -1 : 0);
    const int rows = std::clamp(static_cast<int>(choices.size()), 1, 12);
    m_list->setFixedHeight(rows * std::max(m_list->sizeHintForRow(0), 18) + 2 * m_list->frameWidth());
    m_list->setMinimumWidth(std::min(640, std::max(240, m_list->sizeHintForColumn(0) + 24)));
    adjustSize();
    placeNear(this, anchor, false);
    show();
    raise();
    activateWindow();
    m_list->setFocus();
}

bool ChoicePopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_list && !m_done) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            m_done = true;
            hide();
            emit cancelled();
            return true;
        }
        if (event->type() == QEvent::Hide) {
            m_done = true; // Closed by a click outside.
            emit cancelled();
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace frost::ui::lang
