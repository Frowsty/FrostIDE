// SPDX-License-Identifier: Apache-2.0
// Historical crash correlation is offered honestly; only immutable private copies reach DbgEng.
#include "engine_dump_controller.hpp"

#include "frost/core/log.hpp"
#include "frost/debug/dbgeng_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <QPointer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace frost::app {
namespace {
Result<void> cleanPrivateDirectory(std::string_view path) {
    auto& fs = io::realFileSystem();
    if (auto checked = engine::validateCrashCacheDirectory(fs, path); !checked)
        return checked;
    auto entries = fs.listDirectory(path);
    if (!entries)
        return std::unexpected(std::move(entries.error()));
    if (entries->size() > 16)
        return fail(ErrorCode::OutOfRange, "Private crash cache cleanup exceeds its entry limit");
    for (const auto& entry : *entries) {
        auto child = io::joinPath(path, entry.name);
        if (!child)
            return std::unexpected(std::move(child.error()));
        // There are no nested directories in materialize(). Never recurse through an unexpected
        // entry; removeFile removes symlinks/reparse points themselves, without following them.
        auto removed = entry.type == io::FileType::Directory ? fs.removeDirectory(*child) : fs.removeFile(*child);
        if (!removed)
            return removed;
    }
    return fs.removeDirectory(path);
}
struct CacheLease {
    CacheLease(const QString& pattern, std::function<void(std::string)> cleanup_)
        : directory(pattern), cleanup(std::move(cleanup_)) {
        directory.setAutoRemove(false);
    }
    ~CacheLease() { cleanup(directory.path().toStdString()); }
    QTemporaryDir directory;
    std::function<void(std::string)> cleanup;
};
struct DumpCopy {
    std::shared_ptr<const engine::ValidatedCrashArtifact> artifact;
    std::shared_ptr<CacheLease> lease;
    std::optional<Result<std::string>> result;
    std::atomic<bool> done{false};
};

// Keep the file through actual client destruction, including the existing non-GUI retirement
// batch. Closing the UI controller or removing its offer does not race native dump file reads.
class LeasedDumpClient final : public debug::IDebugAdapterClient {
public:
    LeasedDumpClient(std::unique_ptr<debug::IDebugAdapterClient> client, std::shared_ptr<CacheLease> lease)
        : m_client(std::move(client)), m_lease(std::move(lease)) {}
    ~LeasedDumpClient() override {
        m_client.reset();
        m_lease.reset();
    }
    std::int64_t request(std::string command, rpc::Json arguments, Completion done) override {
        return m_client->request(std::move(command), std::move(arguments), std::move(done));
    }
    void cancel(std::int64_t seq) override { m_client->cancel(seq); }
    void setSupportsCancel(bool supported) noexcept override { m_client->setSupportsCancel(supported); }
    void close() noexcept override { m_client->close(); }
    bool closed() const noexcept override { return m_client->closed(); }
    std::size_t pendingRequests() const override { return m_client->pendingRequests(); }

private:
    std::unique_ptr<debug::IDebugAdapterClient> m_client;
    std::shared_ptr<CacheLease> m_lease;
};
} // namespace

