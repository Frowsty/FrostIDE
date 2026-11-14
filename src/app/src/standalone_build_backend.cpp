// SPDX-License-Identifier: Apache-2.0
// GNU Make execution through the same contained BuildSession and UI events as other builds.
#include "standalone_build_backend.hpp"

#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include <QAction>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>

namespace frost::app {
void StandaloneBuildBackend::attachMenu(ui::ShellWindow& window) {
    if (!m_model.makefile)
        return;
    m_menu = new QMenu(tr("Make"), &window);
    window.menuBar()->addMenu(m_menu);
    auto report = [&window](Result<void> result) {
        if (!result)
            QMessageBox::warning(&window, tr("Make"), QString::fromStdString(result.error().message()));
    };
    connect(m_menu->addAction(tr("Build target…")), &QAction::triggered, this, [this, &window, report] {
        bool accepted = false;
        const auto target =
            QInputDialog::getText(&window, tr("Make target"), tr("Literal target (empty uses the default):"),
                                  QLineEdit::Normal, {}, &accepted);
        if (accepted)
            report(buildTarget(project::MakeAction::Build, target.toStdString()));
    });
    connect(m_menu->addAction(tr("Install")), &QAction::triggered, this,
            [this, report] { report(buildTarget(project::MakeAction::Install)); });
}
StandaloneBuildBackend::StandaloneBuildBackend(project::StandaloneProject model, io::IFileSystem& fs,
                                               io::IProcessLauncher& launcher, project::TrustStore& trust,
                                               build::BuildService& admission, build::BuildServiceCallbacks callbacks,
                                               std::function<Result<std::string>()> findMake)
    : m_model(std::move(model)), m_fs(fs), m_launcher(launcher), m_trust(trust), m_admission(admission),
      m_callbacks(std::move(callbacks)), m_findMake(std::move(findMake)) {}

StandaloneBuildBackend::~StandaloneBuildBackend() {
    delete m_menu.data();
    if (m_session) {
        m_session->cancel();
        m_session.reset();
    }
    if (m_reserved)
        m_admission.releaseRun();
}
Result<void> StandaloneBuildBackend::openSolution(std::shared_ptr<const project::SolutionModel>) {
    return fail(ErrorCode::Unsupported, "a standalone C++ project is open");
}
Result<void> StandaloneBuildBackend::closeSolution() {
    if (m_state != build::BuildState::Idle)
        return fail(ErrorCode::Busy, "stop the Make build before closing");
    return {};
}
Result<void> StandaloneBuildBackend::setActiveConfiguration(std::size_t) {
    return fail(ErrorCode::Unsupported, "Make configuration is controlled by the project's Makefile");
}
void StandaloneBuildBackend::setState(build::BuildState state) {
    m_state = state;
    if (m_callbacks.onStateChanged)
        m_callbacks.onStateChanged(state);
}
Result<void> StandaloneBuildBackend::build(build::BuildAction action, std::optional<project::ProjectId> project) {
    if (project || action == build::BuildAction::Compile)
        return fail(ErrorCode::Unsupported,
                    "choose a literal Make target; no compiler command is inferred from a source filename");
    const auto make = action == build::BuildAction::Clean     ? project::MakeAction::Clean
                      : action == build::BuildAction::Rebuild ? project::MakeAction::Rebuild
                                                              : project::MakeAction::Build;
    auto result = buildTarget(make);
    if (result)
        m_outcome.target.action = action;
    return result;
}
Result<void> StandaloneBuildBackend::buildTarget(project::MakeAction action, std::string target) {
    if (m_state != build::BuildState::Idle)
        return fail(ErrorCode::Busy, "a Make build is already active");
    auto steps = project::makeProjectArguments(m_model, action, target);
    if (!steps)
        return std::unexpected(steps.error());
    auto reserved = m_admission.reserveRun();
    if (!reserved)
        return reserved;
    m_reserved = true;
    ++m_generation;
    m_steps = std::move(*steps);
    m_step = 0;
    m_cancelled = false;
    m_diagnostics.clear();
    m_outcome = {};
    m_outcome.target.projectName = target;
    if (m_callbacks.onDiagnosticsCleared)
        m_callbacks.onDiagnosticsCleared();
    if (!m_trust.isTrusted(m_model.root)) {
        setState(build::BuildState::NeedsTrust);
        if (m_callbacks.onNeedsTrust)
            m_callbacks.onNeedsTrust({m_model.root});
    } else
        saveAndStart();
    return {};
}
void StandaloneBuildBackend::saveAndStart() {
    setState(build::BuildState::SavingDocuments);
    const auto generation = m_generation;
    QPointer<StandaloneBuildBackend> self(this);
    auto saved = [self, generation](Result<void> result) mutable {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, generation, result = std::move(result)]() mutable {
                if (!self || self->m_generation != generation || self->m_state != build::BuildState::SavingDocuments)
                    return;
                if (!result)
                    self->finish(result.error());
                else
                    self->next();
            },
            Qt::QueuedConnection);
    };
    if (m_save)
        m_save(std::move(saved));
    else
        saved({});
}
void StandaloneBuildBackend::next() {
    // Called only on the GUI thread, after the preceding session's last event.
    m_session.reset();
    if (m_cancelled) {
        finish(fail(ErrorCode::Cancelled, "Make build cancelled").error());
        return;
    }
    if (m_step == m_steps.size()) {
        finish();
        return;
    }
    if (!m_trust.isTrusted(m_model.root)) {
        finish(fail(ErrorCode::PermissionDenied, "project trust was revoked").error());
        return;
    }
    setState(build::BuildState::Starting);
    Result<std::string> executable =
        fail(ErrorCode::NotFound, "GNU Make unavailable: install make/gmake and add its directory to PATH");
    if (m_findMake)
        executable = m_findMake();
    else {
        QString found = QStandardPaths::findExecutable(QStringLiteral("gmake"));
        if (found.isEmpty())
            found = QStandardPaths::findExecutable(QStringLiteral("make"));
        if (!found.isEmpty())
            executable = found.toStdString();
    }
    if (!executable) {
        finish(executable.error());
        return;
    }
    io::ProcessSpec spec;
    spec.executable = *executable;
    spec.arguments = m_steps[m_step++];
    spec.workingDirectory = m_model.root;
    spec.environment.emplace_back("NoDefaultCurrentDirectoryInExePath", std::nullopt);
    QPointer<StandaloneBuildBackend> self(this);
    build::BuildCallbacks callbacks;
    callbacks.onLine = [self](auto stream, std::string_view line) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, stream, line = std::string(line)] {
                if (self && self->m_callbacks.onOutput)
                    self->m_callbacks.onOutput(stream == build::OutputStream::Stdout ? build::BuildOutputKind::Stdout
                                                                                     : build::BuildOutputKind::Stderr,
                                               line);
            },
            Qt::QueuedConnection);
    };
    callbacks.onDiagnostic = [self](const build::Diagnostic& diagnostic) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, diagnostic] {
                if (!self)
                    return;
                build::DiagnosticResolver resolver(self->m_fs, self->m_model.root);
                auto resolved = resolver.resolve(diagnostic);
                self->m_diagnostics.push_back(resolved);
                if (self->m_callbacks.onDiagnostic)
                    self->m_callbacks.onDiagnostic(resolved);
            },
            Qt::QueuedConnection);
    };
    callbacks.onFinished = [self](const build::BuildResult& result) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, result] {
                if (!self)
                    return;
                self->m_outcome.result = result;
                if (result.success && !self->m_cancelled)
                    self->next();
                else
                    self->finish();
            },
            Qt::QueuedConnection);
    };
    build::BuildSessionOptions options;
    options.parser = build::makeCompilerOutputParser;
    auto session = build::BuildSession::start(m_launcher, spec, true, std::move(callbacks), systemClock(), options);
    if (!session) {
        finish(session.error());
        return;
    }
    m_session = std::move(*session);
    setState(build::BuildState::Running);
}
void StandaloneBuildBackend::finish(std::optional<Error> error) {
    m_session.reset();
    m_outcome.error = std::move(error);
    if (m_outcome.error)
        m_outcome.result.success = false;
    m_outcome.result.cancelled = m_outcome.result.cancelled || m_cancelled;
    if (m_outcome.result.cancelled)
        m_outcome.result.success = false;
    m_outcome.counts = {};
    for (const auto& item : m_diagnostics) {
        if (item.diagnostic.severity == build::Severity::Error)
            ++m_outcome.counts.errors;
        else if (item.diagnostic.severity == build::Severity::Warning)
            ++m_outcome.counts.warnings;
        else
            ++m_outcome.counts.messages;
    }
    if (m_reserved) {
        m_admission.releaseRun();
        m_reserved = false;
    }
    if (m_callbacks.onFinished)
        m_callbacks.onFinished(m_outcome);
    setState(build::BuildState::Idle);
}
void StandaloneBuildBackend::cancel() {
    if (m_state == build::BuildState::Idle)
        return;
    m_cancelled = true;
    if (m_session) {
        setState(build::BuildState::Cancelling);
        m_session->cancel();
    } else {
        ++m_generation;
        finish(fail(ErrorCode::Cancelled, "Make build cancelled").error());
    }
}
Result<void> StandaloneBuildBackend::trust(std::string_view root) {
    auto result = m_trust.trust(root);
    if (result && m_state == build::BuildState::NeedsTrust && m_trust.isTrusted(m_model.root))
        saveAndStart();
    return result;
}
std::vector<std::string> StandaloneBuildBackend::untrustedRoots() const {
    return m_trust.isTrusted(m_model.root) ? std::vector<std::string>{} : std::vector<std::string>{m_model.root};
}
} // namespace frost::app
