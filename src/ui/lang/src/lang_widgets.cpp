// SPDX-License-Identifier: Apache-2.0
//
// The language-feature widgets (frost/ui/lang/lang_widgets.hpp): kind badges, Markdown to rich
// text, the completion lookup with its documentation panel, parameter info, quick doc, the
// find-usages panel and the symbol, rename and choice popups.

#include "frost/ui/lang/lang_widgets.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QFontDatabase>
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
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

constexpr int kDetailRole = Qt::UserRole + 1; // The completion detail drawn right-aligned.
constexpr int kItemRole = Qt::UserRole + 2;   // The model item index of a completion row.
constexpr int kTargetRole = Qt::UserRole + 3; // The (group, usage) or symbol index of a row.
constexpr int kRangeStartRole = Qt::UserRole + 4;
constexpr int kRangeEndRole = Qt::UserRole + 5;
constexpr int kPathRole = Qt::UserRole + 6;
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

std::string directoryName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string(".") : std::string(path.substr(0, slash));
}

bool isWriteUsage(const fl::Usage& usage) noexcept {
    const std::string_view text = usage.lineText;
    const std::size_t end = std::min<std::size_t>(usage.highlightEnd, text.size());
    std::size_t after = end;
    while (after < text.size() && (text[after] == ' ' || text[after] == '\t')) {
        ++after;
    }
    const std::string_view tail = text.substr(after);
    if (tail.starts_with("++") || tail.starts_with("--") || tail.starts_with("+=") || tail.starts_with("-=") ||
        tail.starts_with("*=") || tail.starts_with("/=") || tail.starts_with("%=") || tail.starts_with("&=") ||
        tail.starts_with("|=") || tail.starts_with("^=") || tail.starts_with("<<=") || tail.starts_with(">>=")) {
        return true;
    }
    if (tail.starts_with('=') && !tail.starts_with("==")) {
        return true;
    }
    std::size_t before = std::min<std::size_t>(usage.highlightBegin, text.size());
    while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t')) {
        --before;
    }
    const std::string_view head = text.substr(0, before);
    return head.ends_with("++") || head.ends_with("--");
}

QToolButton* makeToggleToolButton(QWidget* parent, const QString& text, const QString& toolTip, bool checked) {
    auto* btn = new QToolButton(parent);
    btn->setText(text);
    btn->setToolTip(toolTip);
    btn->setCheckable(true);
    btn->setChecked(checked);
    btn->setAutoRaise(true);
    return btn;
}

bool isFieldOrVariableKind(fl::SymbolKind kind) noexcept {
    using K = fl::SymbolKind;
    return kind == K::Field || kind == K::Property || kind == K::Variable || kind == K::Constant ||
           kind == K::EnumMember;
}

bool isFunctionOrMethodKind(fl::SymbolKind kind) noexcept {
    using K = fl::SymbolKind;
    return kind == K::Function || kind == K::Method || kind == K::Constructor || kind == K::Operator;
}