struct EngineDumpWorkBatch::State {
    QThreadPool pool;
    mutable std::mutex mutex;
    std::atomic<std::size_t> pending{0};
    std::condition_variable idle;
    std::function<void(std::string)> cleanup;
    std::size_t leases = 0;
    bool materializing = false;
    bool draining = false;
};
EngineDumpWorkBatch::EngineDumpWorkBatch(std::function<void(std::string)> cleanup)
    : m_state(std::make_shared<State>()) {
    m_state->pool.setMaxThreadCount(1);
    m_state->cleanup = cleanup ? std::move(cleanup) : [](std::string path) {
        auto removed = cleanPrivateDirectory(path);
        if (!removed)
            log::warn("debug", "Private crash cache cleanup failed: {}", removed.error().message());
    };
}
EngineDumpWorkBatch::~EngineDumpWorkBatch() { drain(); }
Result<void> EngineDumpWorkBatch::submit(std::function<void()> work) {
    const auto state = m_state;
    std::scoped_lock lock(state->mutex);
    if (state->draining)
        return fail(ErrorCode::Cancelled, "Crash dump background work is shutting down");
    if (state->materializing)
        return fail(ErrorCode::Busy, "A crash dump is already being copied");
    state->materializing = true;
    ++state->pending;
    state->pool.start([state, work = std::move(work)] {
        work();
        {
            std::scoped_lock finished(state->mutex);
            state->materializing = false;
            --state->pending;
        }
        state->idle.notify_all();
    });
    return {};
}
Result<std::function<void(std::string)>> EngineDumpWorkBatch::reserveCleanup() {
    const auto state = m_state;
    std::scoped_lock lock(state->mutex);
    if (state->draining || state->leases >= 144)
        return fail(ErrorCode::Busy, "Private crash cache leases reached their bounded capacity");
    ++state->leases;
    return std::function<void(std::string)>([state](std::string path) {
        std::scoped_lock enqueue(state->mutex);
        // Every reserved lease owns a cleanup slot, even while materialization or final drain is
        // active. There is no Busy branch that abandons a directory or removes it on the GUI.
        ++state->pending;
        state->pool.start([state, path = std::move(path)] {
            if (!path.empty())
                state->cleanup(path);
            {
                std::scoped_lock finished(state->mutex);
                --state->leases;
                --state->pending;
            }
            state->idle.notify_all();
        });
    });
}
void EngineDumpWorkBatch::drain() {
    {
        std::unique_lock lock(m_state->mutex);
        m_state->draining = true;
        m_state->idle.wait(lock, [this] { return m_state->pending.load() == 0 && m_state->leases == 0; });
    }
    m_state->pool.waitForDone();
}
std::size_t EngineDumpWorkBatch::pending() const noexcept { return m_state->pending.load(); }

struct EngineDumpController::Impl {
    struct Offer {
        std::shared_ptr<const engine::ValidatedCrashArtifact> artifact;
        EngineDumpAdmission admission;
        std::shared_ptr<DumpCopy> copy;
    };
    static constexpr std::size_t MaxBytes = 256 * 1024 * 1024;
    EngineDumpController& owner;
    EngineDumpControllerOptions options;
    QTimer timer;
    std::map<std::uint64_t, Offer> offers;
    std::size_t bytes = 0;
    std::uint64_t next = 1;
    std::uint64_t revision = 0;
    std::shared_ptr<std::atomic<bool>> launchAdmitted = std::make_shared<std::atomic<bool>>(true);
    bool stopped = false;

