// SPDX-License-Identifier: Apache-2.0
//
// The language-feature popups (frost/ui/lang/lang_widgets.hpp): the symbol popup (file structure,
// go to class or symbol), the rename field, the choice popup, quick definition and the hierarchy
// popup (split from lang_widgets.cpp, FND-12a).

#include "frost/ui/lang/lang_widgets.hpp"

#include "lang_widgets_internal.hpp"
#include <QCheckBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace frost::ui::lang {

namespace fl = ::frost::lang;

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// A Qt::Popup frame with a vertical layout; closes on Esc through the subclass' event filter.
void setUpPopupFrame(QFrame* frame) {
    frame->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    frame->setFrameShape(QFrame::Box);
    frame->setAttribute(Qt::WA_DeleteOnClose, false);
}

} // namespace

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
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (item != nullptr && !item->flags().testFlag(Qt::ItemIsEnabled)) {
            return; // A disabled row (start's disabledReasons).
        }
        if (!m_done && m_list->currentRow() >= 0) {
            m_done = true;
            hide();
            emit chosen(static_cast<std::size_t>(m_list->currentRow()));
        }
    });
}

void ChoicePopup::start(const QString& title, const std::vector<QString>& choices, const QRect& anchor,
                        std::vector<bool> preferred, std::vector<QString> disabledReasons) {
    m_done = false;
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    m_list->clear();
    int firstEnabled = -1;
    for (std::size_t i = 0; i < choices.size(); ++i) {
        auto* row = new QListWidgetItem(choices[i]);
        if (i < preferred.size() && preferred[i]) {
            QFont font = m_list->font();
            font.setBold(true);
            row->setFont(font);
        }
        if (i < disabledReasons.size() && !disabledReasons[i].isEmpty()) {
            // Visible but inert: the reason names the owning epic.
            row->setFlags(row->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
            row->setToolTip(disabledReasons[i]);
        } else if (firstEnabled < 0) {
            firstEnabled = static_cast<int>(i);
        }
        m_list->addItem(row);
    }
    m_list->setCurrentRow(firstEnabled);
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
