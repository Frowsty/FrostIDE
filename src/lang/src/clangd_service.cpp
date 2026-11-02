// SPDX-License-Identifier: Apache-2.0
//
// ClangdService (clangd.hpp): the supervisor coroutine, the stderr pump, document
// synchronisation across restarts and the mapping of published diagnostics to snapshots.

#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"

#include "timeout.hpp"

#include <deque>
#include <format>
#include <map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::lang {
namespace {

constexpr std::string_view kLogCategory = "clangd";
constexpr std::size_t kStderrLines = 64;

[[nodiscard]] std::int64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

// clangd's log lines start with a level letter and a timestamp: "E[12:00:00.000] ...".
[[nodiscard]] log::Level levelOf(std::string_view line) noexcept {
    if (line.size() >= 2 && line[1] == '[') {
        switch (line[0]) {
        case 'E': return log::Level::Error;
        case 'I': return log::Level::Info;
        case 'V':
        case 'D': return log::Level::Debug;
        default: break;
        }
    }
    return log::Level::Warn; // Crash dumps and other unprefixed output.
}

struct OpenDocument {
    std::string path;
    std::string uri;
    std::string languageId;
    std::int64_t version = 1;
    text::Snapshot latest;
    // (LSP version, snapshot) for the recent versions, oldest first; the back is `latest`.
    std::deque<std::pair<std::int64_t, text::Snapshot>> history;
};

} // namespace

struct ClangdService::Shared : std::enable_shared_from_this<ClangdService::Shared> {
    Shared(io::IProcessLauncher& l, io::AsyncContext c, ClangdServiceOptions o)
        : launcher(l), context(c), options(std::move(o)) {}

    io::IProcessLauncher& launcher;
    io::AsyncContext context;
    const ClangdServiceOptions options;
    DiagnosticsStore diagnostics;
    std::stop_source stop;

    mutable Mutex mutex;
    bool started FROST_GUARDED_BY(mutex) = false;
    ClangdState state FROST_GUARDED_BY(mutex) = ClangdState::Stopped;
    unsigned restarts FROST_GUARDED_BY(mutex) = 0;
    std::uint64_t round FROST_GUARDED_BY(mutex) = 0; // Increments per launch; older clients' callbacks are ignored.
    std::shared_ptr<io::IAsyncProcess> process FROST_GUARDED_BY(mutex);
    std::shared_ptr<LspClient> client FROST_GUARDED_BY(mutex);
    std::optional<ServerCapabilities> capabilities FROST_GUARDED_BY(mutex);
    std::optional<CompileCommandsLocation> compileCommands FROST_GUARDED_BY(mutex);
    std::optional<Error> lastError FROST_GUARDED_BY(mutex);
    std::deque<std::string> stderrLines FROST_GUARDED_BY(mutex);
    std::map<std::string, OpenDocument> documents FROST_GUARDED_BY(mutex); // documentKey -> document.

    void setState(ClangdState next) {
        LockGuard lock(mutex);
        state = next;
    }

    void recordError(Error error, ClangdState next) {
        log::warn(kLogCategory, "{}", error.describe());
        LockGuard lock(mutex);
        lastError = std::move(error);
        state = next;
    }

    // The client of round `forRound` if it is still the current one and Running.
    [[nodiscard]] std::shared_ptr<LspClient> runningClient() const FROST_REQUIRES(mutex) {
        return state == ClangdState::Running ? client : nullptr;
    }

    void pushHistory(OpenDocument& document) const {
        document.history.emplace_back(document.version, document.latest);
        while (document.history.size() > std::max<std::size_t>(options.historyPerDocument, 1)) {
            document.history.pop_front();
        }
    }