    Impl(EngineDumpController& owner_, EngineDumpControllerOptions options_)
        : owner(owner_), options(std::move(options_)), timer(&owner_) {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, &owner, [this] { poll(); });
    }
    void error(const Error& why) {
        if (options.notifications)
            options.notifications->notify({QStringLiteral("Could not open crash dump"),
                                           QString::fromStdString(why.message()),
                                           ui::NotificationKind::Error,
                                           std::nullopt,
                                           {}});
    }
    Result<void> admitted(const Offer& value) const {
        if (stopped || !value.admission.current || !value.admission.validatePhysical)
            return fail(ErrorCode::Cancelled, "The crash dump offer is no longer current");
        if (value.admission.projectRoot.empty() ||
            lang::documentKey(value.admission.projectRoot) != lang::documentKey(value.artifact->source().projectPath))
            return fail(ErrorCode::PermissionDenied, "The crash dump belongs to a different project context");
        return value.admission.current();
    }
    Result<void> startSession(const Offer& value, const std::string& path) {
        const auto admittedRevision = revision;
        if (auto valid = admitted(value); !valid)
            return valid;
        if (!options.sessions || !options.launcher)
            return fail(ErrorCode::Unsupported, "The read-only dump debugger is unavailable");
        debug::DbgEngTarget target;
        target.native.name = std::string(io::fileName(path));
        target.native.trustRoots = {value.admission.projectRoot};
        target.dumpFile = path;
        target.localSymbolPaths = value.admission.localSymbolPaths;
        auto arguments = debug::dbgEngDapLaunchArguments(target);
        if (!arguments)
            return std::unexpected(std::move(arguments.error()));
        auto base = options.adapterFactory
                        ? options.adapterFactory
                        : debug::dbgEngDapAdapterFactory(*options.launcher, options.adapterExecutable);
        const auto lease = value.copy->lease;
        debug::AdapterFactory factory =
            [base = std::move(base),
             lease](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
            auto client = base(std::move(handlers));
            if (!client)
                return std::unexpected(std::move(client.error()));
            return std::unique_ptr<debug::IDebugAdapterClient>(
                std::make_unique<LeasedDumpClient>(std::move(*client), lease));
        };
        auto id = options.sessions->createSession(QString::fromStdString(target.native.name));
        if (!id)
            return std::unexpected(std::move(id.error()));
        if (revision != admittedRevision) {
            (void)options.sessions->remove(*id);
            return fail(ErrorCode::Cancelled, "The crash dump offer was revoked while creating its session");
        }
        if (auto valid = admitted(value); !valid) {
            (void)options.sessions->remove(*id);
            return valid;
        }
        auto* controller = options.sessions->controller(*id);
        if (!controller) {
            (void)options.sessions->remove(*id);
            return fail(ErrorCode::Internal, "The dump debugger session could not be created");
        }
        auto started = controller->startAdapterTarget(
            target.native, std::move(factory), std::move(*arguments), std::string(debug::kDbgEngAdapterId),
            debug::DebugTargetMode::Launch, true,
            [token = launchAdmitted, current = value.admission.current]() -> Result<void> {
                if (!token->load())
                    return fail(ErrorCode::Cancelled, "Crash dump launch admission was revoked");
                return current();
            });
        if (!started) {
            (void)options.sessions->remove(*id);
            return started;
        }
        return options.sessions->select(*id);
    }
    void poll() {
        bool pending = false;
        for (auto it = offers.begin(); it != offers.end();) {
            auto& offer = it->second;
            if (!offer.copy) {
                ++it;
                continue;
            }
            if (!offer.copy->done.load(std::memory_order_acquire)) {
                pending = true;
                ++it;
                continue;
            }
            // Registry signals may synchronously revoke offers. Remove this entry before calling
            // external services, retaining its immutable bytes and cache lease locally.
            auto completed = std::move(offer);
            bytes -= completed.artifact->dumpBytes().size();
            it = offers.erase(it);
            Result<void> result;
            if (!completed.copy->result || !*completed.copy->result)
                result = completed.copy->result
                             ? Result<void>(std::unexpected(completed.copy->result->error()))
                             : Result<void>(fail(ErrorCode::Internal, "Crash dump copy completed without a result"));
            else
                result = startSession(completed, **completed.copy->result);
            if (!result)
                error(result.error());
            // Both startSession and error publish GUI signals; reacquire an iterator afterwards.
            it = offers.begin();
        }
        if (!pending)
            timer.stop();
    }
};

EngineDumpController::EngineDumpController(EngineDumpControllerOptions options, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, std::move(options))) {}
EngineDumpController::~EngineDumpController() { shutdown(); }

Result<std::uint64_t> EngineDumpController::offer(std::shared_ptr<const engine::ValidatedCrashArtifact> artifact,
                                                  EngineDumpAdmission admission) {
    auto& impl = *m_impl;
    if (!artifact || !impl.options.notifications || !impl.options.work)
        return fail(ErrorCode::InvalidArgument,
                    "A validated dump and configured notification/work services are required");
    Impl::Offer value{std::move(artifact), std::move(admission), {}};
    if (auto valid = impl.admitted(value); !valid)
        return std::unexpected(std::move(valid.error()));
    for (const auto& [ticket, previous] : impl.offers)
        if (previous.artifact->sha256() == value.artifact->sha256() &&
            previous.artifact->source() == value.artifact->source())
            return ticket;
    const auto size = value.artifact->dumpBytes().size();
    if (impl.offers.size() >= 8 || size > Impl::MaxBytes - impl.bytes)
        return fail(ErrorCode::OutOfRange, "Pending crash dump offers exceed the bounded cache budget");
    if (impl.next == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::OutOfRange, "Crash dump notification identities are exhausted");
    const auto ticket = impl.next++;
    impl.bytes += size;
    impl.offers.emplace(ticket, std::move(value));
    QPointer<EngineDumpController> self(this);
    ui::Notification notice;
    notice.title = QStringLiteral("FrostEngine crash dump");
    notice.text = QStringLiteral("A local crash dump was correlated with a previously authenticated FrostEngine "
                                 "process. Open it for read-only inspection.");
    notice.kind = ui::NotificationKind::Warning;
    notice.actions.push_back({QStringLiteral("Open dump"), [self, ticket] {
                                  if (self) {
                                      auto result = self->open(ticket);
                                      if (!result)
                                          self->m_impl->error(result.error());
                                  }
                              }});
    impl.options.notifications->notify(std::move(notice));
    return ticket;
}

