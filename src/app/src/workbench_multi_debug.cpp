// SPDX-License-Identifier: Apache-2.0
// Workspace compound debugger preparation; every member resolves before any DAP process starts.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include "cmake_integration.hpp"
#include "multi_session_debug.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QProcessEnvironment>

#include <algorithm>
#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

namespace frost::app {
namespace {
using Prepared = MultiSessionDebugCoordinator::Prepared;
using Cancellation = std::stop_callback<std::function<void()>>;
PreparedDebugLaunch nativeLaunch(debug::DebugTarget target) {
    return {[target = std::move(target)](ui::DebugSessionRegistry& sessions, ui::DebugGroupId group) {
        return sessions.startTarget(target, group);
    }};
}
void applyEnvironment(debug::DebugTarget& target, const std::vector<run::EnvironmentEntry>& entries) {
    if (std::ranges::any_of(entries, [](const auto& entry) { return !entry.value; })) {
        auto environment = target.inheritEnvironment ? QProcessEnvironment::systemEnvironment() : QProcessEnvironment{};
        for (const auto& [name, value] : target.environment)
            environment.insert(QString::fromStdString(name), QString::fromStdString(value));
        for (const auto& entry : entries) {
            if (entry.value)
                environment.insert(QString::fromStdString(entry.name), QString::fromStdString(*entry.value));
            else
                environment.remove(QString::fromStdString(entry.name));
        }
        target.environment.clear();
        for (const auto& name : environment.keys())
            target.environment.emplace_back(name.toStdString(), environment.value(name).toStdString());
        target.inheritEnvironment = false;
    } else {
        for (const auto& entry : entries)
            target.environment.emplace_back(entry.name, *entry.value);
    }
}
Result<void> prepareTasks(run::RunManager& runs, const run::RunConfiguration& configuration, debug::DebugTarget target,
                          std::stop_token stop, Prepared done, QObject* context,
                          std::function<Result<void>()> admission) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "The debugger preparation was cancelled");
    if (configuration.beforeLaunchTasks.empty()) {
        done(nativeLaunch(std::move(target)));
        return {};
    }
    auto prepared = configuration;
    prepared.type = run::RunType::Native;
    prepared.target.clear();
    prepared.engineProjectPath.clear();
    prepared.beforeLaunch = run::BeforeLaunch::None;
    prepared.program = target.program;
    prepared.arguments = target.arguments;
    prepared.workingDirectory = target.workingDirectory;
    auto cancellation = std::make_shared<std::unique_ptr<Cancellation>>();
    const QPointer<QObject> guarded(context);
    run::RunHandlers handlers;
    handlers.launchAdmission = std::move(admission);
    handlers.onFinished = [guarded, target = std::move(target), done = std::move(done),
                           cancellation](const run::RunResult& result) mutable {
        if (!guarded)
            return;
        QMetaObject::invokeMethod(
            guarded.data(),
            [guarded, target = std::move(target), done = std::move(done), cancellation, result]() mutable {
                if (!guarded)
                    return;
                cancellation->reset();
                if (result.error)
                    done(std::unexpected(*result.error));
                else if (result.stopped)
                    done(fail(ErrorCode::Cancelled, "The debugger before-launch tasks were stopped"));
                else
                    done(nativeLaunch(std::move(target)));
            },
            Qt::QueuedConnection);
    };
    auto accepted = runs.start(prepared, std::move(handlers), run::RunMode::Prepare);
    if (!accepted)
        return std::unexpected(accepted.error());
    *cancellation = std::make_unique<Cancellation>(stop, [&runs, id = *accepted] { runs.stop(id); });
    return {};
}
} // namespace

Result<void> Workbench::startSelectedDebugConfiguration() {
    if (!m_debug || !m_debugSessions || !m_runController || !m_compoundDebug)
        return fail(ErrorCode::Unsupported, "The debugger workspace is unavailable");
    const auto* selected = m_runController->selectedConfiguration();
    if (selected && selected->type == run::RunType::Compound)
        return startCompoundDebug(*selected);
    // Preserve the established first-session launch path, including MSBuild/CMake Save All/build
    // and attach prompts. Additional sessions use the independent compound preparation machinery.
    if (!m_debugSessions->anyDebugging() && !m_debug->debugging() &&
        !(selected && run::isFrostEngineRunType(selected->type))) {
        static_cast<void>(m_debugSessions->select(m_debugSessions->primaryId()));
        return m_debug->debug();
    }
    if (!selected)
        return fail(ErrorCode::InvalidArgument, "Choose a run configuration for the additional debugger session");
    auto single = *selected;
    single.type = run::RunType::Compound;
    single.configurations = {selected->id};
    return startCompoundDebug(single);
}