std::vector<fl::DocumentSymbol> filterAndSortStructureSymbols(const std::vector<fl::DocumentSymbol>& symbols,
                                                              bool showFields, bool showFunctions, bool sortAlpha) {
    std::vector<fl::DocumentSymbol> out;
    out.reserve(symbols.size());
    for (const fl::DocumentSymbol& sym : symbols) {
        if (!showFields && isFieldOrVariableKind(sym.kind) && sym.children.empty()) {
            continue;
        }
        if (!showFunctions && isFunctionOrMethodKind(sym.kind) && sym.children.empty()) {
            continue;
        }
        fl::DocumentSymbol copy = sym;
        copy.children = filterAndSortStructureSymbols(sym.children, showFields, showFunctions, sortAlpha);
        out.push_back(std::move(copy));
    }
    if (sortAlpha) {
        std::ranges::stable_sort(out, [](const fl::DocumentSymbol& a, const fl::DocumentSymbol& b) {
            return QString::fromStdString(a.name).compare(QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
        });
    }
    return out;
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
    layout->setSpacing(0);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(6, 2, 6, 2);
    headerRow->setSpacing(4);
    m_title = new QLabel(this);
    m_groupByDirButton =
        makeToggleToolButton(this, QStringLiteral("Dir"), QStringLiteral("Group by Directory / File"), false);
    m_showReadButton = makeToggleToolButton(this, QStringLiteral("R"), QStringLiteral("Show Read Access"), true);
    m_showWriteButton = makeToggleToolButton(this, QStringLiteral("W"), QStringLiteral("Show Write Access"), true);
    headerRow->addWidget(m_title, 1);
    headerRow->addWidget(m_groupByDirButton);
    headerRow->addWidget(m_showReadButton);
    headerRow->addWidget(m_showWriteButton);
    layout->addLayout(headerRow);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_tree = new QTreeWidget(m_splitter);
    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setRootIsDecorated(true);
    m_preview = new QPlainTextEdit(m_splitter);
    m_preview->setObjectName(QStringLiteral("findUsagesPreview"));
    m_preview->setReadOnly(true);
    m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_splitter->addWidget(m_tree);
    m_splitter->addWidget(m_preview);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    layout->addWidget(m_splitter, 1);

    connect(m_groupByDirButton, &QToolButton::toggled, this, [this] { rebuildTree(); });
    connect(m_showReadButton, &QToolButton::toggled, this, [this] { rebuildTree(); });
    connect(m_showWriteButton, &QToolButton::toggled, this, [this] { rebuildTree(); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { updatePreview(current); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) { onActivated(item); });
}

void FindUsagesPanel::setLineReader(fl::LineSource reader) { m_lineReader = std::move(reader); }

void FindUsagesPanel::setUsages(const QString& title, std::vector<fl::UsageGroup> groups) {
    m_baseTitle = title;
    m_groups = std::move(groups);
    rebuildTree();
}

void FindUsagesPanel::rebuildTree() {
    m_tree->clear();
    m_preview->clear();
    m_count = 0;
    const bool showRead = m_showReadButton->isChecked();
    const bool showWrite = m_showWriteButton->isChecked();
    const bool byDir = m_groupByDirButton->isChecked();
    std::map<std::string, QTreeWidgetItem*> dirNodes;
    QTreeWidgetItem* firstUsageItem = nullptr;

    for (std::size_t g = 0; g < m_groups.size(); ++g) {
        const fl::UsageGroup& group = m_groups[g];
        std::vector<std::size_t> matchingIndices;
        for (std::size_t u = 0; u < group.usages.size(); ++u) {
            const bool write = isWriteUsage(group.usages[u]);
            if ((write && showWrite) || (!write && showRead)) {
                matchingIndices.push_back(u);
            }
        }
        if (matchingIndices.empty()) {
            continue;
        }
        QTreeWidgetItem* parentNode = nullptr;
        if (byDir) {
            const std::string dir = directoryName(group.path);
            auto it = dirNodes.find(dir);
            if (it == dirNodes.end()) {
                auto* dirItem = new QTreeWidgetItem(m_tree);
                dirItem->setText(0, qs(dir));
                dirItem->setExpanded(true);
                it = dirNodes.emplace(dir, dirItem).first;
            }
            parentNode = it->second;
        }
        auto* file = parentNode != nullptr ? new QTreeWidgetItem(parentNode) : new QTreeWidgetItem(m_tree);
        file->setText(0, QStringLiteral("%1  %2 %3")
                             .arg(qs(fileName(group.path)))
                             .arg(matchingIndices.size())
                             .arg(matchingIndices.size() == 1 ? QStringLiteral("usage") : QStringLiteral("usages")));
        file->setToolTip(0, qs(group.path));
        for (const std::size_t u : matchingIndices) {
            const fl::Usage& usage = group.usages[u];
            auto* row = new QTreeWidgetItem(file);
            row->setText(0, QStringLiteral("%1  %2").arg(usage.line).arg(qs(usage.lineText)));
            row->setData(0, kTargetRole, QVariant::fromValue(QPoint(static_cast<int>(g), static_cast<int>(u))));
            if (firstUsageItem == nullptr) {
                firstUsageItem = row;
            }
            ++m_count;
        }
        file->setExpanded(true);
    }
    m_title->setText(QStringLiteral("%1: %2 %3")
                         .arg(m_baseTitle)
                         .arg(m_count)
                         .arg(m_count == 1 ? QStringLiteral("usage") : QStringLiteral("usages")));
    if (firstUsageItem != nullptr) {
        m_tree->setCurrentItem(firstUsageItem);
    }
}

void FindUsagesPanel::updatePreview(QTreeWidgetItem* current) {
    if (current == nullptr) {
        m_preview->clear();
        return;
    }
    QVariant target = current->data(0, kTargetRole);
    if (!target.isValid() && current->childCount() > 0) {
        target = current->child(0)->data(0, kTargetRole);
    }
    if (!target.isValid()) {
        m_preview->clear();
        return;
    }
    const QPoint at = target.toPoint();
    const auto g = static_cast<std::size_t>(at.x());
    const auto u = static_cast<std::size_t>(at.y());
    if (g >= m_groups.size() || u >= m_groups[g].usages.size()) {
        m_preview->clear();
        return;
    }
    const fl::Usage& usage = m_groups[g].usages[u];
    const std::string& path = m_groups[g].path;
    const std::uint32_t targetLine0 = usage.line > 0 ? usage.line - 1 : 0;
    const std::uint32_t startLine0 = targetLine0 > 7 ? targetLine0 - 7 : 0;
    const std::uint32_t endLine0 = targetLine0 + 7;

    QStringList snippetLines;
    for (std::uint32_t line0 = startLine0; line0 <= endLine0; ++line0) {
        if (m_lineReader) {
            if (auto text = m_lineReader(path, line0)) {
                const QString marker = (line0 == targetLine0) ? QStringLiteral("> ") : QStringLiteral("  ");
                snippetLines.push_back(QStringLiteral("%1%2: %3").arg(marker).arg(line0 + 1, 4).arg(qs(*text)));
            }
        }
    }
    if (snippetLines.isEmpty()) {
        QFile file(qs(path));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList allLines = QString::fromUtf8(file.readAll()).split(u'\n');
            for (std::uint32_t line0 = startLine0; line0 <= endLine0 && static_cast<qsizetype>(line0) < allLines.size();
                 ++line0) {
                QString lineText = allLines[static_cast<qsizetype>(line0)];
                if (lineText.endsWith(u'\r')) {
                    lineText.chop(1);
                }
                const QString marker = (line0 == targetLine0) ? QStringLiteral("> ") : QStringLiteral("  ");
                snippetLines.push_back(QStringLiteral("%1%2: %3").arg(marker).arg(line0 + 1, 4).arg(lineText));
            }
        }
    }
    if (snippetLines.isEmpty()) {
        for (const fl::Usage& entry : m_groups[g].usages) {
            const QString marker = (entry.line == usage.line) ? QStringLiteral("> ") : QStringLiteral("  ");
            snippetLines.push_back(QStringLiteral("%1%2: %3").arg(marker).arg(entry.line, 4).arg(qs(entry.lineText)));
        }
    }
    m_preview->setPlainText(snippetLines.join(u'\n'));
}

QString FindUsagesPanel::title() const { return m_title->text(); }

void FindUsagesPanel::setGroupByDirectory(bool enabled) { m_groupByDirButton->setChecked(enabled); }
bool FindUsagesPanel::groupByDirectory() const noexcept { return m_groupByDirButton->isChecked(); }
void FindUsagesPanel::setShowReadAccess(bool show) { m_showReadButton->setChecked(show); }
bool FindUsagesPanel::showReadAccess() const noexcept { return m_showReadButton->isChecked(); }
void FindUsagesPanel::setShowWriteAccess(bool show) { m_showWriteButton->setChecked(show); }
bool FindUsagesPanel::showWriteAccess() const noexcept { return m_showWriteButton->isChecked(); }

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

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(2);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("structureSearch"));
    m_search->setPlaceholderText(QStringLiteral("Filter symbols"));
    m_search->setClearButtonEnabled(true);
    m_sortAlphaButton = makeToggleToolButton(this, QStringLiteral("A-Z"), QStringLiteral("Sort Alphabetically"), false);
    m_showFieldsButton = makeToggleToolButton(this, QStringLiteral("F"), QStringLiteral("Show Fields/Variables"), true);
    m_showFunctionsButton =
        makeToggleToolButton(this, QStringLiteral("m"), QStringLiteral("Show Functions/Methods"), true);
    m_autoScrollButton =
        makeToggleToolButton(this, QStringLiteral("⇄"), QStringLiteral("Auto-Scroll to Source"), false);
    topRow->addWidget(m_search, 1);
    topRow->addWidget(m_sortAlphaButton);
    topRow->addWidget(m_showFieldsButton);
    topRow->addWidget(m_showFunctionsButton);
    topRow->addWidget(m_autoScrollButton);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("structureStatus"));
    m_status->setWordWrap(true);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("structureTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(2);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addLayout(topRow);
    layout->addWidget(m_status);
    layout->addWidget(m_tree, 1);

    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(m_sortAlphaButton, &QToolButton::toggled, this, [this] { rebuild(); });
    connect(m_showFieldsButton, &QToolButton::toggled, this, [this] { rebuild(); });
    connect(m_showFunctionsButton, &QToolButton::toggled, this, [this] { rebuild(); });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr) {
            return;
        }
        const QPoint pos = item->data(0, kTargetRole).toPoint();
        emit activated(qs(m_path),
                       fl::LspPosition{static_cast<std::uint32_t>(pos.x()), static_cast<std::uint32_t>(pos.y())});
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (m_autoScrollButton->isChecked() && item != nullptr) {
            const QPoint pos = item->data(0, kTargetRole).toPoint();
            emit activated(qs(m_path),
                           fl::LspPosition{static_cast<std::uint32_t>(pos.x()), static_cast<std::uint32_t>(pos.y())});
        }
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
    m_lastHighlightedLine = -1;
    m_tree->clear();
    m_status->setText(status);
    m_status->show();
}

