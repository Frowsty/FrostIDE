// SPDX-License-Identifier: Apache-2.0
//
// CMakeController (see frost/ui/cmake/cmake_controller.hpp): the queued relay from the service
// thread and the state the CMake tool window shows.

#include "frost/ui/cmake/cmake_controller.hpp"

#include "frost/core/mutex.hpp"
#include "frost/project/cmake_view_model.hpp"
#include "frost/ui/tr.hpp"

#include <QMetaObject>

#include <utility>

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
        bridge->post([model = std::move(model)](CMakeController& self) mutable { self.applyModel(std::move(model)); });
    };
    out.onProfilesChanged = [bridge](const std::vector<project::CMakeProfile>& profiles,
                                     std::optional<std::size_t> active) {
        bridge->post([profiles, active](CMakeController& self) { self.applyProfiles(profiles, active); });
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

void CMakeController::applyModel(std::shared_ptr<const project::CMakeProject> model) {
    m_model = std::move(model);
    if (!m_model) {
        m_tree->setViewModel(nullptr);
    } else {
        const std::vector<std::string> presets =
            m_backend != nullptr ? m_backend->presetFiles() : std::vector<std::string>{};
        m_tree->setViewModel(
            std::make_shared<const project::CMakeViewModel>(project::CMakeViewModel::build(*m_model, presets)));
    }
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

} // namespace frost::ui::cmake
