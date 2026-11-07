// SPDX-License-Identifier: Apache-2.0
//
// EditorFindBar (frost/ui/editor/find_bar.hpp): two rows of line edits, toggles and buttons, and the
// key handling of its fields.

#include "frost/ui/editor/find_bar.hpp"

#include "frost/ui/frost_style.hpp"
#include "frost/ui/icon_ids.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace frost::ui {

namespace {

QToolButton* toggle(const QString& text, const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QToolButton* plainButton(const QString& text, const QString& tip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

} // namespace

EditorFindBar::EditorFindBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("frostEditorFindBar"));
    setAutoFillBackground(true);
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(6, 4, 6, 4);
    rows->setSpacing(4);

    auto* findRow = new QHBoxLayout();
    findRow->setSpacing(4);
    m_expandReplace = plainButton(QStringLiteral("▸"), QStringLiteral("Toggle Replace (Ctrl+R)"), this);
    m_expandReplace->setObjectName(QStringLiteral("findBarToggleReplace"));
    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(QStringLiteral("Find"));
    m_find->setClearButtonEnabled(true);
    m_case = toggle(QStringLiteral("Cc"), QStringLiteral("Match Case"), this);
    m_word = toggle(QStringLiteral("W"), QStringLiteral("Words"), this);
    m_regex = toggle(QStringLiteral(".*"), QStringLiteral("Regex"), this);
    m_matches = new QLabel(this);
    m_matches->setMinimumWidth(80);
    m_previous = plainButton(QStringLiteral("↑"), QStringLiteral("Previous Occurrence (Shift+F3)"), this);
    m_next = plainButton(QStringLiteral("↓"), QStringLiteral("Next Occurrence (F3)"), this);
    m_close = plainButton(QStringLiteral("✕"), QStringLiteral("Close (Escape)"), this);
    if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
        if (const QIcon searchIcon = style->icon(icons::kActionsSearch); !searchIcon.isNull()) {
            m_find->addAction(searchIcon, QLineEdit::LeadingPosition);
        }
        if (const QIcon upIcon = style->icon(icons::kNavChevronUp); !upIcon.isNull()) {
            m_previous->setIcon(upIcon);
        }
        if (const QIcon downIcon = style->icon(icons::kNavChevronDown); !downIcon.isNull()) {
            m_next->setIcon(downIcon);
        }
        if (const QIcon closeIcon = style->icon(icons::kActionsClose); !closeIcon.isNull()) {
            m_close->setIcon(closeIcon);
        }
        if (const QIcon rightIcon = style->icon(icons::kNavChevronRight); !rightIcon.isNull()) {
            m_expandReplace->setIcon(rightIcon);
        }
    }
    findRow->addWidget(m_expandReplace);
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_case);
    findRow->addWidget(m_word);
    findRow->addWidget(m_regex);
    findRow->addWidget(m_matches);
    findRow->addWidget(m_previous);
    findRow->addWidget(m_next);
    findRow->addWidget(m_close);
    rows->addLayout(findRow);

    m_replaceRow = new QWidget(this);
    auto* replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(4);
    m_replace = new QLineEdit(m_replaceRow);
    m_replace->setPlaceholderText(QStringLiteral("Replace"));
    m_replaceOne = new QPushButton(QStringLiteral("Replace"), m_replaceRow);
    m_replaceAll = new QPushButton(QStringLiteral("Replace All"), m_replaceRow);
    m_replaceOne->setFocusPolicy(Qt::NoFocus);
    m_replaceAll->setFocusPolicy(Qt::NoFocus);
    replaceRow->addWidget(m_replace, 1);
    replaceRow->addWidget(m_replaceOne);
    replaceRow->addWidget(m_replaceAll);
    replaceRow->addStretch(0);
    rows->addWidget(m_replaceRow);
    m_replaceRow->setVisible(false);

    m_find->installEventFilter(this);
    m_replace->installEventFilter(this);
    connect(m_find, &QLineEdit::textChanged, this, [this] {
        if (!m_silent) {
            emit queryChanged();
        }
    });
    for (QToolButton* option : {m_case, m_word, m_regex}) {
        connect(option, &QToolButton::toggled, this, [this] {
            if (!m_silent) {
                emit queryChanged();
            }
        });
    }
    connect(m_expandReplace, &QToolButton::clicked, this, [this] { setReplaceVisible(!m_replaceVisible); });
    connect(m_previous, &QToolButton::clicked, this, &EditorFindBar::findPreviousRequested);
    connect(m_next, &QToolButton::clicked, this, &EditorFindBar::findNextRequested);
    connect(m_close, &QToolButton::clicked, this, &EditorFindBar::closeRequested);
    connect(m_replaceOne, &QPushButton::clicked, this, &EditorFindBar::replaceRequested);
    connect(m_replaceAll, &QPushButton::clicked, this, &EditorFindBar::replaceAllRequested);
}

