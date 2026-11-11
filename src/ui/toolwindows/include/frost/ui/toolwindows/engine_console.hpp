// SPDX-License-Identifier: Apache-2.0
// Live, bounded engine log with source links and commands delegated to the app's schema policy.
#pragma once

#include "frost/ui/toolwindows/build_output.hpp"

#include <QString>
#include <QWidget>

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace frost::ui {

enum class EngineLogLevel { Trace, Debug, Info, Warning, Error, Critical };

// The app converts real pushed events to this display value; locations are engine-reported,
// resolved and validated by the app. No timestamp or source position is invented by the widget.
struct EngineLogEntry {
    EngineLogLevel level = EngineLogLevel::Info;
    QString logger;
    QString message;
    QString timestamp;
    std::vector<SourceLocation> locations;
};

// Parent owns this widget; GUI thread only. All rows, including hidden rows and source frames,
// share the entry/UTF-16-byte limits. Append is O(entry size + evictions); filtering batches once per event-loop turn
// in O(rows). CommandHandler is borrowed by capture: clear it before its services die. It MUST authorize each
// method/params through the current session policy; this UI never sends a transport request.
class EngineConsole final : public QWidget {
    Q_OBJECT
public:
    using CommandHandler = std::function<void(const QString& method, const QString& paramsJson)>;
    explicit EngineConsole(QWidget* parent = nullptr, std::size_t maxEntries = 5000,
                           std::size_t maxBytes = 4U * 1024U * 1024U);
    ~EngineConsole() override;
    // Returns false for an oversized entry, still accounting for its eviction in droppedEntries().
    // Existing retained entries and their exact locations are never truncated.
    bool append(EngineLogEntry entry);
    void clear();
    void setConnected(bool connected);
    void setCommandHandler(CommandHandler handler);
    // UI syntax checks only. The handler enforces schema policy and surfaces real results/errors.
    // Returns false when disconnected, no handler, missing method, or params is not a JSON object.
    bool submitCommand(const QString& method, const QString& paramsJson = QStringLiteral("{}"));
    // Pass actual pushed playing state. Clear-on-play applies on false -> true only.
    void setPlaying(bool playing);
    void setClearOnPlay(bool enabled);
    void setAutoScroll(bool enabled);
    void setCollapseDuplicates(bool enabled);
    [[nodiscard]] std::size_t retainedEntries() const noexcept { return m_records.size(); }
    [[nodiscard]] std::size_t retainedBytes() const noexcept { return m_bytes; }
    [[nodiscard]] std::size_t droppedEntries() const noexcept { return m_dropped; }
    [[nodiscard]] QTreeWidget* view() const noexcept { return m_view; }
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_search; }
    [[nodiscard]] QLineEdit* loggerField() const noexcept { return m_logger; }
    [[nodiscard]] QComboBox* levelFilter() const noexcept { return m_level; }
    // Opens a selected reported frame, or the first frame of a selected entry.
    bool activate(QTreeWidgetItem* item);
signals:
    void openLocation(const QString& path, int line, int column);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Record {
        EngineLogEntry entry;
        QTreeWidgetItem* item = nullptr;
        std::size_t bytes = 0;
        std::size_t repeats = 1;
    };
    void updateFilters();
    void scheduleRefresh();
    void updateCommandEnabled();
    void updateRow(Record& record);
    void updateDroppedText();
    void copySelection();
    QTreeWidget* m_view = nullptr;
    QLineEdit* m_search = nullptr;
    QLineEdit* m_logger = nullptr;
    QLineEdit* m_method = nullptr;
    QLineEdit* m_params = nullptr;
    QComboBox* m_level = nullptr;
    QCheckBox* m_regex = nullptr;
    QCheckBox* m_autoscroll = nullptr;
    QCheckBox* m_collapse = nullptr;
    QCheckBox* m_clearOnPlay = nullptr;
    QPushButton* m_send = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_droppedText = nullptr;
    CommandHandler m_handler;
    std::deque<Record> m_records;
    std::size_t m_maxEntries;
    std::size_t m_maxBytes;
    std::size_t m_bytes = 0;
    std::size_t m_dropped = 0;
    bool m_refreshQueued = false;
    bool m_connected = false;
    bool m_playing = false;
};

} // namespace frost::ui