void StructurePanel::setSymbols(std::string path, std::vector<fl::DocumentSymbol> symbols) {
    const auto sameStructure = [](auto& self, const std::vector<fl::DocumentSymbol>& a,
                                  const std::vector<fl::DocumentSymbol>& b) -> bool {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].name != b[i].name || a[i].detail != b[i].detail || a[i].kind != b[i].kind ||
                !self(self, a[i].children, b[i].children)) {
                return false;
            }
        }
        return true;
    };

    if (!m_path.empty() && m_path == path && sameStructure(sameStructure, m_symbols, symbols) &&
        (m_tree->topLevelItemCount() > 0 || symbols.empty())) {
        m_symbols = std::move(symbols);
        const auto textFiltered = fl::filterSymbols(m_symbols, m_search->text().trimmed().toStdString());
        const auto shown = filterAndSortStructureSymbols(textFiltered, m_showFieldsButton->isChecked(),
                                                         m_showFunctionsButton->isChecked(),
                                                         m_sortAlphaButton->isChecked());
        if (m_tree->topLevelItemCount() == static_cast<int>(shown.size())) {
            const auto updateInPlace = [&](auto& self, QTreeWidgetItem* item,
                                           const fl::DocumentSymbol& sym) -> void {
                if (!item || item->childCount() != static_cast<int>(sym.children.size()))
                    return;
                item->setData(0, kTargetRole,
                              QPoint(static_cast<int>(sym.selectionRange.start.line),
                                     static_cast<int>(sym.selectionRange.start.character)));
                item->setData(0, kRangeStartRole, static_cast<int>(sym.range.start.line));
                item->setData(0, kRangeEndRole, static_cast<int>(sym.range.end.line));
                for (std::size_t i = 0; i < sym.children.size(); ++i)
                    self(self, item->child(static_cast<int>(i)), sym.children[i]);
            };
            for (std::size_t i = 0; i < shown.size(); ++i)
                updateInPlace(updateInPlace, m_tree->topLevelItem(static_cast<int>(i)), shown[i]);
        }
        return;
    }

    const bool sameFile = (!m_path.empty() && m_path == path);
    m_path = std::move(path);
    m_symbols = std::move(symbols);
    if (!sameFile) {
        m_lastHighlightedLine = -1;
    }
    m_status->setText(m_symbols.empty() ? QStringLiteral("No symbols in this file.") : QString());
    m_status->setVisible(m_symbols.empty());
    rebuild();
}

