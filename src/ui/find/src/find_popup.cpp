// SPDX-License-Identifier: Apache-2.0
//
// FindPopup (frost/ui/find/find_popup.hpp): the Find and Replace in Files popup's widgets, keys and
// result list.

#include "frost/ui/find/find_popup.hpp"

#include "frost/io/path.hpp"

#include "find_util.hpp"
#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>

namespace frost::ui {

using find_detail::fromUtf8;
using find_detail::label;
using find_detail::toUtf8;

namespace {

constexpr std::array kScopeOrder{search::ScopeKind::Solution, search::ScopeKind::Project, search::ScopeKind::Directory,
                                 search::ScopeKind::OpenFiles};

QToolButton* toggleButton(QWidget* parent, const QString& text, const QString& tip, const char* name) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setText(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// A regex that names a line break searches across lines (FindQuery::multiline); so does a pasted
// pattern that contains one.
bool wantsMultiline(const std::string& pattern, bool regex) {
    if (pattern.find('\n') != std::string::npos) {
        return true;
    }
    return regex && (pattern.find("\\n") != std::string::npos || pattern.find("\\r") != std::string::npos);
}

} // namespace

FindPopup::FindPopup(QWidget* owner, FindSessionServices services, const FindSettings& settings, bool solutionOpen)
    : Popup(owner), m_services(std::move(services)), m_base(settings), m_replaceMode(settings.replaceMode) {
    setObjectName(QStringLiteral("findInFilesPopup"));
    m_session = new FindSession(m_services, this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &FindPopup::searchNow);
    buildUi(settings, solutionOpen);
    connectSession();
    updateButtons();
}

FindPopup::~FindPopup() = default;

void FindPopup::buildUi(const FindSettings& settings, bool solutionOpen) {
    m_filling = true;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Title, file mask.
    auto* header = new QHBoxLayout();
    m_title = new QLabel(m_replaceMode ? label("Replace in Files") : label("Find in Files"), this);
    m_title->setObjectName(QStringLiteral("popupTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    header->addWidget(m_title);
    header->addStretch(1);
    m_maskEnabled = new QCheckBox(label("File mask:"), this);
    m_maskEnabled->setObjectName(QStringLiteral("findFileMaskEnabled"));
    m_maskEnabled->setChecked(settings.fileMaskEnabled);
    m_maskEnabled->setFocusPolicy(Qt::NoFocus);
    m_mask = new QLineEdit(fromUtf8(settings.query.fileMask), this);
    m_mask->setObjectName(QStringLiteral("findFileMask"));
    m_mask->setPlaceholderText(QStringLiteral("*.cpp, *.h, !*.generated.h"));
    m_mask->setEnabled(settings.fileMaskEnabled);
    m_mask->setMaximumWidth(220);
    header->addWidget(m_maskEnabled);
    header->addWidget(m_mask);
    layout->addLayout(header);

    // Scopes.
    auto* scopes = new QHBoxLayout();
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    const std::array names{label("In Solution"), label("In Project"), label("Directory"), label("Open Files")};
    const std::array objectNames{"findScopeSolution", "findScopeProject", "findScopeDirectory", "findScopeOpenFiles"};
    for (std::size_t i = 0; i < kScopeOrder.size(); ++i) {
        QToolButton* button = toggleButton(this, names[i], names[i], objectNames[i]);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        group->addButton(button);
        scopes->addWidget(button);
        m_scopes[i] = button;
    }
    const bool solutionScope =
        settings.scope.kind == search::ScopeKind::Solution || settings.scope.kind == search::ScopeKind::Project;
    m_scopes[0]->setEnabled(solutionOpen);
    m_scopes[1]->setEnabled(solutionOpen && !settings.scope.currentFile.empty());
    search::ScopeKind initial = settings.scope.kind;
    if (solutionScope && !solutionOpen) {
        initial = search::ScopeKind::Directory;
    }
    if (initial == search::ScopeKind::Project && !m_scopes[1]->isEnabled()) {
        initial = search::ScopeKind::Solution;
    }
    scopeButton(initial)->setChecked(true);
    m_directory = new QLineEdit(fromUtf8(settings.scope.directory), this);
    m_directory->setObjectName(QStringLiteral("findDirectory"));
    m_directory->setPlaceholderText(label("Directory to search"));
    m_directory->setVisible(initial == search::ScopeKind::Directory);
    scopes->addWidget(m_directory, 1);
    scopes->addStretch(0);
    layout->addLayout(scopes);

    // Query with its toggles.
    auto* queryRow = new QHBoxLayout();
    m_query = new QLineEdit(fromUtf8(settings.query.pattern), this);
    m_query->setObjectName(QStringLiteral("findQuery"));
    m_query->setPlaceholderText(label("Search text"));
    m_query->setClearButtonEnabled(true);
    m_query->selectAll();
    queryRow->addWidget(m_query, 1);
    m_case = toggleButton(this, QStringLiteral("Cc"), label("Match Case"), "findMatchCase");
    m_words = toggleButton(this, QStringLiteral("W"), label("Words"), "findWords");
    m_regex = toggleButton(this, QStringLiteral(".*"), label("Regex"), "findRegex");
    m_case->setChecked(settings.query.options.caseSensitive);
    m_words->setChecked(settings.query.options.wholeWord);
    m_regex->setChecked(settings.query.options.regex);
    queryRow->addWidget(m_case);
    queryRow->addWidget(m_words);
    queryRow->addWidget(m_regex);
    layout->addLayout(queryRow);

    m_replacement = new QLineEdit(fromUtf8(settings.replacement), this);
    m_replacement->setObjectName(QStringLiteral("findReplacement"));
    m_replacement->setPlaceholderText(label("Replace with"));
    m_replacement->setVisible(m_replaceMode);
    layout->addWidget(m_replacement);

    // Results and the preview.
    m_results = new QListWidget(this);
    m_results->setObjectName(QStringLiteral("findResults"));
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setUniformItemSizes(true);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setItemDelegate(new find_detail::MatchItemDelegate(m_results));
    layout->addWidget(m_results, 3);
    m_previewTitle = new QLabel(this);
    m_previewTitle->setObjectName(QStringLiteral("findPreviewTitle"));
    layout->addWidget(m_previewTitle);
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("findPreview"));
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_preview, 2);

    // Status and buttons.
    auto* footer = new QHBoxLayout();
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("findStatus"));
    footer->addWidget(m_status, 1);
    m_openInWindow = new QPushButton(label("Open in Find Window"), this);
    m_openInWindow->setObjectName(QStringLiteral("findOpenInWindow"));
    m_openInWindow->setFocusPolicy(Qt::NoFocus);
    m_openInWindow->setToolTip(QStringLiteral("Ctrl+Enter"));
    footer->addWidget(m_openInWindow);
    m_replaceOne = new QPushButton(label("Replace"), this);
    m_replaceOne->setObjectName(QStringLiteral("findReplaceOne"));
    m_replaceOne->setFocusPolicy(Qt::NoFocus);
    m_replaceOne->setVisible(m_replaceMode);
    m_replaceAll = new QPushButton(label("Replace All"), this);
    m_replaceAll->setObjectName(QStringLiteral("findReplaceAll"));
    m_replaceAll->setFocusPolicy(Qt::NoFocus);
    m_replaceAll->setVisible(m_replaceMode);
    footer->addWidget(m_replaceOne);
    footer->addWidget(m_replaceAll);
    layout->addLayout(footer);

