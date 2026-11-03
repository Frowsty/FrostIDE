// SPDX-License-Identifier: Apache-2.0
// Own the asynchronous managed build, target query and one-module portable-PDB verification.
#include "managed_debug_preparation.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/rpc/json.hpp"

#include <QMetaObject>

#include <format>
#include <utility>

namespace frost::app {
namespace {
constexpr std::size_t kMaxOutput = 4 * 1024 * 1024;
constexpr std::size_t kMaxQueuedOutput = 256 * 1024;
constexpr std::size_t kMaxQueuedLines = 1024;
struct Capture {
    std::string output{};
    std::string error{};
    bool overflow = false;
};
bool hexDigest(std::string_view value, std::size_t length) {
    if (value.size() != length)
        return false;
    for (char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}
bool moduleGuid(std::string_view value) {
    if (value.size() != 36)
        return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-')
                return false;
        } else if (!hexDigest(value.substr(i, 1), 1)) {
            return false;
        }
    }
    return true;
}
} // namespace

struct ManagedDebugPreparation::Relay : std::enable_shared_from_this<Relay> {
    Mutex mutex;
    ManagedDebugPreparation* target FROST_GUARDED_BY(mutex) = nullptr;
    std::size_t queuedBytes FROST_GUARDED_BY(mutex) = 0;
    std::size_t queuedLines FROST_GUARDED_BY(mutex) = 0;
    bool truncationQueued FROST_GUARDED_BY(mutex) = false;
    void post(std::uint64_t generation, Phase phase, build::BuildResult result, std::string output, std::string error,
              bool overflow) {
        LockGuard lock(mutex);
        if (target != nullptr) {
            auto* self = target;
            QMetaObject::invokeMethod(
                self,
                [self, generation, phase, result = std::move(result), output = std::move(output),
                 error = std::move(error), overflow]() mutable {
                    if (self->m_active && self->m_generation == generation)
                        self->finished(phase, std::move(result), std::move(output), std::move(error), overflow);
                },
                Qt::QueuedConnection);
        }
    }
    void postOutput(std::uint64_t generation, build::OutputStream stream, std::string line) {
        LockGuard lock(mutex);
        if (target == nullptr)
            return;
        bool truncation = false;
        if (queuedLines >= kMaxQueuedLines || line.size() > kMaxQueuedOutput ||
            queuedBytes > kMaxQueuedOutput - line.size()) {
            if (truncationQueued)
                return;
            line = "Build output was truncated while the UI was busy.";
            truncation = true;
            truncationQueued = true;
        }
        const auto bytes = line.size();
        queuedBytes += bytes;
        ++queuedLines;
        auto* self = target;
        const auto relay = shared_from_this();
        const bool posted = QMetaObject::invokeMethod(
            self,
            [self, relay, generation, stream, line = std::move(line), bytes, truncation] {
                {
                    LockGuard outputLock(relay->mutex);
                    relay->queuedBytes -= bytes;
                    --relay->queuedLines;
                    if (truncation)
                        relay->truncationQueued = false;
                }
                if (self->m_active && self->m_generation == generation && self->m_output)
                    self->m_output(stream, line);
            },
            Qt::QueuedConnection);
        if (!posted) {
            queuedBytes -= bytes;
            --queuedLines;
            if (truncation)
                truncationQueued = false;
        }
    }
};

ManagedDebugPreparation::ManagedDebugPreparation(io::IProcessLauncher& launcher, project::TrustStore& trust,
                                                 IClock& clock, QObject* parent)
    : QObject(parent), m_launcher(launcher), m_trust(trust), m_clock(clock), m_relay(std::make_shared<Relay>()) {
    LockGuard lock(m_relay->mutex);
    m_relay->target = this;
}

ManagedDebugPreparation::~ManagedDebugPreparation() {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    m_session.reset();
}

bool ManagedDebugPreparation::current() const {
    if (!m_stillCurrent || !m_stillCurrent() || m_roots.empty())
        return false;
    for (const auto& root : m_roots) {
        if (!io::isAbsolutePath(root) || !m_trust.isTrusted(root))
            return false;
    }
    return m_trust.isTrusted(io::parentPath(m_request.project));
}

Result<void> ManagedDebugPreparation::start(build::ManagedProjectRequest request, std::vector<std::string> trustRoots,
                                            std::string guardAssembly, StillCurrent stillCurrent, Completion done,
                                            Output output) {
    if (m_active)
        return fail(ErrorCode::Busy, "a managed debug preparation is already active");
    if (!done || !stillCurrent || !io::isAbsolutePath(guardAssembly) || guardAssembly.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "managed debug preparation needs a guard and completion hooks");
    auto valid = build::managedProjectBuildSpec(request);
    if (!valid)
        return std::unexpected(std::move(valid.error()));
    m_request = std::move(request);
    m_roots = std::move(trustRoots);
    m_guard = std::move(guardAssembly);
    m_stillCurrent = std::move(stillCurrent);
    m_done = std::move(done);
    m_output = std::move(output);
    if (!current()) {
        m_done = {};
        m_output = {};
        return fail(ErrorCode::Untrusted, "managed workspace is no longer trusted or current");
    }
    m_active = true;
    m_cancelled = false;
    ++m_generation;
    auto started = launch(Phase::Build);
    if (!started) {
        m_active = false;
        m_done = {};
        m_output = {};
        return std::unexpected(std::move(started.error()));
    }
    return {};
}

void ManagedDebugPreparation::cancel() {
    if (!m_active)
        return;
    m_cancelled = true;
    if (m_session)
        m_session->cancel();
}