void StructurePanel::rebuild() {
    const auto itemKey = [](QTreeWidgetItem* item) -> QString {
        QStringList parts;
        for (QTreeWidgetItem* cur = item; cur != nullptr; cur = cur->parent()) {
            parts.prepend(cur->text(0) + QLatin1Char('\x1f') + cur->text(1));
        }
        return parts.join(QLatin1Char('\x1e'));
    };

    const bool hadExistingTree = m_tree->topLevelItemCount() > 0;
    std::set<QString> collapsedKeys;
    QString selectedKey;
    const int savedScroll = (m_tree->verticalScrollBar() != nullptr) ? m_tree->verticalScrollBar()->value() : 0;
    if (hadExistingTree) {
        if (QTreeWidgetItem* cur = m_tree->currentItem())
            selectedKey = itemKey(cur);
        for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
            QTreeWidgetItem* item = *it;
            if (item->childCount() > 0 && !item->isExpanded()) {
                collapsedKeys.insert(itemKey(item));
            }
        }
    }

    m_tree->clear();
    const auto textFiltered = fl::filterSymbols(m_symbols, m_search->text().trimmed().toStdString());
    const auto shown =
        filterAndSortStructureSymbols(textFiltered, m_showFieldsButton->isChecked(), m_showFunctionsButton->isChecked(),
                                      m_sortAlphaButton->isChecked());
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
        row->setData(0, kRangeStartRole, static_cast<int>(symbol.range.start.line));
        row->setData(0, kRangeEndRole, static_cast<int>(symbol.range.end.line));
        for (const auto& child : symbol.children)
            self(self, child, row);
    };
    for (const auto& symbol : shown)
        add(add, symbol, nullptr);
    m_tree->expandAll();
    if (hadExistingTree && !collapsedKeys.empty()) {
        for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
            QTreeWidgetItem* item = *it;
            if (item->childCount() > 0 && collapsedKeys.contains(itemKey(item))) {
                item->setExpanded(false);
            }
        }
    }
    if (hadExistingTree && !selectedKey.isEmpty()) {
        for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
            if (itemKey(*it) == selectedKey) {
                const QSignalBlocker blocker(m_tree);
                m_tree->setCurrentItem(*it);
                break;
            }
        }
    } else if (m_lastHighlightedLine >= 0) {
        highlightLine(m_lastHighlightedLine);
    } else if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
    if (hadExistingTree && m_tree->verticalScrollBar() != nullptr) {
        m_tree->verticalScrollBar()->setValue(savedScroll);
    }
}