    setFocusProxy(m_query);
    for (QWidget* field :
         {static_cast<QWidget*>(m_query), static_cast<QWidget*>(m_replacement), static_cast<QWidget*>(m_mask),
          static_cast<QWidget*>(m_directory), static_cast<QWidget*>(m_results)}) {
        field->installEventFilter(this);
    }
    setMinimumSize(760, 540);

    for (QLineEdit* field : {m_query, m_mask, m_directory}) {
        connect(field, &QLineEdit::textChanged, this, &FindPopup::scheduleSearch);
    }
    for (QToolButton* toggle : {m_case, m_words, m_regex}) {
        connect(toggle, &QToolButton::toggled, this, &FindPopup::scheduleSearch);
    }
    connect(m_maskEnabled, &QCheckBox::toggled, this, [this](bool on) {
        m_mask->setEnabled(on);
        scheduleSearch();
    });
    connect(group, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool checked) {
        if (checked) {
            m_directory->setVisible(scopeButton(search::ScopeKind::Directory)->isChecked());
            scheduleSearch();
        }
    });
    connect(m_directory, &QLineEdit::textEdited, this, [this] { m_base.directoryExplicit = true; });
    for (std::size_t i = 0; i < m_scopes.size(); ++i) {
        connect(m_scopes[i], &QToolButton::clicked, this, [this, i] {
            m_base.scopeExplicit = true;
            m_base.preferredScope = kScopeOrder[i];
        });
    }
    // A changed replacement keeps the matches; only the plan differs.
    connect(m_replacement, &QLineEdit::textChanged, this, &FindPopup::updateButtons);
    connect(m_results, &QListWidget::currentRowChanged, this, [this] {
        updatePreview();
        updateButtons();
    });
    connect(m_results, &QListWidget::itemDoubleClicked, this, [this] { static_cast<void>(activateCurrent()); });
    connect(m_results, &QListWidget::itemChanged, this, &FindPopup::onItemChanged);
    connect(m_openInWindow, &QPushButton::clicked, this, &FindPopup::openInFindWindowRequested);
    connect(m_replaceOne, &QPushButton::clicked, this, [this] { emit replaceRequested(false); });
    connect(m_replaceAll, &QPushButton::clicked, this, [this] { emit replaceRequested(true); });
    m_filling = false;
}