    void stderrLine(std::string_view line) {
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            return;
        }
        log::write(levelOf(line), kLogCategory, "{}", line);
        LockGuard lock(mutex);
        stderrLines.emplace_back(line);
        while (stderrLines.size() > kStderrLines) {
            stderrLines.pop_front();
        }
    }

    void onDiagnostics(std::uint64_t fromRound, const Json& params) {
        auto decoded = decodePublishDiagnostics(params);
        if (!decoded) {
            log::warn(kLogCategory, "dropped a publishDiagnostics: {}", decoded.error().describe());
            return;
        }
        {
            LockGuard lock(mutex);
            if (fromRound != round) {
                return; // A previous clangd's late message.
            }
            const PositionEncoding encoding = capabilities ? capabilities->positionEncoding : PositionEncoding::Utf16;
            auto it = documents.find(documentKey(decoded->file));
            if (it != documents.end()) {
                const OpenDocument& document = it->second;
                const text::Snapshot* seen = &document.latest;
                if (decoded->lspVersion) {
                    for (const auto& [version, snapshot] : document.history) {
                        if (version == *decoded->lspVersion) {
                            seen = &snapshot;
                        }
                    }
                    decoded->stale = *decoded->lspVersion < document.version;
                }
                mapToSnapshot(*decoded, *seen, encoding);
            }
        }
        diagnostics.publish(std::move(*decoded));
    }

    // Re-opens every document on a freshly initialized client. Caller holds the lock.
    void replayDocuments(LspClient& fresh) FROST_REQUIRES(mutex) {
        for (auto& [key, document] : documents) {
            document.history.clear();
            pushHistory(document);
            if (auto sent = fresh.didOpen(document.uri, document.languageId, document.version, document.latest.text());
                !sent) {
                log::warn(kLogCategory, "didOpen {} failed: {}", document.path, sent.error().describe());
            }
        }
    }

    [[nodiscard]] InitializeOptions initializeOptions() const {
        InitializeOptions init;
        init.rootPath = options.workspaceRoot;
        init.processId = currentProcessId();
        init.initializationOptions = options.initializationOptions;
        return init;
    }

    [[nodiscard]] LspClient::Handlers handlers(std::uint64_t forRound) {
        std::weak_ptr<Shared> weak = weak_from_this();
        LspClient::Handlers out;
        out.onDiagnostics = [weak, forRound](const Json& params) {
            if (auto self = weak.lock()) {
                self->onDiagnostics(forRound, params);
            }
        };
        out.onMessage = [](int type, std::string_view message) {
            const log::Level level = type == 1 ? log::Level::Error : type == 2 ? log::Level::Warn : log::Level::Debug;
            log::write(level, kLogCategory, "{}", message);
        };
        out.onProtocolError = [](const Error& problem) { log::warn(kLogCategory, "protocol: {}", problem.describe()); };
        return out;
    }

    static sched::Task<void> pumpStderr(std::shared_ptr<Shared> self, std::shared_ptr<io::IAsyncProcess> process) {
        std::vector<char> buffer(16 * 1024);
        std::string partial;
        for (;;) {
            auto got = co_await process->readStderr(std::span<char>(buffer));
            if (!got || *got == 0) {
                break;
            }
            partial.append(buffer.data(), *got);
            std::size_t newline = 0;
            while ((newline = partial.find('\n')) != std::string::npos) {
                self->stderrLine(std::string_view(partial).substr(0, newline));
                partial.erase(0, newline + 1);
            }
            if (partial.size() > 64 * 1024) { // A runaway line: log it in pieces.
                self->stderrLine(partial);
                partial.clear();
            }
        }
        self->stderrLine(partial);
    }

    [[nodiscard]] io::ProcessSpec processSpec(const std::optional<CompileCommandsLocation>& location) const {
        io::ProcessSpec spec;
        spec.executable = options.clangdPath;
        spec.arguments = clangdArguments(options.launch, location);
        spec.workingDirectory = options.workspaceRoot;
        spec.stdinMode = io::Stdio::Pipe;
        spec.stdoutMode = io::Stdio::Pipe;
        spec.stderrMode = io::Stdio::Pipe;
        spec.killOnClose = true;
        return spec;
    }

    static sched::Task<void> supervise(std::shared_ptr<Shared> self) {
        sched::IScheduler& scheduler = self->context.scheduler;
        const std::stop_token stop = self->stop.get_token();
        unsigned consecutive = 0;
        while (!stop.stop_requested()) {
            self->setState(ClangdState::Starting);
            std::optional<CompileCommandsLocation> location;
            if (self->options.compileCommands) {
                auto found = self->options.compileCommands->locate();
                if (found) {
                    location = *found;
                } else {
                    log::warn(kLogCategory, "no compile commands: {}", found.error().describe());
                }
            }
            if (location) {
                log::info(kLogCategory, "compile commands ({}): {}", toString(location->source), location->file);
            }
            auto launched = self->launcher.launchAsync(self->processSpec(location), self->context);
            if (!launched) {
                self->recordError(std::move(launched.error().withContext("starting clangd")), ClangdState::Failed);
                co_return;
            }
            std::shared_ptr<io::IAsyncProcess> process(std::move(*launched));
            std::uint64_t thisRound = 0;
            {
                LockGuard lock(self->mutex);
                thisRound = ++self->round;
            }
            auto client = std::make_shared<LspClient>(processByteStream(process), scheduler, self->initializeOptions(),
                                                      self->handlers(thisRound));
            {
                LockGuard lock(self->mutex);
                self->process = process;
                self->client = client;
                self->compileCommands = location;
                self->capabilities.reset();
            }
            scheduler.spawn(sched::Lane::Background, pumpStderr(self, process)).detach();
            log::info(kLogCategory, "started {} (pid {})", self->options.clangdPath, process->pid());
            const IClock::SteadyTime startedAt = scheduler.clock().steadyNow();

            std::optional<Error> roundError;
            {
                detail::Deadline deadline(scheduler, self->options.initializeTimeout, stop);
                auto initialized = co_await client->initialize(deadline.token());
                if (!initialized && deadline.expired()) {
                    roundError = Error(ErrorCode::Timeout, "clangd did not answer initialize in time");
                } else if (!initialized) {
                    roundError = std::move(initialized.error());
                } else {
                    LockGuard lock(self->mutex);
                    self->capabilities = *initialized;
                    self->state = ClangdState::Running;
                    self->replayDocuments(*client);
                }
            }
            if (stop.stop_requested()) {
                co_return; // stop() shuts the round down.
            }
            if (!roundError) {
                auto exited = co_await process->exited(stop);
                if (!exited) {
                    co_return; // Stopped (or the reactor shut down): stop() takes over.
                }
                roundError = Error(ErrorCode::IoError,
                                   std::format("clangd exited unexpectedly (exit code {})", exited->exitCode));
            } else {
                static_cast<void>(process->terminate());
                detail::Deadline reap(scheduler, std::chrono::seconds(5), stop);
                static_cast<void>(co_await process->exited(reap.token()));
                if (stop.stop_requested()) {
                    co_return;
                }
            }
            client->close();
            {
                LockGuard lock(self->mutex);
                ++self->restarts;
                self->client.reset();
                self->process.reset();
                self->capabilities.reset();
            }
            if (scheduler.clock().steadyNow() - startedAt >= self->options.restart.stableAfter) {
                consecutive = 0;
            }
            ++consecutive;
            if (consecutive > self->options.restart.maxConsecutiveCrashes) {
                self->recordError(std::move(roundError->withContext("clangd crashed too often; giving up")),
                                  ClangdState::Failed);
                co_return;
            }
            const IClock::Duration delay = restartDelay(self->options.restart, consecutive);
            self->recordError(
                std::move(roundError->withContext(std::format(
                    "restarting in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()))),
                ClangdState::Restarting);
            if (!co_await scheduler.sleepFor(delay, stop)) {
                co_return;
            }
        }
    }
};

ClangdService::ClangdService(io::IProcessLauncher& launcher, io::AsyncContext context, ClangdServiceOptions options)
    : m_shared(std::make_shared<Shared>(launcher, context, std::move(options))) {}

ClangdService::~ClangdService() {
    m_shared->stop.request_stop();
    std::shared_ptr<LspClient> client;
    std::shared_ptr<io::IAsyncProcess> process;
    {
        LockGuard lock(m_shared->mutex);
        client = std::move(m_shared->client);
        process = std::move(m_shared->process);
        m_shared->state = ClangdState::Stopped;
    }
    if (client) {
        client->close();
    }
    if (process) {
        static_cast<void>(process->terminate());
    }
    m_supervisor.detach();
}

Result<void> ClangdService::start() {
    const ClangdServiceOptions& options = m_shared->options;
    if (!io::isAbsolutePath(options.clangdPath) || !io::isAbsolutePath(options.workspaceRoot)) {
        return fail(ErrorCode::InvalidArgument, "clangd and the workspace root need absolute paths");
    }
    {
        LockGuard lock(m_shared->mutex);
        if (m_shared->started) {
            return fail(ErrorCode::Busy, "clangd is already started");
        }
        m_shared->started = true;
        m_shared->state = ClangdState::Starting;
    }
    m_supervisor = m_shared->context.scheduler.spawn(sched::Lane::Background, Shared::supervise(m_shared));
    return {};
}

sched::Task<void> ClangdService::stop() {
    std::shared_ptr<Shared> self = m_shared;
    self->stop.request_stop();
    if (m_supervisor.valid()) {
        sched::JoinHandle<void> supervisor = std::move(m_supervisor);
        co_await std::move(supervisor);
    }
    std::shared_ptr<LspClient> client;
    std::shared_ptr<io::IAsyncProcess> process;
    bool running = false;
    {
        LockGuard lock(self->mutex);
        client = std::move(self->client);
        process = std::move(self->process);
        running = self->state == ClangdState::Running;
        self->state = ClangdState::Stopped;
        self->capabilities.reset();
    }
    sched::IScheduler& scheduler = self->context.scheduler;
    const IClock::SteadyTime begin = scheduler.clock().steadyNow();
    if (client && running) {
        if (auto down = co_await client->shutdown(self->options.shutdownTimeout); !down) {
            log::warn(kLogCategory, "shutdown: {}", down.error().describe());
        }
    } else if (client) {
        client->close();
    }
    if (process) {
        const IClock::Duration spent = scheduler.clock().steadyNow() - begin;
        const IClock::Duration left =
            self->options.shutdownTimeout > spent ? self->options.shutdownTimeout - spent : IClock::Duration::zero();
        detail::Deadline deadline(scheduler, left);
        auto exited = co_await process->exited(deadline.token());
        if (!exited) {
            log::warn(kLogCategory, "clangd did not exit in time; killing it");
            static_cast<void>(process->terminate());
            detail::Deadline reap(scheduler, std::chrono::seconds(5));
            static_cast<void>(co_await process->exited(reap.token()));
        } else {
            log::info(kLogCategory, "clangd exited with {}", exited->exitCode);
        }
    }
}

ClangdState ClangdService::state() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->state;
}

unsigned ClangdService::restarts() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->restarts;
}