Result<void> ManagedDebugPreparation::launch(Phase phase) {
    if (!current())
        return fail(ErrorCode::Untrusted, "managed workspace is no longer trusted or current");
    auto capture = std::make_shared<Capture>();
    const auto relay = m_relay;
    const auto generation = m_generation;
    build::BuildCallbacks callbacks;
    callbacks.onLine = [capture, relay, generation, phase](build::OutputStream stream, std::string_view line) {
        if (phase == Phase::Build)
            relay->postOutput(generation, stream, std::string(line));
        if (stream == build::OutputStream::Stderr) {
            if (capture->error.size() < 16383) {
                capture->error.append(line.substr(0, 16383 - capture->error.size()));
                capture->error.push_back('\n');
            }
            return;
        }
        if (phase == Phase::Build)
            return;
        if (line.size() >= kMaxOutput || capture->output.size() > kMaxOutput - line.size() - 1) {
            capture->overflow = true;
            return;
        }
        capture->output.append(line);
        capture->output.push_back('\n');
    };
    callbacks.onFinished = [relay, capture, generation, phase](const build::BuildResult& result) {
        relay->post(generation, phase, result, capture->output, capture->error, capture->overflow);
    };
    Result<std::unique_ptr<build::BuildSession>> started = fail(ErrorCode::Unknown, "invalid preparation phase");
    if (phase == Phase::Build) {
        started =
            build::startManagedProjectBuild(m_launcher, m_trust, m_request, m_roots, std::move(callbacks), m_clock);
    } else if (phase == Phase::Query) {
        started =
            build::startManagedProjectQuery(m_launcher, m_trust, m_request, m_roots, std::move(callbacks), m_clock);
    } else {
        if (!m_trust.isTrusted(io::parentPath(m_assembly)))
            return fail(ErrorCode::Untrusted, "managed assembly directory is not trusted");
        io::ProcessSpec spec;
        spec.executable = m_request.dotnet;
        std::string pdb = m_assembly.substr(0, m_assembly.size() - 4) + ".pdb";
        spec.arguments = {m_guard, m_assembly, std::move(pdb), "--manifest"};
        spec.workingDirectory = std::string(io::parentPath(m_request.project));
        spec.stdinMode = io::Stdio::Null;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        started = build::BuildSession::start(m_launcher, spec, true, std::move(callbacks), m_clock);
    }
    if (!started)
        return std::unexpected(std::move(started.error()));
    m_session = std::move(*started);
    return {};
}

void ManagedDebugPreparation::finished(Phase phase, build::BuildResult result, std::string output, std::string error,
                                       bool overflow) {
    m_session.reset();
    if (m_cancelled || result.cancelled) {
        complete(fail(ErrorCode::Cancelled, "managed debug preparation was cancelled"));
        return;
    }
    if (!current()) {
        complete(fail(ErrorCode::Untrusted, "managed workspace changed or trust was revoked"));
        return;
    }
    if (!result.success || overflow) {
        complete(fail(overflow ? ErrorCode::OutOfRange : ErrorCode::IoError,
                      overflow ? "managed preparation output was too large"
                               : std::format("managed preparation failed with exit code {}{}{}", result.exitCode,
                                             error.empty() ? "" : ": ", error)));
        return;
    }
    if (phase == Phase::Query) {
        auto properties = build::parseManagedProjectProperties(output);
        if (!properties) {
            complete(std::unexpected(std::move(properties.error())));
            return;
        }
        auto target = build::managedConsoleTarget(m_request, *properties);
        if (!target) {
            complete(std::unexpected(std::move(target.error())));
            return;
        }
        m_assembly = std::move(*target);
    } else if (phase == Phase::SourceCheck) {
        auto json = rpc::parseJson(output);
        const auto* sources = json ? json->find("sources") : nullptr;
        if (!json || !json->isObject() || sources == nullptr || !sources->isArray() || sources->size() == 0 ||
            !moduleGuid(json->stringAt("moduleId")) || !hexDigest(json->stringAt("assemblySha256"), 64) ||
            !hexDigest(json->stringAt("pdbSha256"), 64)) {
            complete(fail(ErrorCode::Corrupt, "source verifier returned an invalid document list"));
            return;
        }
        PreparedManagedDebug ready;
        ready.moduleId = json->stringAt("moduleId");
        ready.assemblySha256 = json->stringAt("assemblySha256");
        ready.pdbSha256 = json->stringAt("pdbSha256");
        ready.target.dotnet = m_request.dotnet;
        ready.target.assembly = m_assembly;
        ready.target.workingDirectory = std::string(io::parentPath(m_request.project));
        for (const auto& value : sources->items()) {
            const auto path = value.stringAt("path");
            const auto hash = value.stringAt("sha256");
            if (!value.isObject() || !io::isAbsolutePath(path) || !m_trust.isTrusted(io::parentPath(path)) ||
                !hexDigest(hash, 64)) {
                complete(fail(ErrorCode::Untrusted, "verified source lies outside trusted roots"));
                return;
            }
            ready.sources.push_back({std::string(path), std::string(hash)});
        }
        complete(std::move(ready));
        return;
    }
    const Phase next = phase == Phase::Build ? Phase::Query : Phase::SourceCheck;
    if (auto started = launch(next); !started)
        complete(std::unexpected(std::move(started.error())));
}

void ManagedDebugPreparation::complete(Result<PreparedManagedDebug> result) {
    auto done = std::move(m_done);
    m_output = {};
    m_active = false;
    m_session.reset();
    ++m_generation;
    if (done)
        done(std::move(result));
}

} // namespace frost::app
