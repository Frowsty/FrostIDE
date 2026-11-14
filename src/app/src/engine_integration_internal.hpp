// SPDX-License-Identifier: Apache-2.0
// Shared private state for the app's session, discovery, view and owned-run adapters.
#pragma once

#include "frost/app/workbench.hpp"
#include "frost/engine/command_policy.hpp"
#include "frost/engine/project_content.hpp"
#include "frost/engine/session.hpp"
#include "frost/ui/shell/engine_state.hpp"

#include "engine_crash_observer.hpp"
#include "engine_integration.hpp"
#include "engine_locator.hpp"
#include "engine_native_debug.hpp"
#include <QPointer>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <variant>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
namespace frost::ui {
class EngineConsole;
class EngineView;
} // namespace frost::ui

namespace frost::app {
struct EngineNativeHostState;

inline constexpr std::string_view kEngineConsoleWindow = "frostEngine";
inline constexpr std::string_view kEngineProjectWindow = "engineProject";

struct EnginePolicyState {
    std::mutex mutex;
    std::unique_ptr<engine::CommandPolicy> policy;
    engine::CommandPolicyContext context;
    std::atomic<bool> active{true};
    std::atomic<bool> nativePaused{false};
};

struct EngineRunAdmission {
    std::atomic<bool> active{true};
    std::string id, program, project, root, configDirectory;
    std::uint64_t trustRevision = 0;
    project::TrustStore* trust = nullptr;
    io::IFileSystem* fs = nullptr;
    std::string projectKey;
    std::shared_ptr<std::atomic<bool>> physicalAdmission;
    std::mutex resolutionMutex;
    std::optional<std::string> resolvedProject;
};

struct EngineCommandReply {
    engine::EngineIdentity identity;
    std::string method, message;
    bool success = false;
    lang::Json data;
};
using EnginePush =
    std::variant<engine::EngineState, engine::EngineLogEntry, engine::EngineDiagnostic, EngineCommandReply>;
struct QueuedEnginePush {
    EnginePush value;
    std::size_t bytes = 0;
    std::uint64_t order = 0;
};
struct EngineEventQueue {
    std::mutex mutex;
    std::deque<QueuedEnginePush> pending;
    std::optional<QueuedEnginePush> coalescedState;
    std::uint64_t nextOrder = 0;
    std::size_t bytes = 0;
    std::uint64_t dropped = 0, reportedDropped = 0;
    bool scheduled = false;
    std::atomic<bool> active{true};
};

struct EngineIntegration::Impl {
    EngineIntegration& owner;
    Workbench& workbench;
    QPointer<QWidget> panel;
    QPointer<QComboBox> picker;
    QPointer<QPushButton> refreshButton, pairButton, startButton, disconnectButton, cancelButton, debugLuaButton;
    QPointer<ui::EngineConnectionIndicator> indicator;
    QPointer<ui::EngineConsole> console;
    QPointer<ui::EngineView> projectView;
    QPointer<QLabel> status;
    std::vector<QPointer<QPushButton>> commandButtons;
    QTimer* startupWatch = nullptr;
    bool registered = false, projectRegistered = false, stopped = false, available = false, discoveryBusy = false;
    bool projectBusy = false;
    std::uint64_t epoch = 0, sessionEpoch = 0, discoveryGeneration = 0, contentGeneration = 0;
    std::string root, physicalRoot, projectKey, selectedProject, automationDirectory, chosenProgram;
    std::uint64_t trustRevision = 0;
    std::shared_ptr<std::atomic<bool>> physicalAdmission = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<std::uint64_t>> testsSourceRevision = std::make_shared<std::atomic<std::uint64_t>>(1);
    std::stop_source workStop;
    std::vector<sched::JoinHandle<void>> tasks;
    std::vector<EngineInstallation> installations;
    std::vector<EngineInstanceRecord> instances;
    std::unique_ptr<engine::EngineSession> session;
    std::unique_ptr<EngineNativeDebugAdapter> nativeDebug;
    std::optional<engine::NativePauseLease> nativePauseLease;
    std::optional<EngineNativeDebugBinding> nativePauseBinding;
    std::optional<EngineNativeDebugTarget> nativeTarget;
    std::optional<EngineNativeDebugBinding> retainedNativeBinding, nativeBoundBinding;
    std::shared_ptr<EnginePolicyState> policy = std::make_shared<EnginePolicyState>();
    std::shared_ptr<EngineCrashSessionHolder> crashSessions = std::make_shared<EngineCrashSessionHolder>();
    std::optional<engine::EngineIdentity> identity;
    std::optional<engine::EngineState> state;
    std::shared_ptr<EngineEventQueue> eventQueue = std::make_shared<EngineEventQueue>();
    std::function<void(EnginePush)> pushSink;
    std::optional<engine::ProjectContent> projectContent;
    std::string selectedInstance;
    std::string selectedServerGeneration;
    std::uint64_t lastRequest = 0, lastCompletedRequest = 0;
    std::map<std::uint64_t, std::string> requests;
    lang::Json lastReply;
    std::string lastError;
    std::deque<std::pair<lang::Json, std::size_t>> eventHistory;
    std::size_t eventHistoryBytes = 0, diagnosticBytes = 0;
    std::uint64_t eventHistoryDropped = 0;
    std::deque<std::size_t> diagnosticSizes;
    std::vector<build::BuildDiagnostic> diagnostics;
    std::uint64_t lastEventSequence = 0;
    std::shared_ptr<EngineRunAdmission> runAdmission;
    std::optional<run::RunId> ownedRun;
    std::shared_ptr<EngineNativeHostState> nativeHost;
    std::uint64_t nativeHostReservation = 0;
    io::ProcessId ownedPid = 0;
    std::string ownedConfigDirectory, ownedOutput, ownedSecret;
    IClock::SteadyTime startupDeadline;
    bool awaitingOwnedEndpoint = false, awaitingOwnedAuthentication = false, ownedProcessStarted = false;

    Impl(EngineIntegration& integration, Workbench& host) : owner(integration), workbench(host) {}
    void projectChanged();
    void updateControls();
    void setStatus(const QString& message);
    void background(std::function<void()> work);
    void retireSession();
    void clearSession(bool clearOutput, bool preserveCrashObservation = false);
    void revokeCrashObservation();
    void observedCrashLoss(const engine::EngineIdentity& identity);
    void invalidate(bool stopOwned);
    [[nodiscard]] bool admitted(std::uint64_t expectedEpoch, std::string_view expectedRoot,
                                std::uint64_t expectedTrust) const;
    [[nodiscard]] bool currentIdentity(const engine::EngineIdentity& observed) const;
    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<void> connectInstance(const EngineInstanceRecord& instance);
    [[nodiscard]] Result<std::uint64_t> send(std::string method, lang::Json params);
    void refreshProjectContent();
    void openAsset(std::string relative);
    void openLocation(std::string file, int line, int column);
    void drainPushEvents();
    void observeEvent(std::string_view topic, const lang::Json& data,
                      std::optional<std::uint64_t> sequence = std::nullopt);
    [[nodiscard]] Result<run::RunConfiguration> prepare(const run::RunConfiguration& configuration);
    void endpointFromOutput();
    void checkOwnedStartup();
    void stopOwnedHost();
    [[nodiscard]] std::vector<run::RunConfiguration> generated() const;
};

} // namespace frost::app