std::optional<io::ProcessId> ClangdService::serverPid() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->process ? std::optional<io::ProcessId>(m_shared->process->pid()) : std::nullopt;
}

std::optional<ServerCapabilities> ClangdService::capabilities() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->capabilities;
}

std::optional<CompileCommandsLocation> ClangdService::compileCommands() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->compileCommands;
}

std::optional<Error> ClangdService::lastError() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->lastError;
}

std::vector<std::string> ClangdService::recentStderr() const {
    LockGuard lock(m_shared->mutex);
    return {m_shared->stderrLines.begin(), m_shared->stderrLines.end()};
}

DiagnosticsStore& ClangdService::diagnostics() noexcept { return m_shared->diagnostics; }

Result<void> ClangdService::openDocument(std::string_view path, text::Snapshot snapshot) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    auto uri = pathToUri(*normalized);
    if (!uri) {
        return std::unexpected(std::move(uri.error()));
    }
    LockGuard lock(m_shared->mutex);
    const std::string key = documentKey(*normalized);
    if (m_shared->documents.contains(key)) {
        return fail(ErrorCode::AlreadyExists, std::format("{} is already open", *normalized));
    }
    OpenDocument document;
    document.languageId = std::string(languageIdForPath(*normalized));
    document.path = std::move(*normalized);
    document.uri = std::move(*uri);
    document.latest = std::move(snapshot);
    m_shared->pushHistory(document);
    if (std::shared_ptr<LspClient> client = m_shared->runningClient()) {
        if (auto sent = client->didOpen(document.uri, document.languageId, document.version, document.latest.text());
            !sent) {
            log::warn(kLogCategory, "didOpen {}: {}", document.path, sent.error().describe());
        }
    }
    m_shared->documents.emplace(key, std::move(document));
    return {};
}