void FindPopup::connectSession() {
    connect(m_session, &FindSession::cleared, this, &FindPopup::onCleared);
    connect(m_session, &FindSession::filesAdded, this, &FindPopup::onFilesAdded);
    connect(m_session, &FindSession::finished, this, &FindPopup::onFinished);
}

QToolButton* FindPopup::scopeButton(search::ScopeKind kind) const noexcept {
    const auto it = std::ranges::find(kScopeOrder, kind);
    return it == kScopeOrder.end() ? nullptr : m_scopes[static_cast<std::size_t>(it - kScopeOrder.begin())];
}

FindSettings FindPopup::settings() const {
    FindSettings settings = m_base;
    settings.query.pattern = toUtf8(m_query->text());
    settings.query.options.caseSensitive = m_case->isChecked();
    settings.query.options.wholeWord = m_words->isChecked();
    settings.query.options.regex = m_regex->isChecked();
    settings.query.multiline = wantsMultiline(settings.query.pattern, settings.query.options.regex);
    settings.query.fileMask = toUtf8(m_mask->text());
    settings.fileMaskEnabled = m_maskEnabled->isChecked();
    for (std::size_t i = 0; i < kScopeOrder.size(); ++i) {
        if (m_scopes[i]->isChecked()) {
            settings.scope.kind = kScopeOrder[i];
        }
    }
    settings.scope.directory = toUtf8(m_directory->text().trimmed());
    settings.replaceMode = m_replaceMode;
    settings.replacement = toUtf8(m_replacement->text());
    return settings;
}

void FindPopup::scheduleSearch() {
    if (!m_filling) {
        m_searchPending = true;
        m_debounce->start();
        updateButtons();
        updatePreview();
    }
}

void FindPopup::searchNow() {
    m_debounce->stop();
    if (m_session == nullptr) {
        return;
    }
    m_searchedOnce = true;
    m_searchPending = false;
    m_session->start(settings());
}

FindSession* FindPopup::takeSession() {
    FindSession* session = m_session;
    if (session != nullptr) {
        disconnect(session, nullptr, this, nullptr);
        session->setParent(nullptr);
        m_session = nullptr;
    }
    m_debounce->stop();
    m_results->clear();
    updateButtons();
    return session;
}

std::optional<std::pair<std::size_t, std::size_t>> FindPopup::currentMatch() const {
    const QListWidgetItem* item = m_results->currentItem();
    if (item == nullptr || m_session == nullptr || m_searchPending) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::size_t>(item->data(find_detail::kFileIndexRole).toInt()),
                     static_cast<std::size_t>(item->data(find_detail::kMatchIndexRole).toInt())};
}

bool FindPopup::activateCurrent() {
    const auto current = currentMatch();
    if (!current) {
        return false;
    }
    const FindLocation location = m_session->location(current->first, current->second);
    closePopup(CloseReason::Accepted);
    emit locationActivated(location);
    return true;
}

void FindPopup::showEvent(QShowEvent* event) {
    Popup::showEvent(event);
    if (!m_searchedOnce && !m_query->text().isEmpty()) {
        searchNow();
    }
}

bool FindPopup::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return Popup::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    const bool inField = watched != m_results;
    switch (key->key()) {
    case Qt::Key_Escape: closePopup(CloseReason::Cancelled); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if ((key->modifiers() & Qt::ControlModifier) != 0) {
            emit openInFindWindowRequested();
        } else {
            static_cast<void>(activateCurrent());
        }
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        if (!inField) {
            return false;
        }
        const int page = std::max(1, m_results->height() / std::max(1, m_results->sizeHintForRow(0)));
        const int step = key->key() == Qt::Key_Up       ? -1
                         : key->key() == Qt::Key_Down   ? 1
                         : key->key() == Qt::Key_PageUp ? -page
                                                        : page;
        moveSelection(step);
        return true;
    }
    default: return false;
    }
}