void StructurePanel::highlightLine(int line) {
    m_lastHighlightedLine = line;
    if (line < 0 || m_tree->topLevelItemCount() == 0) {
        return;
    }
    QTreeWidgetItem* best = nullptr;
    int bestSpan = std::numeric_limits<int>::max();
    for (QTreeWidgetItemIterator it(m_tree); *it != nullptr; ++it) {
        QTreeWidgetItem* item = *it;
        const int startLine = item->data(0, kRangeStartRole).toInt();
        const int endLine = item->data(0, kRangeEndRole).toInt();
        if (line >= startLine && line <= endLine) {
            const int span = endLine - startLine;
            if (span <= bestSpan) {
                bestSpan = span;
                best = item;
            }
        }
    }
    if (best != nullptr) {
        // Never force-expand a user-collapsed ancestor when highlighting the caret line.
        for (QTreeWidgetItem* p = best->parent(); p != nullptr; p = p->parent()) {
            if (!p->isExpanded()) {
                best = p;
            }
        }
        if (m_tree->currentItem() == best) {
            return;
        }
        const QSignalBlocker blocker(m_tree);
        m_tree->setCurrentItem(best);
        if (m_autoScrollButton && m_autoScrollButton->isChecked()) {
            m_tree->scrollToItem(best);
        }
    }
}

