// SPDX-License-Identifier: Apache-2.0
//
// RunController: the Run toolbar's controller (W1, ADR 0040; RUN-3, PLAN.md Appendix C R2). It
// binds the run configurations of the open workspace (RUN-1's model, RUN-2's generated types) to
// the toolbar's configuration combo (MainToolbar::runConfigurations), runs the selected one, and
// feeds the Run tool window (run_tool_window.hpp), one tab per run. On the GUI thread.
//
/*
 * What runs. run() runs the selected configuration, by its type:
 *   - native: through the RunManager (run::RunManager, RUN-1; setRunManager), after
 *     hooks.mayRunConfiguration (the workspace's trust gate). Output, input, Stop (the whole
 *     process tree) and the exit code go to the configuration's tab.
 *   - cmake-target: hooks.selectCMakeTarget(target), then hooks.alternateRun (the CMake
 *     integration builds the target and starts it through the same RunManager, laying the
 *     configuration's edits over its launch: hooks of the workbench).
 *   - msbuild: the RunService (build::RunService) runs the project the configuration names: it
 *     builds it, asks MSBuild for TargetPath and the .vcxproj.user debugger settings, and runs it
 *     in a tab (or an external console window).
 * With no configurations (a solution opened before RUN-2's generator ran, the unit tests' service
 * alone) the combo lists the RunService's startup candidates as before and Run runs the startup
 * project, or hooks.alternateRun when hooks.alternateAvailable says an alternate runner is there.
 *
 * Callbacks. RunService takes its callbacks when it is created, so the order is: construct the
 * controller, create the service with callbacks(), then setService(). The callbacks post each
 * event to the controller's thread (queued); events that arrive after the controller is gone
 * are dropped, so the service and the RunManager may outlive it (and vice versa:
 * setService(nullptr) before the service is destroyed).
 *
 * Combo. Configurations by name (cmake, then msbuild, then the user's), then "Edit
 * Configurations..." (hooks.editConfigurations). Choosing an msbuild configuration makes its
 * project the startup project (persisted by the RunService); choosing a cmake one selects its
 * target in the CMake integration (hooks.selectCMakeTarget). A startup project or CMake target
 * chosen elsewhere selects its configuration (selectConfiguration, onStartupChanged).
 *
 * Busy. Running a configuration (or project) that is already running asks hooks.confirmRerun
 * (Rider's "stop and rerun?"); yes reruns it once it has stopped, no leaves it running. Errors go
 * to hooks.reportError.
 */
#pragma once

#include "frost/build/run_service.hpp"
#include "frost/core/error.hpp"
#include "frost/run/run_configuration_set.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/run_tool_window.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;

namespace frost::ui {

class RunConsole;

struct RunControllerHooks {
    // "<project> is running. Stop and rerun it?"; empty: yes.
    std::function<bool(const QString& projectName)> confirmRerun{};
    // Shows a run that could not start or a setting that could not be saved; empty: ignored.
    std::function<void(const QString& message)> reportError{};
    // Opens the Run/Debug Configurations dialog when "Edit Configurations..." is selected.
    std::function<void()> editConfigurations{};
    // Whether a run may start now (no debug session, no project edit); empty: yes.
    std::function<bool()> mayRun{};
    // An alternate project kind (CMake) supplies its own run lifecycle for its targets.
    std::function<bool()> alternateAvailable{};
    std::function<bool()> alternateRunning{};
    std::function<Result<build::RunId>()> alternateRun{};
    std::function<Result<build::RunId>()> alternateRerun{};
    std::function<void()> alternateStop{};
    // Makes `target` the CMake integration's Run/Debug target (a cmake-target configuration chosen).
    std::function<void(const std::string& target)> selectCMakeTarget{};
    // The trust gate of a native configuration; an error is reported and nothing starts. Empty: allowed.
    std::function<Result<void>(const run::RunConfiguration& configuration)> mayRunConfiguration{};
    // The generated configurations of the open workspace, asked on every refresh(); empty: none.
    std::function<std::vector<run::RunConfiguration>()> generatedConfigurations{};
    // Persists the configurations the store keeps (RunConfigurationSet::stored) after an edit.
    std::function<Result<void>(const std::vector<run::RunConfiguration>& stored)> saveConfigurations{};
};

class RunController final : public QObject {
    Q_OBJECT

public:
    // `window` (optional) must outlive the controller or be destroyed with it.
    explicit RunController(RunToolWindow* window, RunControllerHooks hooks = {}, QObject* parent = nullptr);
    ~RunController() override;
    RunController(const RunController&) = delete ("a QObject");
    RunController& operator=(const RunController&) = delete ("a QObject");

    // The callbacks to create the RunService with (header comment). Thread-safe.
    [[nodiscard]] build::RunServiceCallbacks callbacks();
    // The service to drive (null detaches); refreshes the combo.
    void setService(build::RunService* service);
    [[nodiscard]] build::RunService* service() const noexcept { return m_service; }
    // The RunManager that runs native configurations (null: they cannot run). Must outlive its use.
    void setRunManager(run::RunManager* runs) noexcept { m_runs = runs; }
    [[nodiscard]] RunToolWindow* window() const noexcept { return m_window; }

