// SPDX-License-Identifier: Apache-2.0
//
// CMakeController: the UI-thread side of an opened CMake folder (ADR 0046). It turns the
// CMakeService's callbacks (service thread) into Qt signals on its own thread, keeps what the
// CMake tool window and the Solution Explorer show (the profile labels, the active profile, the
// error banner text, the busy flag, the CMake tree model) and forwards Reload and the profile
// choice to an ICMakeBackend. The W1 workbench creates one per opened CMake folder.
/*
 * Wiring (the order matters because the service takes its callbacks when it is created):
 *
 *     CMakeController controller;
 *     auto service = build::CMakeService::create(fs, launcher, trust, options, controller.callbacks());
 *     CMakeServiceBackend backend(*service);
 *     controller.setBackend(&backend);
 *     ... service->openFolder(folder) ...
 *     controller.setBackend(nullptr);   // before the backend or the service go away
 *
 * Banner (problem()): why the folder is not configured as it should be: the failed configure's
 * first error (or why cmake did not start), an unusable active profile, a presets file that does
 * not parse, or a Reload the backend refused (Busy). Empty when there is nothing to report.
 *
 * Threads: a QObject of the thread that created it (the UI thread); every member except
 * callbacks()'s functions is called there. The callbacks may run on any thread and outlive the
 * controller: after its destruction they do nothing.
 */
#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/core/error.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/ui/cmake/cmake_tree_model.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::ui::cmake {

// What CMakeController drives. CMakeServiceBackend adapts CMakeService; tests use a fake.
class ICMakeBackend {
public:
    virtual ~ICMakeBackend() = default;
    // Configure the active profile in the background (the Reload button). Errors: the service's.
    [[nodiscard]] virtual Result<void> reload() = 0;
    // Make profile `index` active. Errors: the service's (Busy during a job, OutOfRange).
    [[nodiscard]] virtual Result<void> selectProfile(std::size_t index) = 0;
    // Stop the running configure or build.
    virtual void cancel() = 0;
    // The presets files shown under the CMake node, and why the presets could not be read.
    [[nodiscard]] virtual std::vector<std::string> presetFiles() const = 0;
    [[nodiscard]] virtual std::optional<std::string> presetsError() const = 0;

protected:
    ICMakeBackend() = default;
    ICMakeBackend(const ICMakeBackend&) = default;
    ICMakeBackend& operator=(const ICMakeBackend&) = default;
};

// ICMakeBackend over a CMakeService, which must outlive it.
class CMakeServiceBackend final : public ICMakeBackend {
public:
    explicit CMakeServiceBackend(build::CMakeService& service) noexcept : m_service(service) {}
    [[nodiscard]] Result<void> reload() override { return m_service.configure(); }
    [[nodiscard]] Result<void> selectProfile(std::size_t index) override { return m_service.setActiveProfile(index); }
    void cancel() override { m_service.cancel(); }
    [[nodiscard]] std::vector<std::string> presetFiles() const override { return m_service.presetFiles(); }
    [[nodiscard]] std::optional<std::string> presetsError() const override { return m_service.presetsError(); }

private:
    build::CMakeService& m_service;
};

class CMakeController final : public QObject {
    Q_OBJECT

public:
    explicit CMakeController(QObject* parent = nullptr);
    ~CMakeController() override;
    CMakeController(const CMakeController&) = delete ("a QObject");
    CMakeController& operator=(const CMakeController&) = delete ("a QObject");

    // Callbacks for CMakeService::create that post each event to this controller (header comment).
    [[nodiscard]] build::CMakeServiceCallbacks callbacks();
    // The backend Reload and the profile selector act on (not owned); nullptr detaches.
    void setBackend(ICMakeBackend* backend) noexcept { m_backend = backend; }

    // The Solution Explorer's model of the folder; owned by the controller.
    [[nodiscard]] CMakeTreeModel* treeModel() const noexcept { return m_tree; }
    [[nodiscard]] const std::shared_ptr<const project::CMakeProject>& model() const noexcept { return m_model; }
    [[nodiscard]] const QStringList& profileLabels() const noexcept { return m_profileLabels; }
    [[nodiscard]] int activeProfile() const noexcept { return m_activeProfile; } // -1: none.
    [[nodiscard]] const QString& problem() const noexcept { return m_problem; }
    [[nodiscard]] bool busy() const noexcept { return m_busy; }

public slots:
    void reload();
    void selectProfile(int index);
    void cancel();

signals:
    void configureStarted(const QString& profile);
    void configureOutput(int kind, const QString& line); // kind: build::BuildOutputKind.
    void configureFinished(bool succeeded, const QString& summary);
    void profilesChanged();
    void problemChanged(const QString& problem);
    void busyChanged(bool busy);
    void modelChanged();

private:
    struct Bridge; // Shared with the callbacks; cleared by the destructor.

    void applyModel(std::shared_ptr<const project::CMakeProject> model);
    void applyProfiles(const std::vector<project::CMakeProfile>& profiles, std::optional<std::size_t> active);
    void applyConfigureFinished(const build::CMakeConfigureOutcome& outcome);
    void setProblem(QString problem);
    void setBusy(bool busy);

    std::shared_ptr<Bridge> m_bridge;
    ICMakeBackend* m_backend = nullptr;
    CMakeTreeModel* m_tree = nullptr;
    std::shared_ptr<const project::CMakeProject> m_model;
    QStringList m_profileLabels;
    std::vector<std::string> m_profileProblems;
    int m_activeProfile = -1;
    QString m_problem;
    QString m_configureProblem;
    bool m_busy = false;
};

} // namespace frost::ui::cmake