void StructurePanel::setSortAlphabetically(bool enabled) { m_sortAlphaButton->setChecked(enabled); }
bool StructurePanel::sortAlphabetically() const noexcept { return m_sortAlphaButton->isChecked(); }
void StructurePanel::setShowFields(bool show) { m_showFieldsButton->setChecked(show); }
bool StructurePanel::showFields() const noexcept { return m_showFieldsButton->isChecked(); }
void StructurePanel::setShowFunctions(bool show) { m_showFunctionsButton->setChecked(show); }
bool StructurePanel::showFunctions() const noexcept { return m_showFunctionsButton->isChecked(); }
void StructurePanel::setAutoScrollToSource(bool enabled) { m_autoScrollButton->setChecked(enabled); }
bool StructurePanel::autoScrollToSource() const noexcept { return m_autoScrollButton->isChecked(); }

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
    m_label = new QLabel(QStringLiteral("Rename to:"), this);
    m_field = new QLineEdit(this);
    m_field->setMinimumWidth(260);
    m_searchComments = new QCheckBox(QStringLiteral("Search in comments and strings"), this);
    m_previewUsages = new QCheckBox(QStringLiteral("Preview usages"), this);
    layout->addWidget(m_label);
    layout->addWidget(m_field);
    layout->addWidget(m_searchComments);
    layout->addWidget(m_previewUsages);
    m_field->installEventFilter(this);
}

void RenameInput::start(const QString& currentName, const QRect& anchor) {
    startWithPrompt(QStringLiteral("Rename to:"), currentName, anchor, true);
}

void RenameInput::startWithPrompt(const QString& promptLabel, const QString& currentName, const QRect& anchor,
                                  bool showRenameOptions) {
    m_done = false;
    m_label->setText(promptLabel);
    m_searchComments->setVisible(showRenameOptions);
    m_previewUsages->setVisible(showRenameOptions);
    m_field->setText(currentName);
    m_field->selectAll();
    adjustSize();
    placeNear(this, anchor, false);
    show();
    raise();
    activateWindow();
    m_field->setFocus();
}

bool RenameInput::searchInCommentsAndStrings() const noexcept { return m_searchComments->isChecked(); }

bool RenameInput::previewUsages() const noexcept { return m_previewUsages->isChecked(); }

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

// --- QuickDefinitionPopup ---

QuickDefinitionPopup::QuickDefinitionPopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("quickDefinitionPopup"));
    setUpPopupFrame(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    m_header = new QLabel(this);
    m_header->setContentsMargins(4, 2, 4, 2);
    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("quickDefinitionEditor"));
    m_editor->setReadOnly(true);
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setMinimumSize(560, 280);
    layout->addWidget(m_header);
    layout->addWidget(m_editor, 1);
    m_editor->installEventFilter(this);
}

void QuickDefinitionPopup::showDefinition(const QString& filePath, fl::LspPosition targetPos,
                                          const QString& snippetText, const QRect& anchor) {
    m_targetPath = filePath;
    m_targetPosition = targetPos;
    const std::string pathStd = filePath.toStdString();
    m_header->setText(QStringLiteral("<b>%1</b> : %2  <span style=\"color:gray\">(%3 — Enter/F4 to open)</span>")
                          .arg(qs(fileName(pathStd)).toHtmlEscaped())
                          .arg(targetPos.line + 1)
                          .arg(filePath.toHtmlEscaped()));
    m_editor->setPlainText(snippetText);
    resize(620, 320);
    placeNear(this, anchor, false);
    show();
    raise();
    activateWindow();
    m_editor->setFocus();
}

QString QuickDefinitionPopup::headerText() const { return m_header->text(); }