Result<void> Workbench::startCompoundDebug(const run::RunConfiguration& configuration) {
    if (!m_compoundDebug || !m_runController || !m_runManager || m_shutDown)
        return fail(ErrorCode::Unsupported, "The compound debugger workspace is unavailable");
    const auto root = m_projectRoot;
    const auto trust = m_services.trust.snapshot(root);
    const QPointer<Workbench> self(this);
    if (auto permission = mayRunConfiguration(configuration,
                                              [self, configuration] {
                                                  if (self)
                                                      static_cast<void>(self->startCompoundDebug(configuration));
                                              });
        !permission)
        return permission;
    std::vector<run::RunConfiguration> members;
    for (const auto& id : configuration.configurations) {
        const auto* member = m_runController->configurations().find(id);
        if (!member)
            return fail(ErrorCode::NotFound, "A compound debugger configuration member is missing");
        members.push_back(*member); // Immutable launch inputs survive combo/profile changes.
    }
    auto admitted = [self, root, revision = trust.revision] {
        if (!self || self->m_shutDown || self->m_projectRoot != root || self->m_buildService->projectEditInProgress())
            return false;
        const auto current = self->m_services.trust.snapshot(root);
        return current.trusted && current.revision == revision;
    };
    auto prepare = [this, self, root, revision = trust.revision](const run::RunConfiguration& member,
                                                                 std::stop_token stop, Prepared done) -> Result<void> {
        if (!self || stop.stop_requested())
            return fail(ErrorCode::Cancelled, "The compound debugger preparation was cancelled");
        auto finalAdmission = [store = &m_services.trust, root, revision, stop]() -> Result<void> {
            if (stop.stop_requested())
                return fail(ErrorCode::Cancelled, "The debugger preparation was cancelled before child launch");
            const auto current = store->snapshot(root);
            if (!current.trusted || current.revision != revision)
                return fail(ErrorCode::Untrusted, "Project trust changed before debugger preparation child launch");
            return {};
        };
        if (member.type == run::RunType::Native) {
            auto plan = run::launchPlanFor(member);
            if (!plan)
                return std::unexpected(plan.error());
            debug::DebugTarget target;
            target.name = member.name;
            target.program = plan->program;
            target.arguments = plan->arguments;
            target.workingDirectory = plan->workingDirectory;
            target.trustRoots = {root};
            applyEnvironment(target, plan->environment);
            return prepareTasks(*m_runManager, member, std::move(target), stop, std::move(done), this, finalAdmission);
        }
        if (member.type == run::RunType::CMakeTarget) {
            if (!m_cmake)
                return fail(ErrorCode::InvalidArgument, "No CMake project is open for this debug member");
            const auto previous = m_cmake->runTargetName();
            m_cmake->selectRunTarget(member.target);
            if (m_cmake->runTargetName() != member.target)
                return fail(ErrorCode::NotFound, "The CMake debug target is no longer present");
            auto cancellation = std::make_shared<std::unique_ptr<Cancellation>>();
            auto accepted =
                m_cmake->debug(member, [this, self, member, stop, previous, done = std::move(done), cancellation,
                                        finalAdmission](Result<debug::DebugTarget> target) mutable {
                    if (!self)
                        return;
                    cancellation->reset();
                    if (m_cmake)
                        m_cmake->selectRunTarget(previous);
                    if (!target) {
                        done(std::unexpected(target.error()));
                        return;
                    }
                    if (auto tasks =
                            prepareTasks(*m_runManager, member, std::move(*target), stop, done, this, finalAdmission);
                        !tasks)
                        done(std::unexpected(tasks.error()));
                });
            if (!accepted)
                return accepted;
            *cancellation = std::make_unique<Cancellation>(stop, [self] {
                if (self)
                    QMetaObject::invokeMethod(
                        self.data(),
                        [self] {
                            if (self && self->m_cmake)
                                self->m_cmake->stop();
                        },
                        Qt::QueuedConnection);
            });
            return {};
        }
        if (member.type == run::RunType::MSBuild) {
            if (!m_solution || !m_runService)
                return fail(ErrorCode::InvalidArgument, "No MSBuild solution is open for this debug member");
            const auto project = m_solution->findProjectByPath(member.target);
            if (!project)
                return fail(ErrorCode::NotFound, "The MSBuild debug project is no longer present");
            auto cancellation = std::make_shared<std::unique_ptr<Cancellation>>();
            build::DebugRunHandlers handlers;
            handlers.onFinished = [this, self, member, stop, done = std::move(done), cancellation,
                                   finalAdmission](const build::RunOutcome& outcome,
                                                   const std::optional<build::DebugLaunch>& launch) mutable {
                if (!self)
                    return;
                QMetaObject::invokeMethod(
                    self.data(),
                    [this, self, member, stop, outcome, launch, done = std::move(done), cancellation,
                     finalAdmission]() mutable {
                        if (!self)
                            return;
                        cancellation->reset();
                        if (outcome.error || !launch) {
                            done(outcome.error
                                     ? Result<PreparedDebugLaunch>(std::unexpected(*outcome.error))
                                     : Result<PreparedDebugLaunch>(
                                           fail(ErrorCode::Cancelled, "The MSBuild debug preparation did not finish")));
                            return;
                        }
                        auto target = ui::debugTargetFor(outcome, *launch);
                        if (!member.program.empty())
                            target.program = member.program;
                        if (!member.arguments.empty())
                            target.arguments = member.arguments;
                        if (!member.workingDirectory.empty())
                            target.workingDirectory = member.workingDirectory;
                        applyEnvironment(target, member.environment);
                        if (auto tasks = prepareTasks(*m_runManager, member, std::move(target), stop, done, this,
                                                      finalAdmission);
                            !tasks)
                            done(std::unexpected(tasks.error()));
                    },
                    Qt::QueuedConnection);
            };
            auto accepted = m_runService->debug(*project, std::move(handlers));
            if (!accepted)
                return std::unexpected(accepted.error());
            *cancellation = std::make_unique<Cancellation>(
                stop, [service = m_runService.get(), id = *accepted] { service->stop(id); });
            return {};
        }
        if (member.type == run::RunType::Attach) {
            std::vector<RunningProcessInfo> matches;
            for (auto process : enumerateRunningProcesses())
                if (run::attachTargetMatches(member.target, process.executablePath))
                    matches.push_back(std::move(process));
            if (matches.size() != 1)
                return fail(matches.empty() ? ErrorCode::NotFound : ErrorCode::InvalidArgument,
                            "An attach member must select exactly one actual running process");
            const auto process = matches.front();
            if (process.isDotNet)
                return fail(ErrorCode::Unsupported, "Managed compound attach requires verified managed preparation");
            debug::DebugTarget target;
            target.name = member.name;
            target.program = process.executablePath;
            target.trustRoots = {root};
            debug::AdapterFactory factory = m_options.debugAdapterFactory;
            // Empty means the controller resolves/captures the default pinned LLDB provenance at
            // actual session start. A supplied custom factory remains explicitly unverified.
            auto arguments = rpc::Json::object()
                                 .set("type", "lldb-dap")
                                 .set("request", "attach")
                                 .set("pid", static_cast<std::int64_t>(process.pid))
                                 .set("program", process.executablePath);
            PreparedDebugLaunch prepared;
            prepared.start =
                [self, process, root, target, factory = std::move(factory), arguments = std::move(arguments)](
                    ui::DebugSessionRegistry& sessions, ui::DebugGroupId group) mutable -> Result<ui::DebugSessionId> {
                auto live = [self, process, root] {
                    if (!self || self->m_projectRoot != root || !self->m_services.trust.snapshot(root).trusted)
                        return false;
                    const auto list = enumerateRunningProcesses();
                    return std::ranges::any_of(list, [&](const auto& current) {
                        return current.pid == process.pid && current.executablePath == process.executablePath;
                    });
                };
                return sessions.attachTarget(target, std::move(factory), std::move(arguments), std::move(live), group);
            };
            done(std::move(prepared));
            return {};
        }
        return fail(ErrorCode::Unsupported, "This debug configuration needs its engine adapter provider");
    };
    auto saveAndPrepare = [this, self, admitted, work = std::move(prepare)](const run::RunConfiguration& member,
                                                                            std::stop_token stop,
                                                                            Prepared done) -> Result<void> {
        saveAll([self, admitted, work, member, stop, done = std::move(done)](Result<void> saved) mutable {
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, admitted, work, member, stop, saved = std::move(saved), done = std::move(done)]() mutable {
                    if (!self || stop.stop_requested())
                        return;
                    if (!saved) {
                        done(std::unexpected(saved.error()));
                        return;
                    }
                    if (!admitted()) {
                        done(fail(ErrorCode::Untrusted, "Workspace admission changed during Save All"));
                        return;
                    }
                    if (auto accepted = work(member, stop, done); !accepted)
                        done(std::unexpected(accepted.error()));
                },
                Qt::QueuedConnection);
        });
        return {};
    };
    auto accepted = m_compoundDebug->start(std::move(members), std::move(admitted), std::move(saveAndPrepare));
    return accepted ? Result<void>{} : Result<void>(std::unexpected(accepted.error()));
}
} // namespace frost::app
