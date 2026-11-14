// SPDX-License-Identifier: Apache-2.0
// Session registry: independent DAP lifetimes, shared logical breakpoints and selected presentation.
#include "frost/ui/debug_session_registry.hpp"

#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/run_console.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace frost::ui {
struct DebugSessionRegistry::Owned {
    std::unique_ptr<debug::BreakpointManager> breakpoints;
    std::unique_ptr<DebugController> owned;
    DebugController* controller = nullptr;
    QString name;
    DebugGroupId group = 0;
    std::uint64_t listener = 0;
    QPointer<RunConsole> earlyConsole;
    ~Owned() {
        if (earlyConsole && !earlyConsole->parentWidget())
            delete earlyConsole.data();
    }
};

DebugSessionRegistry::DebugSessionRegistry(debug::BreakpointManager& catalog, DebugController& primary, Factory factory,
                                           QObject* parent)
    : QObject(parent), m_catalog(catalog), m_factory(std::move(factory)) {
    auto entry = std::make_unique<Owned>();
    entry->controller = &primary;
    entry->name = tr("Debug");
    if (!primary.console()) {
        entry->earlyConsole = new RunConsole();
        primary.setConsole(entry->earlyConsole);
    }
    m_entries.emplace(primaryId(), std::move(entry));
    observe(primaryId());
}
DebugSessionRegistry::~DebugSessionRegistry() {
    shutdown();
    for (auto& [id, entry] : m_entries) {
        entry->controller->setEditorPresentationEnabled(false);
        entry->controller->breakpoints().unsubscribe(entry->listener);
        for (const auto& view : m_editors)
            if (view)
                entry->controller->detachEditor(view);
    }
}
DebugController* DebugSessionRegistry::controller(DebugSessionId id) const noexcept {
    const auto found = m_entries.find(id);
    return found == m_entries.end() ? nullptr : found->second->controller;
}
DebugController* DebugSessionRegistry::selected() const noexcept { return controller(m_selected); }
std::vector<DebugSessionEntry> DebugSessionRegistry::sessions() const {
    std::vector<DebugSessionEntry> result;
    for (const auto& [id, entry] : m_entries)
        result.push_back({id, entry->group,
                          entry->controller->title().isEmpty() ? entry->name : entry->controller->title(),
                          entry->controller});
    return result;
}
bool DebugSessionRegistry::anyDebugging() const {
    return std::ranges::any_of(m_entries, [](const auto& item) { return item.second->controller->debugging(); });
}
bool DebugSessionRegistry::anyRerunPending() const {
    return std::ranges::any_of(m_entries, [](const auto& item) { return item.second->controller->rerunPending(); });
}
Result<void> DebugSessionRegistry::select(DebugSessionId id) {
    auto* next = controller(id);
    if (!next)
        return fail(ErrorCode::NotFound, "The debugger session is no longer available");
    if (m_selected == id)
        return {};
    if (auto* previous = selected())
        previous->setEditorPresentationEnabled(false);
    m_selected = id;
    next->setEditorPresentationEnabled(true);
    emit selectedChanged(id);
    return {};
}
Result<DebugSessionId> DebugSessionRegistry::createSession(QString name, DebugGroupId group) {
    if (m_shutdown)
        return fail(ErrorCode::Cancelled, "The debugger registry is shutting down");
    if (!m_factory || m_entries.size() >= 128 || m_nextId == std::numeric_limits<DebugSessionId>::max())
        return fail(ErrorCode::Busy, "Close an ended debugger session before creating another");
    if (group && (!m_groups.contains(group) || m_groups.at(group)))
        return fail(ErrorCode::InvalidArgument, "The compound debugger group is absent or already failed");
    auto entry = std::make_unique<Owned>();
    entry->breakpoints = std::make_unique<debug::BreakpointManager>();
    entry->breakpoints->replaceLogicalFrom(m_catalog);
    entry->owned = m_factory(*entry->breakpoints);
    if (!entry->owned)
        return fail(ErrorCode::Unsupported, "The debugger controller factory is unavailable");
    entry->controller = entry->owned.get();
    entry->controller->setEditorPresentationEnabled(false);
    entry->name = std::move(name);
    entry->group = group;
    entry->earlyConsole = new RunConsole();
    entry->controller->setConsole(entry->earlyConsole);
    const auto id = m_nextId++;
    m_entries.emplace(id, std::move(entry));
    observe(id);
    for (const auto& view : m_editors)
        if (view)
            controller(id)->attachEditor(view);
    emit sessionsChanged(); // Consumers create this session's console before launch output arrives.
    return id;
}
Result<DebugSessionId> DebugSessionRegistry::startTarget(const debug::DebugTarget& target, DebugGroupId group) {
    auto id = createSession(QString::fromStdString(target.name), group);
    if (!id) {
        if (group)
            rollbackGroup(group);
        return id;
    }
    auto* targetController = controller(*id);
    if (!targetController) {
        if (group)
            rollbackGroup(group);
        return fail(ErrorCode::Cancelled, "The debugger session was closed before launch");
    }
    auto started = targetController->startTarget(target);
    if (!started) {
        if (group)
            rollbackGroup(group);
        static_cast<void>(remove(*id));
        return std::unexpected(started.error());
    }
    static_cast<void>(select(*id));
    return id;
}
Result<DebugSessionId> DebugSessionRegistry::attachTarget(const debug::DebugTarget& target,
                                                          debug::AdapterFactory factory, rpc::Json arguments,
                                                          std::function<bool()> admission, DebugGroupId group) {
    auto id = createSession(QString::fromStdString(target.name), group);
    if (!id) {
        if (group)
            rollbackGroup(group);
        return id;
    }
    auto* targetController = controller(*id);
    if (!targetController) {
        if (group)
            rollbackGroup(group);
        return fail(ErrorCode::Cancelled, "The debugger session was closed before attach");
    }
    auto started =
        targetController->attachTarget(target, std::move(factory), std::move(arguments), std::move(admission));
    if (!started) {
        if (group)
            rollbackGroup(group);
        static_cast<void>(remove(*id));
        return std::unexpected(started.error());
    }
    static_cast<void>(select(*id));
    return id;
}
DebugGroupId DebugSessionRegistry::createGroup() {
    if (m_shutdown || m_preparing.size() >= 128 || m_nextGroup == std::numeric_limits<DebugGroupId>::max())
        return 0;
    const auto id = m_nextGroup++;
    m_groups.emplace(id, false);
    m_preparing.insert(id);
    emit sessionsChanged();
    return id;
}
void DebugSessionRegistry::finishPreparing(DebugGroupId group) {
    m_preparing.erase(group);
    if (std::ranges::none_of(m_entries, [group](const auto& item) { return item.second->group == group; }))
        m_groups.erase(group);
    emit sessionsChanged();
}
void DebugSessionRegistry::stopGroup(DebugGroupId group) { stopGroupImpl(group, true); }
void DebugSessionRegistry::rollbackGroup(DebugGroupId group) { stopGroupImpl(group, false); }
void DebugSessionRegistry::stopGroupImpl(DebugGroupId group, bool explicitRequest) {
    if (!group || !m_stoppingGroups.insert(group).second)
        return;
    const bool active = m_groups.contains(group) && !m_groups.at(group);
    if (active)
        m_groups[group] = true;
    m_preparing.erase(group);
    // Snapshot ids: stop can synchronously trigger observers which change selection.
    const auto snapshot = sessions();
    if (explicitRequest)
        for (const auto& entry : snapshot)
            if (entry.group == group)
                if (auto* current = controller(entry.id); current && current->debugging())
                    emit explicitStopRequested(entry.id);
    if (active && explicitRequest)
        emit groupStopRequested(group);
    for (const auto& entry : snapshot)
        if (entry.group == group)
            if (auto* current = controller(entry.id); current && current->debugging())
                current->stop();
    if (std::ranges::none_of(m_entries, [group](const auto& item) { return item.second->group == group; }))
        m_groups.erase(group);
    emit sessionsChanged();
    m_stoppingGroups.erase(group);
}
void DebugSessionRegistry::stopSelected() {
    const auto id = selectedId();
    if (auto* current = selected(); current && current->debugging()) {
        emit explicitStopRequested(id);
        if (auto* stillCurrent = controller(id))
            stillCurrent->stop();
    }
}
void DebugSessionRegistry::stopAll() {
    std::vector<DebugGroupId> groups;
    for (const auto& [group, failed] : m_groups)
        groups.push_back(group);
    for (const auto group : groups)
        stopGroup(group);
    const auto snapshot = sessions();
    for (const auto& entry : snapshot)
        if (auto* current = controller(entry.id); entry.group == 0 && current && current->debugging()) {
            emit explicitStopRequested(entry.id);
            if (auto* stillCurrent = controller(entry.id))
                stillCurrent->stop();
        }
}
void DebugSessionRegistry::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    for (auto& [id, entry] : m_entries)
        entry->controller->shutdown();
}
Result<void> DebugSessionRegistry::remove(DebugSessionId id) {
    auto found = m_entries.find(id);
    if (found == m_entries.end())
        return fail(ErrorCode::NotFound, "The debugger session is no longer available");
    if (id == primaryId() || found->second->controller->debugging())
        return fail(ErrorCode::Busy, "Stop the debugger before closing its session");
    if (m_selected == id)
        static_cast<void>(select(primaryId()));
    emit sessionRemoving(id); // Views release controller references before the controller dies.
    auto& entry = *found->second;
    entry.controller->breakpoints().unsubscribe(entry.listener);
    for (const auto& view : m_editors)
        if (view)
            entry.controller->detachEditor(view);
    const auto group = entry.group;
    m_entries.erase(found);
    if (group && !m_preparing.contains(group) &&
        std::ranges::none_of(m_entries, [group](const auto& item) { return item.second->group == group; }))
        m_groups.erase(group);
    emit sessionsChanged();
    return {};
}
DebugSessionEntry DebugSessionRegistry::nativeForPid(std::int64_t pid) const {
    DebugSessionEntry result;
    if (pid <= 0)
        return result;
    for (const auto& entry : sessions()) {
        if (entry.controller->nativeSession() && entry.controller->session()->state() != debug::DebugState::Ended &&
            entry.controller->session()->processId() == pid) {
            if (result.id)
                return {}; // PID alone must never pick an arbitrary session.
            result = entry;
        }
    }
    return result;
}
void DebugSessionRegistry::attachEditor(EditorView* view) {
    if (!view)
        return;
    if (std::ranges::none_of(m_editors, [view](const auto& item) { return item == view; }))
        m_editors.emplace_back(view);
    for (auto& [id, entry] : m_entries)
        entry->controller->attachEditor(view);
}
void DebugSessionRegistry::detachEditor(EditorView* view) {
    for (auto& [id, entry] : m_entries)
        entry->controller->detachEditor(view);
    std::erase_if(m_editors, [view](const auto& item) { return !item || item == view; });
}
void DebugSessionRegistry::synchronize(DebugSessionId source) {
    if (m_syncing || m_shutdown)
        return;
    auto* origin = controller(source);
    if (!origin)
        return;
    m_syncing = true;
    m_catalog.replaceLogicalFrom(origin->breakpoints());
    for (auto& [id, entry] : m_entries)
        if (id != source)
            entry->controller->breakpoints().replaceLogicalFrom(m_catalog);
    m_syncing = false;
}
void DebugSessionRegistry::observe(DebugSessionId id) {
    auto* current = controller(id);
    connect(current, &DebugController::userStopRequested, this, [this, id] {
        if (auto* child = controller(id); child && child->debugging())
            emit explicitStopRequested(id);
    });
    m_entries.at(id)->listener = current->breakpoints().subscribe([this, id](std::string_view,
                                                                             debug::BreakpointManager::Change change) {
        if (change == debug::BreakpointManager::Change::Lines || change == debug::BreakpointManager::Change::Functions)
            synchronize(id);
    });
    connect(current, &DebugController::stateChanged, this, [this, id] {
        auto found = m_entries.find(id);
        if (found == m_entries.end())
            return;
        auto& entry = *found->second;
        if (entry.group && !entry.controller->debugging() && entry.controller->lastOutcome() &&
            entry.controller->lastOutcome()->error && !m_groups.at(entry.group)) {
            const auto group = entry.group;
            const auto error = QString::fromStdString(entry.controller->lastOutcome()->error->message());
            rollbackGroup(group);
            emit groupFailed(group, error);
        }
        emit sessionStateChanged(id);
    });
}
} // namespace frost::ui