bool QuickDefinitionPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F4) {
            hide();
            emit openRequested(m_targetPath, m_targetPosition);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

// --- HierarchyPopup ---

HierarchyPopup::HierarchyPopup(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("hierarchyPopup"));
    setUpPopupFrame(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(3);
    auto* topBar = new QHBoxLayout();
    topBar->setContentsMargins(2, 2, 2, 2);
    topBar->setSpacing(4);
    m_title = new QLabel(this);
    m_primaryBtn =
        makeToggleToolButton(this, QStringLiteral("Incoming"), QStringLiteral("Primary hierarchy view"), true);
    m_secondaryBtn =
        makeToggleToolButton(this, QStringLiteral("Outgoing"), QStringLiteral("Secondary hierarchy view"), false);
    topBar->addWidget(m_title, 1);
    topBar->addWidget(m_primaryBtn);
    topBar->addWidget(m_secondaryBtn);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("hierarchyTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(2);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addLayout(topBar);
    layout->addWidget(m_tree, 1);
    resize(560, 380);
    m_tree->installEventFilter(this);
    connect(m_primaryBtn, &QToolButton::clicked, this, [this] {
        m_primaryBtn->setChecked(true);
        m_secondaryBtn->setChecked(false);
        emit modeChanged(false);
    });
    connect(m_secondaryBtn, &QToolButton::clicked, this, [this] {
        m_primaryBtn->setChecked(false);
        m_secondaryBtn->setChecked(true);
        emit modeChanged(true);
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem*, int) { activateCurrent(); });
}

void HierarchyPopup::configureModes(const QString& primaryModeLabel, const QString& secondaryModeLabel,
                                    bool secondaryActive) {
    m_primaryBtn->setText(primaryModeLabel);
    m_secondaryBtn->setText(secondaryModeLabel);
    m_primaryBtn->setChecked(!secondaryActive);
    m_secondaryBtn->setChecked(secondaryActive);
}

void HierarchyPopup::setEntries(const QString& title, std::vector<Entry> entries) {
    m_title->setText(title);
    m_tree->clear();
    const auto addEntry = [&](auto& self, const Entry& entry, QTreeWidgetItem* parent) -> void {
        auto* row = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        row->setIcon(0, kindIcon(entry.kind));
        row->setText(0, entry.label);
        row->setText(1, entry.detail);
        row->setData(0, kPathRole, qs(entry.path));
        row->setData(0, kTargetRole,
                     QPoint(static_cast<int>(entry.position.line), static_cast<int>(entry.position.character)));
        for (const Entry& child : entry.children) {
            self(self, child, row);
        }
    };
    for (const Entry& entry : entries) {
        addEntry(addEntry, entry, nullptr);
    }
    m_tree->expandAll();
    if (m_tree->topLevelItemCount() > 0) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
}

void HierarchyPopup::popup(QWidget* over) {
    if (over != nullptr) {
        const QPoint centre = over->mapToGlobal(over->rect().center());
        move(centre.x() - width() / 2, std::max(over->mapToGlobal(QPoint(0, 0)).y() + 40, centre.y() - height() / 2));
    }
    show();
    raise();
    activateWindow();
    m_tree->setFocus();
}

bool HierarchyPopup::isSecondaryMode() const noexcept { return m_secondaryBtn->isChecked(); }

void HierarchyPopup::activateCurrent() {
    QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, kPathRole).toString();
    const QPoint pos = item->data(0, kTargetRole).toPoint();
    if (!path.isEmpty()) {
        hide();
        emit activated(path, fl::LspPosition{static_cast<std::uint32_t>(pos.x()), static_cast<std::uint32_t>(pos.y())});
    }
}

bool HierarchyPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            hide();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateCurrent();
            return true;
        }
        if (key->key() == Qt::Key_Tab) {
            const bool nextSecondary = !isSecondaryMode();
            m_primaryBtn->setChecked(!nextSecondary);
            m_secondaryBtn->setChecked(nextSecondary);
            emit modeChanged(nextSecondary);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace frost::ui::lang