Result<void> ClangdService::changeDocument(std::string_view path, const text::ChangeSet& changes,
                                           text::Snapshot after) {
    LockGuard lock(m_shared->mutex);
    auto it = m_shared->documents.find(documentKey(path));
    if (it == m_shared->documents.end()) {
        return fail(ErrorCode::NotFound, std::format("{} is not open", path));
    }
    OpenDocument& document = it->second;
    if (changes.baseVersion == changes.version && changes.changes.empty()) {
        document.latest = std::move(after);
        return {};
    }
    const std::shared_ptr<LspClient> client = m_shared->runningClient();
    const PositionEncoding encoding =
        m_shared->capabilities ? m_shared->capabilities->positionEncoding : PositionEncoding::Utf16;
    std::vector<ContentChange> contents;
    if (client) {
        if (document.latest.version() == changes.baseVersion && document.latest.sharesStorageWith(after)) {
            contents = contentChanges(changes, document.latest, after, encoding);
        } else {
            contents.push_back(ContentChange{.range = std::nullopt, .text = after.text()});
        }
    }
    ++document.version;
    document.latest = std::move(after);
    m_shared->pushHistory(document);
    if (client && !contents.empty()) {
        if (auto sent = client->didChange(document.uri, document.version, contents); !sent) {
            log::warn(kLogCategory, "didChange {}: {}", document.path, sent.error().describe());
        }
    }
    return {};
}

