// SPDX-License-Identifier: Apache-2.0
// Engine Console: bounded live logs and explicit app-authorized commands, without engine polling.
#include "frost/ui/toolwindows/engine_console.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <utility>

namespace frost::ui {
namespace {
QString levelName(EngineLogLevel level) {
    switch (level) {
    case EngineLogLevel::Trace: return EngineConsole::tr("Trace");
    case EngineLogLevel::Debug: return EngineConsole::tr("Debug");
    case EngineLogLevel::Info: return EngineConsole::tr("Info");
    case EngineLogLevel::Warning: return EngineConsole::tr("Warning");
    case EngineLogLevel::Error: return EngineConsole::tr("Error");
    case EngineLogLevel::Critical: return EngineConsole::tr("Critical");
    }
    return {};
}
QString qs(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
bool sameEntry(const EngineLogEntry& a, const EngineLogEntry& b) {
    return a.level == b.level && a.logger == b.logger && a.message == b.message && a.locations == b.locations;
}
} // namespace

EngineConsole::EngineConsole(QWidget* parent, std::size_t maxEntries, std::size_t maxBytes)
    : QWidget(parent), m_maxEntries(std::max(std::size_t{1}, maxEntries)), m_maxBytes(maxBytes) {
    setObjectName(QStringLiteral("engineConsole"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* filters = new QHBoxLayout;
    m_level = new QComboBox(this);
    m_level->setObjectName(QStringLiteral("engineLogLevel"));
    m_level->setAccessibleName(tr("Minimum log level"));
    for (const EngineLogLevel level : {EngineLogLevel::Trace, EngineLogLevel::Debug, EngineLogLevel::Info,
                                       EngineLogLevel::Warning, EngineLogLevel::Error, EngineLogLevel::Critical})
        m_level->addItem(levelName(level), static_cast<int>(level));
    m_logger = new QLineEdit(this);
    m_logger->setObjectName(QStringLiteral("engineLogLogger"));
    m_logger->setPlaceholderText(tr("Logger"));
    m_logger->setAccessibleName(tr("Filter logger"));
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("engineLogSearch"));
    m_search->setPlaceholderText(tr("Search engine logs"));
    m_search->setAccessibleName(tr("Search engine logs"));
    m_regex = new QCheckBox(tr("Regex"), this);
    m_regex->setObjectName(QStringLiteral("engineLogRegex"));
    filters->addWidget(m_level);
    filters->addWidget(m_logger);
    filters->addWidget(m_search, 1);
    filters->addWidget(m_regex);
    layout->addLayout(filters);
    auto* options = new QHBoxLayout;
    m_autoscroll = new QCheckBox(tr("Autoscroll"), this);
    m_autoscroll->setObjectName(QStringLiteral("engineLogAutoscroll"));
    m_autoscroll->setChecked(true);
    m_collapse = new QCheckBox(tr("Collapse duplicates"), this);
    m_collapse->setObjectName(QStringLiteral("engineLogCollapse"));
    m_clearOnPlay = new QCheckBox(tr("Clear on Play"), this);
    m_clearOnPlay->setObjectName(QStringLiteral("engineLogClearOnPlay"));
    auto* clearButton = new QPushButton(tr("Clear"), this);
    clearButton->setObjectName(QStringLiteral("engineLogClear"));
    auto* copyButton = new QPushButton(tr("Copy"), this);
    copyButton->setObjectName(QStringLiteral("engineLogCopy"));
    m_droppedText = new QLabel(this);
    m_droppedText->setTextFormat(Qt::PlainText);
    options->addWidget(m_autoscroll);
    options->addWidget(m_collapse);
    options->addWidget(m_clearOnPlay);
    options->addWidget(clearButton);
    options->addWidget(copyButton);
    options->addWidget(m_droppedText, 1);
    layout->addLayout(options);
    m_view = new QTreeWidget(this);
    m_view->setObjectName(QStringLiteral("engineLogView"));
    m_view->setHeaderLabels({tr("Time"), tr("Level"), tr("Logger"), tr("Message")});
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_view->setAccessibleDescription(
        tr("Enter or F4 opens a reported source location. Expand a log to see all frames."));
    m_view->installEventFilter(this);
    layout->addWidget(m_view, 1);
    auto* command = new QHBoxLayout;
    m_method = new QLineEdit(this);
    m_method->setObjectName(QStringLiteral("engineCommandMethod"));
    m_method->setPlaceholderText(tr("Command method"));
    m_method->setAccessibleName(tr("Engine command method"));
    m_params = new QLineEdit(QStringLiteral("{}"), this);
    m_params->setObjectName(QStringLiteral("engineCommandParams"));
    m_params->setAccessibleName(tr("Engine command parameters as JSON object"));
    m_send = new QPushButton(tr("Send"), this);
    m_send->setObjectName(QStringLiteral("engineCommandSend"));
    command->addWidget(m_method);
    command->addWidget(m_params, 1);
    command->addWidget(m_send);
    layout->addLayout(command);
    m_status = new QLabel(tr("Engine disconnected"), this);
    m_status->setObjectName(QStringLiteral("engineCommandStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_level, &QComboBox::currentIndexChanged, this, [this] { updateFilters(); });
    connect(m_logger, &QLineEdit::textChanged, this, [this] { updateFilters(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { updateFilters(); });
    connect(m_regex, &QCheckBox::toggled, this, [this] { updateFilters(); });
    connect(m_collapse, &QCheckBox::toggled, this, [this] { updateFilters(); });
    connect(clearButton, &QPushButton::clicked, this, &EngineConsole::clear);
    connect(copyButton, &QPushButton::clicked, this, &EngineConsole::copySelection);
    connect(m_view, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { activate(item); });
    const auto send = [this] { submitCommand(m_method->text(), m_params->text()); };
    connect(m_send, &QPushButton::clicked, this, send);
    connect(m_method, &QLineEdit::returnPressed, this, send);
    connect(m_params, &QLineEdit::returnPressed, this, send);
    updateCommandEnabled();
}

EngineConsole::~EngineConsole() = default;

bool EngineConsole::append(EngineLogEntry entry) {
    // Do not retain a caller's oversized reserved capacity beyond the bounded contents.
    entry.message.squeeze();
    entry.logger.squeeze();
    entry.timestamp.squeeze();
    entry.locations.shrink_to_fit();
    for (auto& location : entry.locations)
        location.path.shrink_to_fit();
    // Account the actual retained UTF-16 buffers plus frame paths and row metadata. Empty strings
    // cannot bypass the count limit; oversized entries are dropped whole to preserve source links.
    std::size_t bytes = sizeof(Record);
    const auto addBytes = [&bytes, this](std::size_t n) {
        if (n > m_maxBytes || bytes > m_maxBytes - n) {
            bytes = std::numeric_limits<std::size_t>::max();
            return;
        }
        bytes += n;
    };
    for (const auto* text : {&entry.message, &entry.logger, &entry.timestamp})
        addBytes(static_cast<std::size_t>(text->size()) * sizeof(QChar));
    for (const auto& location : entry.locations)
        addBytes(sizeof(SourceLocation) + location.path.size());
    if (bytes > m_maxBytes) {
        ++m_dropped;
        updateDroppedText();
        return false;
    }
    while (!m_records.empty() && (m_records.size() >= m_maxEntries || m_bytes > m_maxBytes - bytes)) {
        m_bytes -= m_records.front().bytes;
        delete m_records.front().item;
        m_records.pop_front();
        ++m_dropped;
    }
    // Every pushed event remains an individually bounded record. Collapse only affects display,
    // so unchecking it restores timestamps, source frames and the complete retained stream.
    m_bytes += bytes;
    m_records.push_back(Record{std::move(entry), new QTreeWidgetItem(m_view), bytes, 1});
    Record& record = m_records.back();
    for (const auto& location : record.entry.locations) {
        auto* frame = new QTreeWidgetItem(record.item);
        frame->setText(3, qs(location.path) + QStringLiteral(":") + QString::number(location.line));
        frame->setData(0, Qt::UserRole, qs(location.path));
        frame->setData(0, Qt::UserRole + 1, location.line);
        frame->setData(0, Qt::UserRole + 2, location.column);
    }
    updateRow(record);
    scheduleRefresh();
    updateDroppedText();
    return true;
}

void EngineConsole::scheduleRefresh() {
    if (m_refreshQueued)
        return;
    m_refreshQueued = true;
    // This is a single coalesced display update, never an engine poll. QObject ownership cancels
    // the queued callback when a project switch destroys the console.
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_refreshQueued = false;
            updateFilters();
            if (m_autoscroll->isChecked())
                m_view->scrollToBottom();
        },
        Qt::QueuedConnection);
}

void EngineConsole::updateRow(Record& record) {
    record.item->setText(0, record.entry.timestamp);
    record.item->setText(1, levelName(record.entry.level));
    record.item->setText(2, record.entry.logger);
    const QString suffix = record.repeats > 1 ? tr(" (x%1)").arg(static_cast<qulonglong>(record.repeats)) : QString();
    record.item->setText(3, record.entry.message + suffix);
}

void EngineConsole::updateFilters() {
    const QRegularExpression regex(m_search->text(), QRegularExpression::CaseInsensitiveOption);
    const bool valid = !m_regex->isChecked() || regex.isValid();
    m_search->setToolTip(valid ? QString() : regex.errorString());
    Record* previous = nullptr;
    for (auto& record : m_records) {
        record.repeats = 1;
        const auto& entry = record.entry;
        const QString searchable = entry.logger + QStringLiteral(" ") + entry.message;
        const bool matches = valid && static_cast<int>(entry.level) >= m_level->currentData().toInt() &&
                             entry.logger.contains(m_logger->text(), Qt::CaseInsensitive) &&
                             (m_regex->isChecked() ? regex.match(searchable).hasMatch()
                                                   : searchable.contains(m_search->text(), Qt::CaseInsensitive));
        const bool collapsed = matches && previous && m_collapse->isChecked() && sameEntry(previous->entry, entry);
        record.item->setHidden(!matches || collapsed);
        updateRow(record);
        if (collapsed) {
            ++previous->repeats;
            updateRow(*previous);
        } else {
            previous = matches ? &record : nullptr;
        }
    }
}

void EngineConsole::clear() {
    m_records.clear();
    m_view->clear();
    m_bytes = 0;
    m_dropped = 0;
    updateDroppedText();
}
void EngineConsole::setConnected(bool connected) {
    m_connected = connected;
    m_status->setText(connected ? tr("Commands are checked by the engine session policy") : tr("Engine disconnected"));
    updateCommandEnabled();
}
void EngineConsole::setCommandHandler(CommandHandler handler) {
    m_handler = std::move(handler);
    updateCommandEnabled();
}
void EngineConsole::updateCommandEnabled() {
    const bool enabled = m_connected && static_cast<bool>(m_handler);
    m_method->setEnabled(enabled);
    m_params->setEnabled(enabled);
    m_send->setEnabled(enabled);
}
bool EngineConsole::submitCommand(const QString& method, const QString& paramsJson) {
    if (!m_connected || !m_handler)
        return false;
    QJsonParseError error;
    const QJsonDocument params = QJsonDocument::fromJson(paramsJson.toUtf8(), &error);
    if (method.trimmed().isEmpty() || error.error != QJsonParseError::NoError || !params.isObject()) {
        m_status->setText(tr("Enter a command method and a valid JSON object for parameters"));
        return false;
    }
    // A local copy remains valid if the app clears the callback during a synchronous disconnect.
    const auto handler = m_handler;
    m_status->setText(tr("Command submitted to session policy"));
    handler(method.trimmed(), paramsJson);
    return true;
}
void EngineConsole::setPlaying(bool playing) {
    if (playing && !m_playing && m_clearOnPlay->isChecked())
        clear();
    m_playing = playing;
}
void EngineConsole::setClearOnPlay(bool enabled) { m_clearOnPlay->setChecked(enabled); }
void EngineConsole::setAutoScroll(bool enabled) { m_autoscroll->setChecked(enabled); }
void EngineConsole::setCollapseDuplicates(bool enabled) { m_collapse->setChecked(enabled); }
void EngineConsole::updateDroppedText() {
    m_droppedText->setText(m_dropped == 0
                               ? QString()
                               : tr("%1 older or oversized entries discarded").arg(static_cast<qulonglong>(m_dropped)));
}

bool EngineConsole::activate(QTreeWidgetItem* item) {
    if (!item || item->treeWidget() != m_view)
        return false;
    if (!item->parent()) {
        if (item->childCount() == 0)
            return false;
        item = item->child(0);
    }
    const QString path = item->data(0, Qt::UserRole).toString();
    const int line = item->data(0, Qt::UserRole + 1).toInt();
    if (path.isEmpty() || line <= 0)
        return false;
    emit openLocation(path, line, item->data(0, Qt::UserRole + 2).toInt());
    return true;
}
void EngineConsole::copySelection() {
    QStringList lines;
    for (const auto* item : m_view->selectedItems()) {
        QStringList columns;
        for (int column = 0; column < m_view->columnCount(); ++column)
            columns.push_back(item->text(column));
        lines.push_back(columns.join(QLatin1Char('\t')));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}
bool EngineConsole::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Copy)) {
            copySelection();
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F4) &&
            (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            activate(m_view->currentItem());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace frost::ui
