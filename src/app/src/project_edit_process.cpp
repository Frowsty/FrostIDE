// SPDX-License-Identifier: Apache-2.0
// Owned two-phase ProjectHost process for one project-definition edit.
#include "project_edit_process.hpp"

#include "frost/core/mutex.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

namespace frost::app {
struct ProjectEditProcess::Relay {
    Mutex mutex;
    ProjectEditProcess* target FROST_GUARDED_BY(mutex) = nullptr;
    template<class F>
    void post(F fn) {
        LockGuard lock(mutex);
        if (target) {
            auto* self = target;
            QMetaObject::invokeMethod(self, [self, fn = std::move(fn)] { fn(*self); }, Qt::QueuedConnection);
        }
    }
};

ProjectEditProcess::ProjectEditProcess(io::IProcessLauncher& launcher, IClock& clock, QObject* parent)
    : QObject(parent), m_launcher(launcher), m_clock(clock), m_relay(std::make_shared<Relay>()) {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = this;
    }
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_error = "Project edit timed out";
        stop();
    });
}

ProjectEditProcess::~ProjectEditProcess() {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    stop();
}

Result<void> ProjectEditProcess::start(io::ProcessSpec spec, std::string request, std::string transactionId,
                                       Admit admit, Complete complete) {
    if (m_session)
        return fail(ErrorCode::Busy, "A project edit is already running");
    m_output.clear();
    m_error.clear();
    m_accepted = false;
    m_resultSeen = false;
    m_success = false;
    m_cancelled = false;
    m_id = std::move(transactionId);
    m_admit = std::move(admit);
    m_complete = std::move(complete);
    spec.stdinMode = io::Stdio::Pipe;
    auto relay = m_relay;
    build::RunSessionCallbacks callbacks;
    callbacks.onOutput = [relay](build::RunStream stream, std::string_view text) {
        relay->post([stream, chunk = std::string(text)](ProjectEditProcess& self) { self.output(stream, chunk); });
    };
    callbacks.onExited = [relay](const build::RunExit& exit) {
        relay->post([exit](ProjectEditProcess& self) { self.exited(exit); });
    };
    auto started = build::RunSession::start(m_launcher, spec, std::move(callbacks), m_clock,
                                            {.terminateDescendantsOnExit = true, .rejectInvalidUtf8 = true});
    if (!started)
        return std::unexpected(std::move(started.error()));
    m_session = std::move(*started);
    if (auto written = m_session->writeInput(std::move(request) + "\n"); !written) {
        m_session->stop();
        return std::unexpected(std::move(written.error()));
    }
    m_timeout.start(120000);
    return {};
}

void ProjectEditProcess::output(build::RunStream stream, const std::string& chunk) {
    if (stream != build::RunStream::Stdout || m_resultSeen || m_cancelled)
        return;
    m_output += chunk;
    if (m_output.size() > 8 * 1024 * 1024) {
        m_error = "ProjectHost response is too large";
        stop();
        return;
    }
    for (std::size_t newline; (newline = m_output.find('\n')) != std::string::npos;) {
        const QByteArray line(m_output.data(), static_cast<qsizetype>(newline));
        m_output.erase(0, newline + 1);
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            m_error = "Invalid ProjectHost response";
            stop();
            return;
        }
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("Ready")).toBool()) {
            if (m_accepted || object.value(QStringLiteral("TransactionId")).toString().toStdString() != m_id) {
                m_error = "Unexpected ProjectHost transaction";
                stop();
                return;
            }
            const std::string digest = object.value(QStringLiteral("JournalDigest")).toString().toStdString();
            if (digest.size() != 64 || digest.find_first_not_of("0123456789ABCDEF") != std::string::npos) {
                m_error = "Invalid ProjectHost journal digest";
                stop();
                return;
            }
            const bool allow = !m_cancelled && m_admit && m_admit(digest);
            if (m_cancelled)
                return;
            QJsonObject decision{{QStringLiteral("Accept"), allow ? QStringLiteral("true") : QStringLiteral("false")},
                                 {QStringLiteral("TransactionId"), QString::fromStdString(m_id)}};
            m_accepted = allow;
            auto sent =
                m_session->writeInput(QJsonDocument(decision).toJson(QJsonDocument::Compact).toStdString() + "\n");
            if (!sent) {
                m_error = sent.error().message();
                stop();
                return;
            }
            m_session->closeInput();
        } else {
            m_resultSeen = true;
            const QJsonValue error = object.value(QStringLiteral("Error"));
            m_success = m_accepted && error.isNull();
            if (!error.isNull())
                m_error = error.toString().toStdString();
            m_session->closeInput();
        }
    }
}

void ProjectEditProcess::exited(const build::RunExit& result) {
    m_timeout.stop();
    const bool success = m_success && m_resultSeen && result.exitCode == 0 && !result.waitFailed &&
                         !result.cleanupFailed && !result.decodingFailed && !result.stopped;
    if (!success && m_error.empty())
        m_error = "ProjectHost ended without a successful response";
    auto done = std::move(m_complete);
    m_session.reset();
    if (done)
        done(success, m_accepted, m_error);
}
void ProjectEditProcess::stop() {
    m_cancelled = true;
    if (m_session)
        m_session->stop();
}
} // namespace frost::app