void EditorFindBar::setReplaceVisible(bool visible) {
    m_replaceVisible = visible;
    m_replaceRow->setVisible(visible);
    if (m_expandReplace != nullptr) {
        m_expandReplace->setText(visible ? QStringLiteral("▾") : QStringLiteral("▸"));
        if (const FrostStyle* style = FrostStyle::current(); style != nullptr) {
            const QIcon chevron = style->icon(visible ? icons::kNavChevronDown : icons::kNavChevronRight);
            if (!chevron.isNull()) {
                m_expandReplace->setIcon(chevron);
            }
        }
    }
    adjustSize();
}

void EditorFindBar::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    const FrostStyle* style = FrostStyle::current();
    const QColor border = style != nullptr ? style->color(ColorKey::SeparatorColor) : palette().color(QPalette::Mid);
    painter.fillRect(QRect(0, height() - 1, width(), 1), border);
}

QString EditorFindBar::findText() const { return m_find->text(); }

QString EditorFindBar::replaceText() const { return m_replace->text(); }

text::SearchOptions EditorFindBar::options() const {
    return text::SearchOptions{m_case->isChecked(), m_word->isChecked(), m_regex->isChecked()};
}

void EditorFindBar::setFindText(const QString& text) {
    m_silent = true;
    m_find->setText(text);
    m_silent = false;
}

void EditorFindBar::setOptions(text::SearchOptions options) {
    m_silent = true;
    m_case->setChecked(options.caseSensitive);
    m_word->setChecked(options.wholeWord);
    m_regex->setChecked(options.regex);
    m_silent = false;
}

void EditorFindBar::focusFind() {
    m_find->setFocus(Qt::ShortcutFocusReason);
    m_find->selectAll();
}

void EditorFindBar::focusReplace() {
    m_replace->setFocus(Qt::ShortcutFocusReason);
    m_replace->selectAll();
}

void EditorFindBar::showMatches(std::size_t current, std::size_t total) {
    if (total == 0) {
        m_matches->setText(QStringLiteral("0 results"));
    } else if (current == 0) {
        m_matches->setText(QStringLiteral("%1 results").arg(total));
    } else {
        m_matches->setText(QStringLiteral("%1/%2").arg(current).arg(total));
    }
}

void EditorFindBar::showError(const QString& message) { m_matches->setText(message); }

QString EditorFindBar::matchText() const { return m_matches->text(); }

bool EditorFindBar::eventFilter(QObject* watched, QEvent* event) {
    if ((watched != m_find && watched != m_replace) ||
        (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)) {
        return QWidget::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    const Qt::KeyboardModifiers mods = key->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    const bool shift = (mods & Qt::ShiftModifier) != 0;
    const bool ctrlOnly = mods == Qt::ControlModifier;
    const bool altOnly = mods == Qt::AltModifier;
    const bool noMods = mods == Qt::NoModifier;

    const auto handledKey = [&]() -> bool {
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_F3:
        case Qt::Key_Escape: return true;
        case Qt::Key_Down:
        case Qt::Key_Up: return watched == m_find && noMods;
        case Qt::Key_F:
        case Qt::Key_R:
        case Qt::Key_H: return ctrlOnly;
        case Qt::Key_C:
        case Qt::Key_W:
        case Qt::Key_X: return altOnly;
        case Qt::Key_P:
        case Qt::Key_A: return altOnly && m_replaceVisible;
        default: return false;
        }
    }();
    if (!handledKey) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::ShortcutOverride) {
        key->accept();
        return true;
    }

    switch (key->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (watched == m_replace) {
            emit replaceRequested();
        } else if (shift) {
            emit findPreviousRequested();
        } else {
            emit findNextRequested();
        }
        return true;
    case Qt::Key_F3:
        if (shift) {
            emit findPreviousRequested();
        } else {
            emit findNextRequested();
        }
        return true;
    case Qt::Key_Down: emit findNextRequested(); return true;
    case Qt::Key_Up: emit findPreviousRequested(); return true;
    case Qt::Key_F: focusFind(); return true;
    case Qt::Key_R:
    case Qt::Key_H: emit replaceModeRequested(); return true;
    case Qt::Key_C: m_case->toggle(); return true;
    case Qt::Key_W: m_word->toggle(); return true;
    case Qt::Key_X: m_regex->toggle(); return true;
    case Qt::Key_P: emit replaceRequested(); return true;
    case Qt::Key_A: emit replaceAllRequested(); return true;
    case Qt::Key_Escape: emit closeRequested(); return true;
    default: return QWidget::eventFilter(watched, event);
    }
}

} // namespace frost::ui