void FindPopup::moveSelection(int delta) {
    const int count = m_results->count();
    if (count == 0) {
        return;
    }
    const int current = m_results->currentRow();
    m_results->setCurrentRow(std::clamp(current < 0 ? 0 : current + delta, 0, count - 1));
}

void FindPopup::onCleared() {
    m_filling = true;
    m_results->clear();
    m_filling = false;
    updatePreview();
    m_status->setText(m_session != nullptr ? m_session->statusText() : QString());
    updateButtons();
}

void FindPopup::onFilesAdded(int first, int count) {
    m_filling = true;
    const std::vector<search::FileMatches>& files = m_session->files();
    for (int f = first; f < first + count; ++f) {
        const search::FileMatches& file = files[static_cast<std::size_t>(f)];
        const QString name = fromUtf8(io::fileName(file.path));
        for (std::size_t m = 0; m < file.matches.size(); ++m) {
            const search::FindMatch& match = file.matches[m];
            const find_detail::RowText row = find_detail::rowText(match);
            auto* item = new QListWidgetItem(row.text);
            item->setData(find_detail::kFileIndexRole, f);
            item->setData(find_detail::kMatchIndexRole, static_cast<int>(m));
            item->setData(find_detail::kHighlightStartRole, row.highlightStart);
            item->setData(find_detail::kHighlightLengthRole, row.highlightLength);
            item->setData(find_detail::kDetailRole, QStringLiteral("%1 %2").arg(name).arg(match.line + 1));
            item->setToolTip(fromUtf8(file.path));
            if (m_replaceMode) {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(m_session->isIncluded(static_cast<std::size_t>(f), m) ? Qt::Checked
                                                                                          : Qt::Unchecked);
            }
            m_results->addItem(item);
        }
    }
    m_filling = false;
    if (m_results->currentRow() < 0 && m_results->count() > 0) {
        m_results->setCurrentRow(0);
    }
    m_status->setText(m_session->statusText());
    updateButtons();
}

void FindPopup::onFinished() {
    if (m_session != nullptr) {
        m_status->setText(m_session->statusText());
    }
    updateButtons();
}

void FindPopup::onItemChanged(QListWidgetItem* item) {
    if (m_filling || m_session == nullptr || !m_replaceMode) {
        return;
    }
    m_session->setIncluded(static_cast<std::size_t>(item->data(find_detail::kFileIndexRole).toInt()),
                           static_cast<std::size_t>(item->data(find_detail::kMatchIndexRole).toInt()),
                           item->checkState() == Qt::Checked);
    updateButtons();
}

void FindPopup::updatePreview() {
    m_preview->clear();
    m_previewTitle->clear();
    const auto current = currentMatch();
    if (!current) {
        return;
    }
    const search::FileMatches& file = m_session->files()[current->first];
    const search::FindMatch& match = file.matches[current->second];
    m_previewTitle->setText(QStringLiteral("%1:%2").arg(fromUtf8(file.path)).arg(match.line + 1));
    QStringList lines;
    for (const std::string& line : match.before) {
        lines.push_back(fromUtf8(line));
    }
    lines.push_back(fromUtf8(match.lineText));
    for (const std::string& line : match.after) {
        lines.push_back(fromUtf8(line));
    }
    m_preview->setPlainText(lines.join(QLatin1Char('\n')));
    // Highlight the match on its line (the line comes after `before`).
    const QTextBlock block = m_preview->document()->findBlockByNumber(static_cast<int>(match.before.size()));
    if (!block.isValid()) {
        return;
    }
    const std::string_view text = match.lineText;
    const std::size_t column = std::min<std::size_t>(match.previewColumn, text.size());
    const std::size_t length = std::min<std::size_t>(match.previewLength, text.size() - column);
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + static_cast<int>(fromUtf8(text.substr(0, column)).size()));
    cursor.setPosition(cursor.position() + static_cast<int>(fromUtf8(text.substr(column, length)).size()),
                       QTextCursor::KeepAnchor);
    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;
    selection.format.setBackground(find_detail::matchBackground());
    selection.format.setFontWeight(QFont::Bold);
    m_preview->setExtraSelections({selection});
    m_preview->setTextCursor(QTextCursor(block));
    m_preview->centerCursor();
}

void FindPopup::updateButtons() {
    const bool hasResults = m_session != nullptr && !m_searchPending && !m_session->files().empty();
    m_openInWindow->setEnabled(hasResults);
    const bool idle = m_session != nullptr && !m_session->running();
    m_replaceOne->setEnabled(m_replaceMode && idle && currentMatch().has_value());
    m_replaceAll->setEnabled(m_replaceMode && idle && hasResults && m_session->includedCount() > 0);
}

} // namespace frost::ui
