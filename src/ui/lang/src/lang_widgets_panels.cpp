// SPDX-License-Identifier: Apache-2.0
//
// The language-feature panels (frost/ui/lang/lang_widgets.hpp): the find-usages panel with its
// grouping, access filters and preview, and the file structure panel (split from lang_widgets.cpp,
// FND-12a).

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/lang/lang_widgets.hpp"

#include "lang_widgets_internal.hpp"
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
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

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

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
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        if (const QIcon folderIcon = style->icon(icons::kNodesFolder); !folderIcon.isNull()) {
            m_groupByDirButton->setIcon(folderIcon);
        }
    }
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
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        if (const QIcon searchIcon = style->icon(icons::kActionsSearch); !searchIcon.isNull()) {
            m_search->addAction(searchIcon, QLineEdit::LeadingPosition);
        }
    }
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
        const auto shown =
            filterAndSortStructureSymbols(textFiltered, m_showFieldsButton->isChecked(),
                                          m_showFunctionsButton->isChecked(), m_sortAlphaButton->isChecked());
        if (m_tree->topLevelItemCount() == static_cast<int>(shown.size())) {
            const auto updateInPlace = [&](auto& self, QTreeWidgetItem* item, const fl::DocumentSymbol& sym) -> void {
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

} // namespace frost::ui::lang
