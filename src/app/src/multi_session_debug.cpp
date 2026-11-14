// SPDX-License-Identifier: Apache-2.0
// Compound debugger transaction, including partial launch failure cleanup and cancellable admission.
#include "multi_session_debug.hpp"

#include <QPointer>

#include <algorithm>
#include <set>
#include <utility>
namespace frost::app {
struct MultiSessionDebugCoordinator::Job {
    ui::DebugGroupId group = 0;
    std::vector<run::RunConfiguration> members;
    std::vector<PreparedDebugLaunch> prepared;
    std::function<bool()> admitted;
    Prepare prepare;
    AdditionalPrepare additional;
    std::stop_source stop;
    bool awaiting = false;
    bool starting = false;
    std::size_t nextStart = 0;
};
MultiSessionDebugCoordinator::MultiSessionDebugCoordinator(ui::DebugSessionRegistry& sessions, QObject* parent)
    : QObject(parent), m_sessions(sessions) {
    connect(&sessions, &ui::DebugSessionRegistry::groupStopRequested, this, [this](ui::DebugGroupId group) {
        if (m_job && m_job->group == group)
            cancel();
    });
    connect(&sessions, &ui::DebugSessionRegistry::groupFailed, this,
            [this](ui::DebugGroupId group, const QString& error) {
                if (m_job && m_job->group == group) {
                    const auto job = m_job;
                    abort(job, Error(ErrorCode::IoError, error.toStdString()));
                } else
                    emit failed(error);
            });
}
MultiSessionDebugCoordinator::~MultiSessionDebugCoordinator() { cancel(); }
Result<ui::DebugGroupId> MultiSessionDebugCoordinator::start(std::vector<run::RunConfiguration> members,
                                                             std::function<bool()> admitted, Prepare prepare) {
    if (m_job)
        return fail(ErrorCode::Busy, "A compound debugger launch is still preparing");
    if (members.empty() || members.size() > 32 || !prepare || !admitted)
        return fail(ErrorCode::InvalidArgument, "A compound debugger needs 1 to 32 admitted members");
    std::set<std::string> seen;
    for (const auto& member : members) {
        if (auto valid = run::validateRunConfiguration(member); !valid)
            return std::unexpected(valid.error());
        if (member.type == run::RunType::Compound || !seen.insert(member.id).second)
            return fail(ErrorCode::InvalidArgument,
                        "Compound debug members must be unique non-compound configurations");
    }
    if (!admitted())
        return fail(ErrorCode::Untrusted, "Workspace admission changed before compound debugging");
    auto job = std::make_shared<Job>();
    job->group = m_sessions.createGroup();
    if (!job->group)
        return fail(ErrorCode::Busy, "The debugger cannot create another compound group");
    job->members = std::move(members);
    job->admitted = std::move(admitted);
    job->prepare = std::move(prepare);
    job->additional = m_additional;
    m_job = job;
    emit stateChanged();
    advance(job);
    return job->group;
}
void MultiSessionDebugCoordinator::advance(const std::shared_ptr<Job>& job) {
    if (m_job != job || job->stop.stop_requested())
        return;
    if (!job->admitted()) {
        abort(job, Error(ErrorCode::Untrusted, "Workspace admission changed during compound debugging"));
        return;
    }
    if (job->prepared.size() == job->members.size()) {
        job->starting = true;
        startNext(job);
        return;
    }
    const auto index = job->prepared.size();
    job->awaiting = true;
    QPointer<MultiSessionDebugCoordinator> self(this);
    Prepared completed = [self, job, index](Result<PreparedDebugLaunch> prepared) mutable {
        if (!self || self->m_job != job || job->stop.stop_requested() || job->starting || !job->awaiting ||
            job->prepared.size() != index)
            return;
        job->awaiting = false;
        if (!prepared) {
            self->abort(job, prepared.error());
            return;
        }
        job->prepared.push_back(std::move(*prepared));
        self->advance(job);
    };
    Result<void> queued;
    if (job->additional) {
        auto handled = job->additional(job->members[index], job->stop.get_token(), completed);
        if (handled)
            queued = std::move(*handled);
        else
            queued = job->prepare(job->members[index], job->stop.get_token(), std::move(completed));
    } else {
        queued = job->prepare(job->members[index], job->stop.get_token(), std::move(completed));
    }
    if (!self)
        return;
    if (!queued && m_job == job && !job->starting && job->awaiting)
        abort(job, queued.error());
}
void MultiSessionDebugCoordinator::startNext(const std::shared_ptr<Job>& job) {
    while (m_job == job && !job->stop.stop_requested()) {
        if (!job->admitted()) {
            abort(job, Error(ErrorCode::Untrusted, "Workspace admission changed before an adapter launch"));
            return;
        }
        if (job->nextStart == job->prepared.size()) {
            m_job.reset();
            m_sessions.finishPreparing(job->group);
            emit stateChanged();
            return;
        }
        auto& member = job->prepared[job->nextStart];
        if (bool(member.start) == bool(member.startPhased)) {
            abort(job, Error(ErrorCode::Corrupt, "A compound member must supply exactly one debugger launch form"));
            return;
        }
        if (member.start) {
            auto started = member.start(m_sessions, job->group);
            if (!started) {
                abort(job, started.error());
                return;
            }
            ++job->nextStart;
            continue;
        }
        const auto index = job->nextStart;
        job->awaiting = true;
        QPointer<MultiSessionDebugCoordinator> self(this);
        PreparedDebugLaunch::Started done = [self, job, index](Result<std::vector<ui::DebugSessionId>> ids) {
            if (!self || self->m_job != job || job->stop.stop_requested() || !job->starting || !job->awaiting ||
                job->nextStart != index)
                return;
            job->awaiting = false;
            if (!ids) {
                self->abort(job, ids.error());
                return;
            }
            if (!job->admitted()) {
                self->abort(job, Error(ErrorCode::Untrusted, "Workspace admission changed during phased launch"));
                return;
            }
            std::set<ui::DebugSessionId> unique;
            const auto sessions = self->m_sessions.sessions();
            if (ids->empty() || ids->size() > 128 || std::ranges::any_of(*ids, [&](auto id) {
                    const auto entry = std::ranges::find(sessions, id, &ui::DebugSessionEntry::id);
                    return !unique.insert(id).second || entry == sessions.end() || entry->group != job->group ||
                           !entry->controller->session() ||
                           entry->controller->session()->state() == debug::DebugState::Ended;
                })) {
                self->abort(job, Error(ErrorCode::Corrupt, "A phased launch did not return its live group sessions"));
                return;
            }
            ++job->nextStart;
            self->startNext(job);
        };
        auto accepted = member.startPhased(m_sessions, job->group, job->stop.get_token(), std::move(done));
        if (self && !accepted && m_job == job && job->awaiting && job->nextStart == index)
            abort(job, accepted.error());
        return;
    }
}
void MultiSessionDebugCoordinator::abort(const std::shared_ptr<Job>& job, const Error& error) {
    if (m_job != job)
        return;
    m_job.reset();
    job->stop.request_stop();
    m_sessions.rollbackGroup(job->group);
    emit stateChanged();
    emit failed(QString::fromStdString(error.message()));
}
void MultiSessionDebugCoordinator::cancel() {
    if (!m_job)
        return;
    auto job = std::move(m_job);
    job->stop.request_stop();
    m_sessions.stopGroup(job->group);
    emit stateChanged();
}
} // namespace frost::app
