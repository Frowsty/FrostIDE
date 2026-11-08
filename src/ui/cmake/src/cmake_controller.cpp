// SPDX-License-Identifier: Apache-2.0
//
// CMakeController (see frost/ui/cmake/cmake_controller.hpp): the queued relay from the service
// thread and the state the CMake tool window shows.

#include "frost/ui/cmake/cmake_controller.hpp"

#include "frost/core/mutex.hpp"
#include "frost/project/cmake_view_model.hpp"
#include "frost/ui/tr.hpp"

#include <QMetaObject>

#include <string>
#include <utility>
#include <vector>

namespace frost::ui::cmake {

namespace {

constexpr std::string_view kContext = "cmake";

QString toQString(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

// The callbacks' handle on the controller: null once it is destroyed. Posting happens under the
// lock, so the controller cannot go away between the check and QMetaObject::invokeMethod; Qt
// deletes the events still queued for a destroyed object, so a posted function always finds its
// controller alive.
struct CMakeController::Bridge {
    Mutex mutex;
    CMakeController* controller FROST_GUARDED_BY(mutex) = nullptr;
    ICMakeBackend* backend FROST_GUARDED_BY(mutex) = nullptr; // setBackend's, for presetFiles().

    // The backend's presets files, read on the calling thread (empty without a backend). The lock
    // keeps a backend that setBackend is detaching alive until the read ends.
    std::vector<std::string> presetFiles() {
        LockGuard lock(mutex);
        return backend != nullptr ? backend->presetFiles() : std::vector<std::string>{};
    }

    // Runs function(controller) on the controller's thread, unless it was destroyed.
    template<class F>
    void post(F function) {
        LockGuard lock(mutex);
        if (controller == nullptr) {
            return;
        }
        CMakeController* target = controller;
        QMetaObject::invokeMethod(
            target, [target, function = std::move(function)]() mutable { function(*target); }, Qt::QueuedConnection);
    }
};

CMakeController::CMakeController(QObject* parent)
    : QObject(parent), m_bridge(std::make_shared<Bridge>()), m_tree(new CMakeTreeModel(this)) {
    LockGuard lock(m_bridge->mutex);
    m_bridge->controller = this;
}

CMakeController::~CMakeController() {
    LockGuard lock(m_bridge->mutex);
    m_bridge->controller = nullptr;
    m_bridge->backend = nullptr;
}

void CMakeController::setBackend(ICMakeBackend* backend) noexcept {
    m_backend = backend;
    LockGuard lock(m_bridge->mutex);
    m_bridge->backend = backend;
}

build::CMakeServiceCallbacks CMakeController::callbacks() {
    build::CMakeServiceCallbacks out;
    const std::shared_ptr<Bridge> bridge = m_bridge;
    out.onStateChanged = [bridge](build::BuildState state) {
        bridge->post([state](CMakeController& self) { self.setBusy(state != build::BuildState::Idle); });
    };
    out.onConfigureStarted = [bridge](const std::string& profile) {
        bridge->post([text = toQString(profile)](CMakeController& self) { emit self.configureStarted(text); });
    };
    out.onConfigureOutput = [bridge](build::BuildOutputKind kind, std::string_view line) {
        bridge->post([kind, text = toQString(line)](CMakeController& self) {
            emit self.configureOutput(static_cast<int>(kind), text);
        });
    };
    out.onConfigureFinished = [bridge](const build::CMakeConfigureOutcome& outcome) {
        bridge->post([outcome](CMakeController& self) { self.applyConfigureFinished(outcome); });
    };
    out.onModelChanged = [bridge](std::shared_ptr<const project::CMakeProject> model) {
        // The tree is built here, on the service's thread (header comment, Threads).
        std::shared_ptr<const project::CMakeViewModel> view;
        if (model) {
            view = std::make_shared<const project::CMakeViewModel>(
                project::CMakeViewModel::build(*model, bridge->presetFiles()));
        }
        bridge->post([model = std::move(model), view = std::move(view)](CMakeController& self) mutable {
            self.applyModel(std::move(model), std::move(view));
        });
    };
    out.onProfilesChanged = [bridge](const std::vector<project::CMakeProfile>& profiles,
                                     std::optional<std::size_t> active) {
        bridge->post([profiles, active](CMakeController& self) { self.applyProfiles(profiles, active); });
    };
    out.onProgress = [bridge](const build::CMakeProgress& progress) {
        bridge->post([progress](CMakeController& self) { self.applyProgress(progress); });
    };
    return out;
}

void CMakeController::reload() {
    if (m_backend == nullptr) {
        return;
    }
    if (auto started = m_backend->reload(); !started) {
        setProblem(toQString(started.error().message()));
    }
}

void CMakeController::selectProfile(int index) {
    if (m_backend == nullptr || index < 0 || index == m_activeProfile) {
        return;
    }
    if (auto selected = m_backend->selectProfile(static_cast<std::size_t>(index)); !selected) {
        setProblem(toQString(selected.error().message()));
        emit profilesChanged(); // The selector goes back to the active profile.
    }
}

void CMakeController::cancel() {
    if (m_backend != nullptr) {
        m_backend->cancel();
    }
}

void CMakeController::applyModel(std::shared_ptr<const project::CMakeProject> model,
                                 std::shared_ptr<const project::CMakeViewModel> view) {
    m_model = std::move(model);
    m_tree->setViewModel(m_model ? std::move(view) : nullptr);
    emit modelChanged();
}

void CMakeController::applyProfiles(const std::vector<project::CMakeProfile>& profiles,
                                    std::optional<std::size_t> active) {
    m_profileLabels.clear();
    m_profileProblems.clear();
    for (const project::CMakeProfile& profile : profiles) {
        m_profileLabels.push_back(toQString(profile.displayName.empty() ? profile.name : profile.displayName));
        m_profileProblems.push_back(profile.problem);
    }
    m_activeProfile = active ? static_cast<int>(*active) : -1;
    m_configureProblem.clear(); // A new profile has not failed yet.
    // The profiles change whenever the presets are (re)read, so the listing is read with them.
    m_presetListing = m_backend != nullptr ? m_backend->presetListing() : project::CMakePresetListing{};
    QString problem;
    if (m_activeProfile >= 0 && !m_profileProblems[static_cast<std::size_t>(m_activeProfile)].empty()) {
        problem = toQString(m_profileProblems[static_cast<std::size_t>(m_activeProfile)]);
    } else if (m_backend != nullptr) {
        if (auto presets = m_backend->presetsError()) {
            problem = qtr(kContext, "The CMake presets could not be read: ") + toQString(*presets);
        }
    }
    setProblem(problem);
    emit profilesChanged();
}

void CMakeController::applyConfigureFinished(const build::CMakeConfigureOutcome& outcome) {
    QString summary;
    if (outcome.succeeded()) {
        summary = qtr(kContext, "CMake configured successfully");
        m_configureProblem.clear();
    } else if (outcome.result.cancelled) {
        summary = qtr(kContext, "CMake configure was cancelled");
        m_configureProblem.clear();
    } else if (outcome.error) {
        summary = qtr(kContext, "CMake configure failed: ") + toQString(outcome.error->message());
        m_configureProblem = summary;
    } else {
        summary = qtr(kContext, "CMake configure failed with %1 error(s); see the CMake output")
                      .arg(static_cast<qulonglong>(outcome.counts.errors));
        m_configureProblem = summary;
    }
    setProblem(m_configureProblem);
    emit configureFinished(outcome.succeeded(), summary);
}

void CMakeController::setProblem(QString problem) {
    if (problem == m_problem) {
        return;
    }
    m_problem = std::move(problem);
    emit problemChanged(m_problem);
}

void CMakeController::setBusy(bool busy) {
    if (busy == m_busy) {
        return;
    }
    m_busy = busy;
    emit busyChanged(m_busy);
}

void CMakeController::applyProgress(const build::CMakeProgress& progress) {
    // Queued posts arrive in order, but the service's direct deliveries (its reply reads) race the
    // queued ones: the greatest sequence number is the newest state.
    if (progress.sequence <= m_progress.sequence) { // The service numbers from 1.
        return;
    }
    m_progress = progress;
    emit progressChanged();
}

QString CMakeController::progressText() const {
    const QString profile = toQString(m_progress.profile);
    switch (m_progress.activity) {
    case build::CMakeActivity::Idle: return {};
    case build::CMakeActivity::ReadingModel: return qtr(kContext, "Reading the CMake project model");
    case build::CMakeActivity::WaitingForTrust: return qtr(kContext, "Waiting for the folder to be trusted");
    case build::CMakeActivity::Preparing: return qtr(kContext, "Preparing to configure %1").arg(profile);
    case build::CMakeActivity::Configuring: return qtr(kContext, "Configuring %1").arg(profile);
    case build::CMakeActivity::Generating: return qtr(kContext, "Generating %1").arg(profile);
    case build::CMakeActivity::Cleaning: return qtr(kContext, "Cleaning %1").arg(profile);
    case build::CMakeActivity::Building: return qtr(kContext, "Building %1").arg(profile);
    case build::CMakeActivity::Cancelling: return qtr(kContext, "Cancelling");
    }
    return {};
}

} // namespace frost::ui::cmake