    /*
     * Fills `combo` with the configurations (header comment, "Combo") and keeps it in step. The
     * combo is disabled while it lists nothing. `combo` may be null.
     */
    void bindStartupCombo(QComboBox* combo);
    // Asks hooks.generatedConfigurations again and refills the combo (after a solution was opened
    // or closed, or the CMake model changed).
    void refreshStartupCombo();
    // Installs or replaces the "Edit Configurations..." handler, then refreshes the combo.
    void setEditConfigurationsHandler(std::function<void()> handler);

    // The workspace's stored configurations (run::loadRunConfigurations); refreshes.
    void setStoredConfigurations(std::vector<run::RunConfiguration> stored);
    // The merged configurations (generated, then the user's); the toolbar's list.
    [[nodiscard]] const run::RunConfigurationSet& configurations() const noexcept { return m_set; }
    // The dialog's OK: replaces the configurations, persists the store (hooks.saveConfigurations)
    // and refreshes. Errors: those of the save (the new list applies anyway).
    [[nodiscard]] Result<void> replaceConfigurations(const std::vector<run::RunConfiguration>& edited);
    // The selected configuration, or null (none, or no configurations).
    [[nodiscard]] const run::RunConfiguration* selectedConfiguration() const noexcept;
    /*
     * Selects the configuration `id` in the combo. With `apply` the selection is acted on as if the
     * user chose it (startup project, CMake target); without, only the combo follows (a choice made
     * elsewhere). Errors: NotFound (no such configuration).
     */
    [[nodiscard]] Result<void> selectConfiguration(const std::string& id, bool apply = true);
    // Selects the cmake-target configuration of `target`, if there is one, without applying it.
    void followCMakeTarget(const std::string& target);

    // Runs the selected configuration (header comment); `project` runs that project of the
    // solution instead (a gutter marker). Errors: those of the runner, Cancelled when the user
    // declined the rerun, Busy while debugging.
    [[nodiscard]] Result<build::RunId> run(std::optional<project::ProjectId> project = std::nullopt);
    // Runs configuration `id` (header comment). Errors: NotFound, those of run().
    [[nodiscard]] Result<build::RunId> runConfiguration(const std::string& id);
    // Reruns the current tab's run (else run()).
    [[nodiscard]] Result<build::RunId> rerun();
    // Stops the current tab's run; with none active there, every run.
    void stop();
    // Makes `project` the startup project. Errors: those of RunService::setStartupProject.
    [[nodiscard]] Result<void> setStartupProject(project::ProjectId project);

    // The RunService run the newest service tab shows (0: none yet).
    [[nodiscard]] build::RunId currentRun() const noexcept { return m_current; }
    // Whether any run (service, alternate, native configuration) has not finished.
    [[nodiscard]] bool running() const;
    // Whether the native configuration `id` has a run that has not finished.
    [[nodiscard]] bool configurationRunning(const std::string& id) const;
    // The RunManager run of the native configuration `id` that has not finished, if any.
    [[nodiscard]] std::optional<run::RunId> configurationRun(const std::string& id) const;
    [[nodiscard]] bool canRun() const;
    void alternateStateChanged() { emit runStateChanged(); }

signals:
    // A run started, changed state or finished (for the toolbar's Run and Stop buttons).
    void runStateChanged();
    // The configurations changed (generated again, edited).
    void configurationsChanged();

private:
    struct Relay;
    struct NativeRun {
        run::RunId id = 0;
        QPointer<RunConsole> console;
        bool rerunPending = false;
    };

    void onStarted(const build::RunInfo& info);
    void onStateChanged(const build::RunInfo& info);
    void onOutput(build::RunId run, build::RunOutputKind kind, const std::string& text);
    void onFinished(const build::RunOutcome& outcome);
    void onStartupChanged(std::optional<project::ProjectId> project);
    void report(const Error& error);
    void fillCombo();
    void applySelection(const run::RunConfiguration& configuration);
    [[nodiscard]] Result<build::RunId> runService(std::optional<project::ProjectId> project);
    [[nodiscard]] Result<build::RunId> runNative(const run::RunConfiguration& configuration);
    void nativeFinished(const std::string& id, const run::RunResult& result);
    [[nodiscard]] std::optional<project::ProjectId> projectOf(const run::RunConfiguration& configuration) const;
    [[nodiscard]] RunConsole* openServiceTab(const build::RunInfo& info);

    QPointer<RunToolWindow> m_window;
    RunControllerHooks m_hooks;
    build::RunService* m_service = nullptr;
    run::RunManager* m_runs = nullptr;
    QPointer<QComboBox> m_combo;
    bool m_fillingCombo = false;
    std::shared_ptr<Relay> m_relay;
    run::RunConfigurationSet m_set;
    std::string m_selectedId;
    // The RunService's runs this controller shows, by id (each has its tab's console).
    std::map<build::RunId, QPointer<RunConsole>> m_serviceConsoles;
    build::RunId m_current = 0;
    std::optional<project::ProjectId> m_currentProject;
    bool m_currentActive = false;
    // Native configuration runs by configuration id.
    std::map<std::string, NativeRun, std::less<>> m_native;
};

} // namespace frost::ui
