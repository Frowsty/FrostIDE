// SPDX-License-Identifier: Apache-2.0
// GUI-thread registry of independently owned DAP controllers with explicit session/group identity.
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/debug_controller.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace frost::ui {
using DebugSessionId = std::uint64_t;
using DebugGroupId = std::uint64_t;

struct DebugSessionEntry {
    DebugSessionId id = 0;
    DebugGroupId group = 0;
    QString name;
    DebugController* controller = nullptr; // Borrowed until removed; use id for asynchronous work.
};

// One controller and one verification/capability store per session. The primary controller/catalog
// are borrowed and must outlive this registry; additional controllers are owned. Logical line and
// function definitions synchronize, while adapter verdicts and exception capabilities never do.
// GUI thread only. Selection affects presentation/actions, never session/process ownership.
class DebugSessionRegistry final : public QObject {
    Q_OBJECT
public:
    using Factory = std::function<std::unique_ptr<DebugController>(debug::BreakpointManager&)>;
    DebugSessionRegistry(debug::BreakpointManager& catalog, DebugController& primary, Factory factory,
                         QObject* parent = nullptr);
    ~DebugSessionRegistry() override;
    [[nodiscard]] DebugSessionId primaryId() const noexcept { return 1; }
    [[nodiscard]] DebugSessionId selectedId() const noexcept { return m_selected; }
    [[nodiscard]] DebugController* selected() const noexcept;
    [[nodiscard]] DebugController* controller(DebugSessionId id) const noexcept;
    [[nodiscard]] std::vector<DebugSessionEntry> sessions() const;
    [[nodiscard]] bool anyDebugging() const;
    [[nodiscard]] bool anyRerunPending() const;
    // Fails NotFound for an absent id; only this controller presents shared editor decorations.
    [[nodiscard]] Result<void> select(DebugSessionId id);
    // Stable monotonically increasing ids, never reused. At most 128 retained sessions.
    [[nodiscard]] Result<DebugSessionId> createSession(QString name, DebugGroupId group = 0);
    // Starts native launch through the controller's real trust/adapter gate. An immediate error
    // removes the new session and stops only sessions in this nonzero group.
    [[nodiscard]] Result<DebugSessionId> startTarget(const debug::DebugTarget& target, DebugGroupId group = 0);
    [[nodiscard]] Result<DebugSessionId> attachTarget(const debug::DebugTarget& target, debug::AdapterFactory factory,
                                                      rpc::Json arguments, std::function<bool()> admission = {},
                                                      DebugGroupId group = 0);
    [[nodiscard]] DebugGroupId createGroup();
    void finishPreparing(DebugGroupId group);
    [[nodiscard]] bool hasPendingWork() const noexcept { return !m_preparing.empty(); }
    void stopGroup(DebugGroupId group);
    // Automatic launch/error cleanup preserves historical engine evidence. User Stop paths emit
    // explicitStopRequested before cleanup so an exact engine owner can revoke its observation.
    void rollbackGroup(DebugGroupId group);
    void stopSelected();
    // Ordinary stop semantics: launched debuggees terminate; attached hosts detach and survive.
    void stopAll();
    void shutdown();
    // Refuses removal of the primary or a live session. Ended sessions retain results until closed.
    [[nodiscard]] Result<void> remove(DebugSessionId id);
    // Returns an empty entry if absent or ambiguous. Callers must additionally bind authenticated project,
    // engine identity, this registry id and controller.sessionGeneration before an engine action.
    [[nodiscard]] DebugSessionEntry nativeForPid(std::int64_t pid) const;
    void attachEditor(EditorView* view);
    void detachEditor(EditorView* view);
signals:
    void sessionsChanged();
    void sessionRemoving(frost::ui::DebugSessionId id);
    void selectedChanged(frost::ui::DebugSessionId id);
    void sessionStateChanged(frost::ui::DebugSessionId id);
    void groupStopRequested(frost::ui::DebugGroupId group);
    void groupFailed(frost::ui::DebugGroupId group, const QString& error);
    void explicitStopRequested(frost::ui::DebugSessionId session);

private:
    struct Owned;
    void observe(DebugSessionId id);
    void synchronize(DebugSessionId source);
    void stopGroupImpl(DebugGroupId group, bool explicitRequest);
    debug::BreakpointManager& m_catalog;
    Factory m_factory;
    std::map<DebugSessionId, std::unique_ptr<Owned>> m_entries;
    std::vector<QPointer<EditorView>> m_editors;
    std::map<DebugGroupId, bool> m_groups;
    std::set<DebugGroupId> m_preparing;
    std::set<DebugGroupId> m_stoppingGroups;
    DebugSessionId m_selected = 1, m_nextId = 2;
    DebugGroupId m_nextGroup = 1;
    bool m_syncing = false, m_shutdown = false;
};
} // namespace frost::ui