Result<void> ClangdService::saveDocument(std::string_view path) {
    LockGuard lock(m_shared->mutex);
    auto it = m_shared->documents.find(documentKey(path));
    if (it == m_shared->documents.end()) {
        return fail(ErrorCode::NotFound, std::format("{} is not open", path));
    }
    if (std::shared_ptr<LspClient> client = m_shared->runningClient()) {
        const std::string text = it->second.latest.text();
        if (auto sent = client->didSave(it->second.uri, text); !sent) {
            log::warn(kLogCategory, "didSave {}: {}", it->second.path, sent.error().describe());
        }
    }
    return {};
}

Result<void> ClangdService::closeDocument(std::string_view path) {
    FileDiagnostics cleared;
    {
        LockGuard lock(m_shared->mutex);
        auto it = m_shared->documents.find(documentKey(path));
        if (it == m_shared->documents.end()) {
            return fail(ErrorCode::NotFound, std::format("{} is not open", path));
        }
        if (std::shared_ptr<LspClient> client = m_shared->runningClient()) {
            if (auto sent = client->didClose(it->second.uri); !sent) {
                log::warn(kLogCategory, "didClose {}: {}", it->second.path, sent.error().describe());
            }
        }
        cleared.file = it->second.path;
        cleared.uri = it->second.uri;
        m_shared->documents.erase(it);
    }
    m_shared->diagnostics.publish(std::move(cleared));
    return {};
}

sched::Task<Result<Json>> ClangdService::request(std::string method, Json params, std::stop_token token) {
    std::shared_ptr<LspClient> client;
    {
        LockGuard lock(m_shared->mutex);
        client = m_shared->runningClient();
    }
    if (!client) {
        co_return fail(ErrorCode::Busy, method + ": clangd is not running");
    }
    co_return co_await client->request(std::move(method), std::move(params), std::move(token));
}

PositionEncoding ClangdService::encoding() const {
    LockGuard lock(m_shared->mutex);
    return m_shared->capabilities ? m_shared->capabilities->positionEncoding : PositionEncoding::Utf16;
}

Result<void> ClangdService::killServerForTesting() {
    std::shared_ptr<io::IAsyncProcess> process;
    {
        LockGuard lock(m_shared->mutex);
        process = m_shared->process;
    }
    if (!process) {
        return fail(ErrorCode::NotFound, "clangd is not running");
    }
    return process->terminate();
}

} // namespace frost::lang
