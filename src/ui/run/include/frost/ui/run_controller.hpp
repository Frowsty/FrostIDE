// SPDX-License-Identifier: Apache-2.0
//
// RunController: binds a RunService (Qt-free, frost::build) to the Run console, the toolbar's
// startup project combo (MainToolbar::runConfigurations) and the run actions (run_actions.hpp)
// on the GUI thread (W1, ADR 0040).
//
/*
 * Callbacks. RunService takes its callbacks when it is created, so the order is: construct the
 * controller, create the service with callbacks(), then setService(). The callbacks post each
 * event to the controller's thread (queued); events that arrive after the controller is gone
 * are dropped, so the service may outlive it (and vice versa: setService(nullptr) before the
 * service is destroyed).
 *
 * The console shows the latest run that started; output of an older run that is still going
 * (another project) is not shown. TODO(M4): one console tab per run, as Rider has.
 *
 * Busy. run() on a project that is already running asks hooks.confirmRerun (Rider's "stop and
 * rerun?"); yes reruns it, no leaves it running. Errors go to hooks.reportError. GUI thread only.
 */
#pragma once

#include "frost/build/run_service.hpp"
#include "frost/core/error.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QComboBox;

namespace frost::ui {

class RunConsole;

struct RunControllerHooks {
    // "<project> is running. Stop and rerun it?"; empty: yes.
    std::function<bool(const QString& projectName)> confirmRerun{};
    // Shows a run that could not start or a setting that could not be saved; empty: ignored.
    std::function<void(const QString& message)> reportError{};
};

class RunController final : public QObject {
    Q_OBJECT

public:
    // `console` (optional) must outlive the controller or be destroyed with it.
    explicit RunController(RunConsole* console, RunControllerHooks hooks = {}, QObject* parent = nullptr);
    ~RunController() override;
    RunController(const RunController&) = delete ("a QObject");
    RunController& operator=(const RunController&) = delete ("a QObject");

    // The callbacks to create the RunService with (header comment). Thread-safe.
    [[nodiscard]] build::RunServiceCallbacks callbacks();
    // The service to drive (null detaches); refreshes the startup combo.
    void setService(build::RunService* service);
    [[nodiscard]] build::RunService* service() const noexcept { return m_service; }

    /*
     * Fills `combo` with the startup candidates and keeps it in step: the user's choice becomes the
     * startup project (persisted by the service), and changes from elsewhere (Set as Startup
     * Project) select the entry. The combo is disabled without a solution. `combo` may be null.
     */
    void bindStartupCombo(QComboBox* combo);
    // Re-reads the candidates (after a solution was opened or closed).
    void refreshStartupCombo();

    // Runs `project` (default: the startup project), with the Busy prompt (header comment).
    // Errors: those of RunService::run, Cancelled when the user declined the rerun.
    [[nodiscard]] Result<build::RunId> run(std::optional<project::ProjectId> project = std::nullopt);
    // Reruns the console's project (else the startup project).
    [[nodiscard]] Result<build::RunId> rerun();
    // Stops the console's run, if it is active.
    void stop();
    // Makes `project` the startup project. Errors: those of RunService::setStartupProject.
    [[nodiscard]] Result<void> setStartupProject(project::ProjectId project);

    // The run the console shows (0: none yet).
    [[nodiscard]] build::RunId currentRun() const noexcept { return m_current; }
    // Whether the console's run has not finished.
    [[nodiscard]] bool running() const noexcept { return m_currentActive; }

signals:
    // A run started, changed state or finished (for the toolbar's Run and Stop buttons).
    void runStateChanged();

private:
    struct Relay;

    void onStarted(const build::RunInfo& info);
    void onStateChanged(const build::RunInfo& info);
    void onOutput(build::RunId run, build::RunOutputKind kind, const std::string& text);
    void onFinished(const build::RunOutcome& outcome);
    void onStartupChanged(std::optional<project::ProjectId> project);
    void report(const Error& error);

    QPointer<RunConsole> m_console;
    RunControllerHooks m_hooks;
    build::RunService* m_service = nullptr;
    QPointer<QComboBox> m_combo;
    bool m_fillingCombo = false;
    std::shared_ptr<Relay> m_relay;
    build::RunId m_current = 0;
    std::optional<project::ProjectId> m_currentProject;
    bool m_currentActive = false;
};

} // namespace frost::ui