Result<void> EngineDumpController::open(std::uint64_t ticket) {
    auto& impl = *m_impl;
    const auto found = impl.offers.find(ticket);
    if (found == impl.offers.end())
        return fail(ErrorCode::Cancelled, "The crash dump offer expired");
    auto& value = found->second;
    if (auto valid = impl.admitted(value); !valid)
        return valid;
    if (value.copy)
        return fail(ErrorCode::Busy, "This crash dump is already opening");
    if (!io::isAbsolutePath(impl.options.cacheDirectory))
        return fail(ErrorCode::InvalidArgument, "The IDE crash cache directory is unavailable");
    auto copy = std::make_shared<DumpCopy>();
    copy->artifact = value.artifact;
    const auto directory = impl.options.cacheDirectory;
    const auto adapterPath = impl.options.adapterExecutable;
    const bool checkAdapter = !impl.options.adapterFactory;
    const auto validatePhysical = value.admission.validatePhysical;
    const auto projectRoot = value.admission.projectRoot;
    const auto work = impl.options.work;
    auto submitted = work->submit([copy, directory, adapterPath, checkAdapter, validatePhysical, projectRoot, work] {
        const auto materialize = [&]() -> Result<std::string> {
            if (auto current = validatePhysical(); !current)
                return std::unexpected(std::move(current.error()));
            auto& fs = io::realFileSystem();
            if (checkAdapter) {
                auto adapter = fs.status(adapterPath);
                if (!adapter || adapter->type != io::FileType::Regular)
                    return fail(ErrorCode::NotFound, "The staged read-only DbgEng adapter is unavailable");
            }
            auto base = engine::validateCrashCacheDirectory(fs, directory);
            if (!base)
                return std::unexpected(std::move(base.error()));
            const auto cacheKey = lang::documentKey(directory);
            auto projectKey = lang::documentKey(projectRoot);
            if (cacheKey == projectKey)
                return fail(ErrorCode::PermissionDenied, "A crash dump cache cannot be a workspace");
            if (!projectKey.ends_with('/') && !projectKey.ends_with('\\'))
                projectKey += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
            if (cacheKey.starts_with(projectKey))
                return fail(ErrorCode::PermissionDenied, "A crash dump cache cannot be inside a workspace");
            auto cleanup = work->reserveCleanup();
            if (!cleanup)
                return std::unexpected(std::move(cleanup.error()));
            copy->lease = std::make_shared<CacheLease>(QString::fromStdString(directory + "/engine-dump-XXXXXX"),
                                                       std::move(*cleanup));
            if (!copy->lease->directory.isValid())
                return fail(ErrorCode::IoError, "A private IDE crash cache directory could not be created");
            auto materialized = copy->artifact->materialize(fs, copy->lease->directory.path().toStdString());
            if (!materialized)
                return materialized;
            if (auto current = validatePhysical(); !current)
                return std::unexpected(std::move(current.error()));
            return materialized;
        };
        copy->result.emplace(materialize());
        copy->done.store(true, std::memory_order_release);
    });
    if (!submitted)
        return submitted;
    value.copy = std::move(copy);
    impl.timer.start();
    return {};
}
void EngineDumpController::revoke() {
    m_impl->launchAdmitted->store(false);
    m_impl->launchAdmitted = std::make_shared<std::atomic<bool>>(true);
    ++m_impl->revision;
    m_impl->timer.stop();
    m_impl->offers.clear();
    m_impl->bytes = 0;
}
void EngineDumpController::shutdown() {
    m_impl->stopped = true;
    revoke();
}

} // namespace frost::app
