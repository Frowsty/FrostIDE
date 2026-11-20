// SPDX-License-Identifier: Apache-2.0
#include "unity_tests_window.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <set>
namespace frost::app {
namespace {
QString q(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
bool active(std::string_view state) {
    return state == "discovering" || state == "queued" || state == "running" || state == "cancelling";
}
struct RequestedOperation {
    bool run;
    std::string job, mode, owner, catalog;

    bool acknowledges(const UnityTestObservation& observed) const {
        if (observed.job != job || observed.mode != mode || (!owner.empty() && observed.owner != owner))
            return false;
        if (run) {
            return !owner.empty() && !catalog.empty() && observed.catalog == catalog && !observed.execution.empty() &&
                   (observed.state == "queued" || observed.state == "running" || observed.state == "cancelling" ||
                    observed.state == "finished");
        }
        return observed.execution.empty() &&
               ((observed.state == "discovered" && observed.catalog == job) ||
                ((observed.state == "discovering" || observed.state == "cancelled") && observed.catalog.empty()));
    }
};

class UnityTestsWidget final : public QWidget {
public:
    UnityTestsWidget(QWidget* parent, UnityTestsWindowContext context)
        : QWidget(parent), m_context(std::move(context)) {
        setObjectName("unityTestsWidget");
        auto* layout = new QVBoxLayout(this);
        auto* bar = new QHBoxLayout;
        m_mode = new QComboBox(this);
        m_mode->addItems({"EditMode", "PlayMode"});
        m_mode->setObjectName("unityTestsMode");
        bar->addWidget(m_mode);
        const auto button = [&](const QString& label, const char* name, auto callback) {
            auto* result = new QPushButton(label, this);
            result->setObjectName(name);
            bar->addWidget(result);
            connect(result, &QPushButton::clicked, this, callback);
            return result;
        };
        m_observe = button(tr("Observe"), "unityTestsObserve", [this] { observe(true); });
        m_discover = button(tr("Discover"), "unityTestsDiscover", [this] { start(false, {}); });
        m_all = button(tr("Run All"), "unityTestsRunAll", [this] { start(true, {}); });
        m_selected = button(tr("Run Selected"), "unityTestsRunSelected", [this] {
            std::set<int> rows;
            for (const auto* item : m_table->selectedItems())
                rows.insert(item->row());
            std::vector<std::string> ids;
            for (int row : rows)
                ids.push_back(m_rows.at(static_cast<size_t>(row)).id);
            if (ids.empty()) {
                message("Select actual discovered test rows");
                return;
            }
            start(true, std::move(ids));
        });
        m_failed = button(tr("Run Failed"), "unityTestsRunFailed", [this] {
            std::vector<std::string> ids;
            for (const auto& row : m_rows)
                if (row.outcome == "Failed")
                    ids.push_back(row.id);
            if (ids.empty()) {
                message("No actual failed tests are available");
                return;
            }
            start(true, std::move(ids));
        });
        m_stop = button(tr("Stop"), "unityTestsStop", [this] { cancelRun(); });
        layout->addLayout(bar);
        m_status = new QLabel(tr("Select a connected Unity editor in Unity Controls, then discover its actual EditMode "
                                 "or PlayMode tests."),
                              this);
        m_status->setWordWrap(true);
        layout->addWidget(m_status);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(3);
        m_table->setHorizontalHeaderLabels({tr("Unity test"), tr("Framework state"), tr("Seconds")});
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        layout->addWidget(m_table, 1);
        m_details = new QPlainTextEdit(this);
        m_details->setReadOnly(true);
        layout->addWidget(m_details, 1);
        connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
            const int at = m_table->currentRow();
            if (at >= 0 && static_cast<size_t>(at) < m_rows.size()) {
                const auto& row = m_rows[static_cast<size_t>(at)];
                m_details->setPlainText(q(row.message + "\n" + row.stack) +
                                        (row.truncated ? tr("\n[Framework detail truncated]") : QString()));
            }
        });
        connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            if (row < 0 || static_cast<size_t>(row) >= m_rows.size() || m_rows[static_cast<size_t>(row)].truncated ||
                !m_context.openLocation || !m_source)
                return;
            const QRegularExpression pattern("\\(at (.+):(\\d+)\\)");
            const auto match = pattern.match(q(m_rows[static_cast<size_t>(row)].stack));
            if (!match.hasMatch())
                return;
            const QFileInfo file(QDir(q(m_source->root)).absoluteFilePath(match.captured(1)));
            const auto canonical = file.canonicalFilePath();
            const auto relative = QDir(q(m_source->root)).relativeFilePath(canonical);
            if (canonical.isEmpty() || relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
                return;
            m_context.openLocation(canonical.toStdString(), match.captured(2).toInt());
        });
        connect(m_mode, &QComboBox::currentTextChanged, this, [this] {
            ++m_round;
            m_pending = false;
            m_requested.reset();
            m_rows.clear();
            m_table->setRowCount(0);
            m_catalog.clear();
            m_poll = false;
            m_owned = false;
            update();
        });
        m_timer.setInterval(250);
        connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
        m_timer.start();
        update();
    }

private:
    void message(std::string_view value) {
        m_status->setText(q(value));
        update();
    }
    std::optional<UnityEditorIdentity> selected() const {
        auto value = m_context.selected ? m_context.selected() : std::nullopt;
        if (!value || !value->tests || !value->epoch || value->epoch->load() != value->revision)
            return std::nullopt;
        return value;
    }
    bool sameDomain(const UnityEditorIdentity& left, const UnityEditorIdentity& right) const {
        return left.root == right.root && left.instance == right.instance && left.session == right.session &&
               left.generation == right.generation && left.revision == right.revision;
    }
    void update() {
        const auto source = selected();
        const bool ready = source.has_value() && !m_pending;
        m_observe->setEnabled(ready);
        m_discover->setEnabled(ready && !m_poll);
        m_all->setEnabled(ready && !m_poll && !m_catalog.empty() &&
                          m_catalogMode == m_mode->currentText().toStdString());
        m_selected->setEnabled(m_all->isEnabled());
        m_failed->setEnabled(m_all->isEnabled());
        m_stop->setEnabled(ready && m_owned && m_poll);
        m_mode->setEnabled(!m_pending && !m_poll);
    }
    void send(const lang::Json& request, std::function<void(const lang::Json&)> done) {
        if (m_pending) {
            message("A Unity test observation is pending");
            return;
        }
        const auto source = selected();
        if (!source || !m_context.request) {
            message("Select the exact trusted Unity editor with test protocol support");
            return;
        }
        m_source = source;
        m_pending = true;
        const auto round = ++m_round;
        QPointer<UnityTestsWidget> self(this);
        update();
        auto dispatch = m_context.request;
        const bool action = request.stringAt("op") == "run" || request.stringAt("op") == "discover";
        dispatch(*source, request,
                 [self, source = *source, round, action, done = std::move(done)](Result<lang::Json> response) {
                     if (!self || self->m_round != round)
                         return;
                     self->m_pending = false;
                     const auto current = self->selected();
                     if (!current || !self->sameDomain(*current, source)) {
                         if (action && self->m_requested)
                             self->m_poll = true;
                         self->message(
                             "Unity test domain changed; only actual matching owner/job observations can resume");
                         return;
                     }
                     if (!response) {
                         if (action) {
                             if (response.error().code() == ErrorCode::Busy ||
                                 response.error().code() == ErrorCode::Untrusted ||
                                 response.error().code() == ErrorCode::InvalidArgument ||
                                 response.error().code() == ErrorCode::OutOfRange)
                                 self->m_requested.reset();
                             else
                                 self->m_poll = true;
                         }
                         self->message(response.error().message());
                         return;
                     }
                     done(*response);
                     if (self)
                         self->update();
                 });
    }
    void observe(bool explicitRequest) {
        if (m_pending)
            return;
        const auto source = selected();
        if (!source) {
            message("Select a connected test-capable Unity editor");
            return;
        }
        if (m_source && (m_source->root != source->root || m_source->instance != source->instance)) {
            m_rows.clear();
            m_table->setRowCount(0);
            m_owner.clear();
            m_job.clear();
            m_catalog.clear();
            m_owned = false;
            m_poll = false;
            m_requested.reset();
        }
        send(lang::Json::object().set("op", "observe"), [this, explicitRequest](const lang::Json& json) {
            auto observed = parseUnityTestState(json);
            if (!observed) {
                if (m_requested)
                    rejectReceipt(observed.error().message());
                else
                    message(observed.error().message());
                return;
            }
            if (m_owned && !m_execution.empty() && observed->owner == m_owner && observed->job == m_job &&
                observed->execution != m_execution) {
                m_poll = false;
                m_owned = false;
                m_catalog.clear();
                message("Unity changed the actual framework execution identity; no run was rebound");
                return;
            }
            if (m_requested) {
                if (!m_requested->acknowledges(*observed)) {
                    rejectReceipt("Unity did not acknowledge the interrupted test operation");
                    return;
                }
                m_owner = observed->owner;
                m_job = observed->job;
                m_requested.reset();
                m_owned = true;
            }
            if (!m_job.empty() && m_owned && (observed->owner != m_owner || observed->job != m_job)) {
                m_poll = false;
                m_owned = false;
                message("The selected editor no longer owns this Unity test job; no old result was rebound");
                return;
            }
            if (explicitRequest || m_owned) {
                m_owner = observed->owner;
                m_job = observed->job;
                m_catalog = observed->catalog;
                m_catalogMode = observed->mode;
                m_execution = observed->execution;
                m_poll = active(observed->state);
                if (explicitRequest && m_poll)
                    m_owned = true;
                if (!m_job.empty()) {
                    snapshot();
                    return;
                }
            }
            message(observed->message);
        });
    }
    void tick() {
        if (!isVisible())
            return;
        const auto source = selected();
        if (m_source && (!source || !sameDomain(*source, *m_source))) {
            ++m_round;
            m_pending = false;
            if (!source) {
                message("Unity test editor disconnected; waiting for the same instance");
                return;
            }
            if (source->root != m_source->root || source->instance != m_source->instance) {
                m_poll = false;
                m_owned = false;
                m_catalog.clear();
                message("A different Unity editor is selected; observe or discover its own tests");
                return;
            }
            if (m_owned || m_requested) {
                observe(false);
                return;
            }
        }
        if (m_poll && !m_pending) {
            if (m_requested)
                observe(false);
            else
                snapshot();
        }
        update();
    }
    void start(bool run, std::vector<std::string> ids) {
        if (m_pending || m_poll) {
            message("Wait for or stop the owned Unity test operation");
            return;
        }
        QPointer<UnityTestsWidget> self(this);
        if (m_context.prepare) {
            const auto prepare = m_context.prepare;
            const auto saved = prepare();
            if (!self)
                return;
            if (!saved) {
                message(saved.error().message());
                return;
            }
        }
        const RequestedOperation operation{run, QUuid::createUuid().toString(QUuid::Id128).toStdString(),
                                           m_mode->currentText().toStdString(), m_owner, m_catalog};
        auto request = lang::Json::object()
                           .set("op", run ? "run" : "discover")
                           .set("mode", operation.mode)
                           .set("job", operation.job)
                           .set("owner", operation.owner)
                           .set("catalog", operation.catalog);
        auto selectedIds = lang::Json::array();
        for (const auto& id : ids)
            selectedIds.push(id);
        request.set("ids", std::move(selectedIds));
        m_requested = operation;
        send(request, [this, operation](const lang::Json& json) {
            auto result = parseUnityTestState(json);
            if (!result || !operation.acknowledges(*result)) {
                rejectReceipt(result ? "Unity acknowledged a different test operation" : result.error().message());
                return;
            }
            m_requested.reset();
            m_owner = result->owner;
            m_job = result->job;
            m_catalog = result->catalog;
            m_catalogMode = result->mode;
            m_execution = result->execution;
            m_owned = true;
            m_poll = active(result->state);
            m_rows.clear();
            m_table->setRowCount(0);
            message(result->message);
            snapshot();
        });
    }
    void rejectReceipt(std::string_view reason) {
        m_requested.reset();
        m_catalog.clear();
        m_owned = false;
        m_poll = false;
        message(reason);
    }
    void cancelRun() {
        send(lang::Json::object().set("op", "cancel").set("owner", m_owner).set("job", m_job),
             [this](const lang::Json& json) {
                 auto result = parseUnityTestState(json);
                 if (!result || result->owner != m_owner || result->job != m_job) {
                     message("Unity cancellation did not preserve owned job identity");
                     return;
                 }
                 m_poll = active(result->state);
                 message(result->message);
                 snapshot();
             });
    }
    void snapshot() {
        if (m_pending || m_job.empty())
            return;
        m_pages.clear();
        m_seen.clear();
        send(lang::Json::object().set("op", "snapshot").set("owner", m_owner).set("job", m_job),
             [this](const lang::Json& json) { page(json, {}, 0); });
    }
    void page(const lang::Json& json, std::string expected, int offset) {
        auto result = parseUnityTestPage(json, expected, offset);
        if (!result || result->owner != m_owner || result->job != m_job) {
            message(result ? "Unity snapshot changed owner/job" : result.error().message());
            m_poll = false;
            m_catalog.clear();
            return;
        }
        if (m_owned && !m_execution.empty() && result->execution != m_execution) {
            m_poll = false;
            m_owned = false;
            m_catalog.clear();
            message("Unity snapshot changed the actual framework execution identity; no results were published");
            return;
        }
        if (offset == 0) {
            m_snapshotState = *result;
            m_snapshotState.cases.clear();
        } else if (result->total != m_snapshotState.total || result->state != m_snapshotState.state ||
                   result->catalog != m_snapshotState.catalog || result->mode != m_snapshotState.mode ||
                   result->execution != m_snapshotState.execution) {
            message("Unity snapshot changed between pages");
            m_poll = false;
            m_catalog.clear();
            return;
        }
        for (auto& row : result->cases) {
            if (!m_seen.insert(row.id).second) {
                message("Unity snapshot repeats a test identity");
                m_poll = false;
                m_catalog.clear();
                return;
            }
            m_pages.push_back(std::move(row));
        }
        if (result->next < result->total) {
            const auto token = result->snapshot;
            const int next = result->next;
            send(lang::Json::object()
                     .set("op", "page")
                     .set("owner", m_owner)
                     .set("job", m_job)
                     .set("snapshot", token)
                     .set("offset", next),
                 [this, token, next](const lang::Json& response) { page(response, token, next); });
            return;
        }
        m_rows = std::move(m_pages);
        m_catalog = result->catalog;
        m_catalogMode = result->mode;
        m_poll = active(result->state);
        m_table->setRowCount(0);
        for (const auto& row : m_rows) {
            const int index = m_table->rowCount();
            m_table->insertRow(index);
            m_table->setItem(index, 0, new QTableWidgetItem(q(row.fullName)));
            m_table->setItem(index, 1, new QTableWidgetItem(q(row.outcome)));
            const bool finished = row.outcome == "Passed" || row.outcome == "Failed" || row.outcome == "Skipped" ||
                                  row.outcome == "Inconclusive";
            m_table->setItem(index, 2,
                             new QTableWidgetItem(finished ? QString::number(row.duration, 'g', 8) : QString()));
        }
        message(result->message);
    }
    UnityTestsWindowContext m_context;
    QComboBox* m_mode = nullptr;
    QPushButton *m_observe = nullptr, *m_discover = nullptr, *m_all = nullptr, *m_selected = nullptr,
                *m_failed = nullptr, *m_stop = nullptr;
    QLabel* m_status = nullptr;
    QTableWidget* m_table = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QTimer m_timer;
    std::optional<UnityEditorIdentity> m_source;
    std::uint64_t m_round = 0;
    bool m_pending = false, m_poll = false, m_owned = false;
    std::optional<RequestedOperation> m_requested;
    std::string m_owner, m_job, m_catalog, m_catalogMode, m_execution;
    std::vector<UnityTestCase> m_rows, m_pages;
    std::set<std::string> m_seen;
    UnityTestObservation m_snapshotState;
};
} // namespace
QWidget* createUnityTestsWidget(QWidget* parent, UnityTestsWindowContext context) {
    return new UnityTestsWidget(parent, std::move(context));
}
} // namespace frost::app
