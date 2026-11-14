// SPDX-License-Identifier: Apache-2.0
//
// FindToolWindow (frost/ui/find/find_tool_window.hpp): the Find tool window's grouped result tree
// and its toolbar.

#include "frost/ui/find/find_tool_window.hpp"

#include "frost/io/path.hpp"
#include "frost/ui/icon_ids.hpp"

#include "find_util.hpp"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace frost::ui {

using find_detail::fromUtf8;
using find_detail::label;

namespace {

QToolButton* toolButton(QWidget* parent, std::string_view icon, const QString& text, const QString& tip,
                        const char* name) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setIcon(find_detail::themedIcon(icon));
    button->setText(text);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setToolButtonStyle(button->icon().isNull() ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
    return button;
}

} // namespace

FindToolWindow::FindToolWindow(QWidget* parent)
    : QWidget(parent), m_tree(new QTreeWidget(this)), m_status(new QLabel(this)) {
    setObjectName(QStringLiteral("findToolWindow"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 2, 4, 2);
    m_rerun = toolButton(this, icons::kActionsRefresh, label("Rerun"), label("Rerun"), "findRerun");
    m_previous = toolButton(this, icons::kNavChevronUp, label("Previous"), label("Previous Occurrence (Ctrl+Alt+Up)"),
                            "findPreviousOccurrence");
    m_next = toolButton(this, icons::kNavChevronDown, label("Next"), label("Next Occurrence (Ctrl+Alt+Down)"),
                        "findNextOccurrence");
    m_stop = toolButton(this, icons::kRunStop, label("Stop"), label("Stop"), "findStop");
    bar->addWidget(m_rerun);
    bar->addWidget(m_previous);
    bar->addWidget(m_next);
    bar->addWidget(m_stop);
    setTabOrder(m_stop, m_tree);
    m_status->setObjectName(QStringLiteral("findToolStatus"));
    bar->addWidget(m_status, 1);
    layout->addLayout(bar);
    m_tree->setObjectName(QStringLiteral("findResultTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setUniformRowHeights(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setItemDelegate(new find_detail::MatchItemDelegate(m_tree));
    m_tree->installEventFilter(this);
    layout->addWidget(m_tree, 1);

    connect(m_rerun, &QToolButton::clicked, this, [this] {
        if (m_session != nullptr) {
            m_session->rerun();
        }
    });
    connect(m_previous, &QToolButton::clicked, this, [this] { static_cast<void>(stepOccurrence(-1)); });
    connect(m_next, &QToolButton::clicked, this, [this] { static_cast<void>(stepOccurrence(1)); });
    connect(m_stop, &QToolButton::clicked, this, [this] {
        if (m_session != nullptr) {
            m_session->cancel();
        }
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        if (item != nullptr && item->parent() != nullptr) {
            static_cast<void>(activateCurrent());
        }
    });
    updateStatus();
}

FindToolWindow::~FindToolWindow() = default;

void FindToolWindow::setSession(FindSession* session) {
    if (m_session == session) {
        return;
    }
    if (m_session != nullptr) {
        disconnect(m_session, nullptr, this, nullptr);
        delete m_session.data();
    }
    m_session = session;
    if (session != nullptr) {
        session->setParent(this);
        connect(session, &FindSession::cleared, this, &FindToolWindow::rebuild);
        connect(session, &FindSession::filesAdded, this, &FindToolWindow::onFilesAdded);
        connect(session, &FindSession::finished, this, &FindToolWindow::updateStatus);
    }
    rebuild();
}

void FindToolWindow::rebuild() {
    m_tree->clear();
    if (m_session != nullptr && !m_session->files().empty()) {
        onFilesAdded(0, static_cast<int>(m_session->files().size()));
    }
    updateStatus();
}

void FindToolWindow::onFilesAdded(int first, int count) {
    const std::vector<search::FileMatches>& files = m_session->files();
    for (int f = first; f < first + count; ++f) {
        const search::FileMatches& file = files[static_cast<std::size_t>(f)];
        auto* fileRow = new QTreeWidgetItem(m_tree);
        fileRow->setText(0, fromUtf8(io::fileName(file.path)));
        fileRow->setIcon(0, find_detail::themedIcon(icons::kNodesFileCode));
        fileRow->setData(0, find_detail::kFileIndexRole, f);
        fileRow->setData(0, find_detail::kMatchIndexRole, -1);
        fileRow->setData(0, find_detail::kDetailRole,
                         QStringLiteral("%1   %2").arg(fromUtf8(io::parentPath(file.path)),
                                                       find_detail::matchesText(file.matches.size())));
        fileRow->setToolTip(0, fromUtf8(file.path));
        for (std::size_t m = 0; m < file.matches.size(); ++m) {
            const search::FindMatch& match = file.matches[m];
            const find_detail::RowText row = find_detail::rowText(match);
            auto* matchRow = new QTreeWidgetItem(fileRow);
            matchRow->setText(0, row.text);
            matchRow->setData(0, find_detail::kFileIndexRole, f);
            matchRow->setData(0, find_detail::kMatchIndexRole, static_cast<int>(m));
            matchRow->setData(0, find_detail::kHighlightStartRole, row.highlightStart);
            matchRow->setData(0, find_detail::kHighlightLengthRole, row.highlightLength);
            matchRow->setData(0, find_detail::kDetailRole,
                              QStringLiteral("%1:%2").arg(match.line + 1).arg(match.column + 1));
        }
        fileRow->setExpanded(true);
    }
    updateStatus();
}

void FindToolWindow::updateStatus() {
    if (m_session == nullptr) {
        m_status->setText(label("Use Find in Files (Ctrl+Shift+F) and Open in Find Window to list results here."));
    } else {
        const FindSettings& settings = m_session->settings();
        m_status->setText(QStringLiteral("\"%1\" %2: %3")
                              .arg(fromUtf8(settings.query.pattern), label(search::scopeName(settings.scope.kind)),
                                   m_session->statusText()));
    }
    const bool running = m_session != nullptr && m_session->running();
    m_rerun->setEnabled(m_session != nullptr && !running);
    m_stop->setEnabled(running);
    const bool occurrences = hasOccurrences();
    m_previous->setEnabled(occurrences);
    m_next->setEnabled(occurrences);
}

bool FindToolWindow::hasOccurrences() const { return m_session != nullptr && m_session->matchCount() > 0; }

std::optional<std::pair<std::size_t, std::size_t>> FindToolWindow::currentMatch() const {
    const QTreeWidgetItem* item = m_tree->currentItem();
    if (item == nullptr || item->parent() == nullptr || m_session == nullptr) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::size_t>(item->data(0, find_detail::kFileIndexRole).toInt()),
                     static_cast<std::size_t>(item->data(0, find_detail::kMatchIndexRole).toInt())};
}

bool FindToolWindow::activateCurrent() {
    const auto current = currentMatch();
    if (!current) {
        return false;
    }
    emit locationActivated(m_session->location(current->first, current->second));
    return true;
}

bool FindToolWindow::stepOccurrence(int delta) {
    if (!hasOccurrences()) {
        return false;
    }
    // Every match row in tree order, and where the selection is among them.
    std::vector<QTreeWidgetItem*> rows;
    int at = -1;
    const QTreeWidgetItem* current = m_tree->currentItem();
    for (int f = 0; f < m_tree->topLevelItemCount(); ++f) {
        QTreeWidgetItem* file = m_tree->topLevelItem(f);
        if (file == current) {
            // A selected file row: the next match is its first, the previous the one before it.
            at = static_cast<int>(rows.size()) - (delta > 0 ? 1 : 0);
        }
        for (int m = 0; m < file->childCount(); ++m) {
            if (file->child(m) == current) {
                at = static_cast<int>(rows.size());
            }
            rows.push_back(file->child(m));
        }
    }
    if (rows.empty()) {
        return false;
    }
    const int size = static_cast<int>(rows.size());
    int next = 0;
    if (at < 0 && current == nullptr) {
        next = delta > 0 ? 0 : size - 1;
    } else {
        next = ((at + delta) % size + size) % size;
    }
    QTreeWidgetItem* target = rows[static_cast<std::size_t>(next)];
    target->parent()->setExpanded(true);
    m_tree->setCurrentItem(target);
    m_tree->scrollToItem(target);
    return activateCurrent();
}

bool FindToolWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tree && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && currentMatch()) {
            static_cast<void>(activateCurrent());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace frost::ui
