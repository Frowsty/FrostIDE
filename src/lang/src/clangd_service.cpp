// SPDX-License-Identifier: Apache-2.0
//
// ClangdService (clangd.hpp, clangd_service.hpp): the supervisor coroutine, the stderr pump,
// ResourceGovernor integration (FND-18, CPP-4), background index progress and metrics tracking,
// document synchronisation across restarts and mapping of published diagnostics to snapshots.

#include "frost/lang/clangd_service.hpp"

#include "frost/core/log.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"

#include "timeout.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <format>
#include <map>
#include <set>

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

[[nodiscard]] bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    const auto it = std::ranges::search(haystack, needle, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return !it.empty();
}

[[nodiscard]] std::size_t countShardsInDirectory(std::string_view baseDir, std::set<std::string>& visited) noexcept {
    if (baseDir.empty()) {
        return 0;
    }
    const auto cacheJoined = io::joinPath(baseDir, ".cache/clangd/index");
    if (!cacheJoined) {
        return 0;
    }
    const std::string normalized = io::normalizePath(*cacheJoined).value_or(*cacheJoined);
    const std::string key = documentKey(normalized);
    if (!visited.insert(key).second) {
        return 0;
    }
    const auto entries = io::realFileSystem().listDirectory(normalized);
    if (!entries) {
        return 0;
    }
    std::size_t count = 0;
    for (const io::DirEntry& entry : *entries) {
        if (entry.type == io::FileType::Regular && entry.name.ends_with(".idx")) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::size_t
countWorkspaceIndexShards(std::string_view workspaceRoot,
                          const std::optional<CompileCommandsLocation>& compileCommands) noexcept {
    std::set<std::string> visited;
    std::size_t total = 0;
    if (compileCommands && !compileCommands->directory.empty()) {
        total += countShardsInDirectory(compileCommands->directory, visited);
    }
    total += countShardsInDirectory(workspaceRoot, visited);
    if (const auto buildDir = io::joinPath(workspaceRoot, "build")) {
        total += countShardsInDirectory(*buildDir, visited);
    }
    return total;
}

struct OpenDocument {
    std::string path;
    std::string uri;
    std::string languageId;
    std::int64_t version = 1;
    text::Snapshot latest;
    // (LSP version, snapshot) for the recent versions, oldest first; the back is `latest`.
    std::deque<std::pair<std::int64_t, text::Snapshot>> history;
    // The LSP version of the last diagnostics publish (the inactive regions map to it).
    std::optional<std::int64_t> diagnosedVersion;
};

} // namespace

struct ClangdService::Shared : std::enable_shared_from_this<ClangdService::Shared> {
    Shared(io::IProcessLauncher& l, io::AsyncContext c, ClangdServiceOptions o)
        : launcher(l), context(c), options(std::move(o)), governorRef(std::make_shared<sched::ResourceGovernor>()) {
        indexMetricsState.governorRssCapBytes = governorRef->clangdRssCapBytes();
    }

    io::IProcessLauncher& launcher;
    io::AsyncContext context;
    const ClangdServiceOptions options;
    DiagnosticsStore diagnostics;
    std::stop_source stop;

    mutable Mutex mutex;
    std::shared_ptr<sched::ResourceGovernor> governorRef FROST_GUARDED_BY(mutex);
    bool started FROST_GUARDED_BY(mutex) = false;
    ClangdState state FROST_GUARDED_BY(mutex) = ClangdState::Stopped;
    unsigned restarts FROST_GUARDED_BY(mutex) = 0;
    std::uint64_t round FROST_GUARDED_BY(mutex) = 0; // Increments per launch; older clients' callbacks are ignored.
    std::shared_ptr<io::IAsyncProcess> process FROST_GUARDED_BY(mutex);
    std::shared_ptr<GeneratedCommandsLease> processLease FROST_GUARDED_BY(mutex);
    std::shared_ptr<LspClient> client FROST_GUARDED_BY(mutex);
    std::optional<ServerCapabilities> capabilities FROST_GUARDED_BY(mutex);
    std::optional<CompileCommandsLocation> compileCommands FROST_GUARDED_BY(mutex);
    std::optional<Error> lastError FROST_GUARDED_BY(mutex);
    std::deque<std::string> stderrLines FROST_GUARDED_BY(mutex);
    std::map<std::string, OpenDocument> documents FROST_GUARDED_BY(mutex);   // documentKey -> document.
    std::map<std::string, InactiveRegions> inactive FROST_GUARDED_BY(mutex); // documentKey -> regions.
    std::map<std::uint64_t, ClangdService::InactiveRegionsListener> inactiveListeners FROST_GUARDED_BY(mutex);
    std::uint64_t nextInactiveListener FROST_GUARDED_BY(mutex) = 1;
    ApplyEditHandler applyEditHandler FROST_GUARDED_BY(mutex);

    IndexMetrics indexMetricsState FROST_GUARDED_BY(mutex);
    std::optional<IClock::SteadyTime> indexStartedAt FROST_GUARDED_BY(mutex);
    std::string indexProgressToken FROST_GUARDED_BY(mutex);

    [[nodiscard]] std::shared_ptr<sched::ResourceGovernor> governor() const {
        LockGuard lock(mutex);
        return governorRef;
    }

    void setGovernor(std::shared_ptr<sched::ResourceGovernor> next) {
        if (!next) {
            next = std::make_shared<sched::ResourceGovernor>();
        }
        LockGuard lock(mutex);
        governorRef = std::move(next);
        indexMetricsState.governorRssCapBytes = governorRef->clangdRssCapBytes();
        indexMetricsState.peakRssSampleValid = false;
        indexMetricsState.peakRssSampleError = "governor changed; OS peak not sampled yet";
    }

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

    // Samples the running clangd process's RSS via the ResourceGovernor and updates peakRssBytes.
    void sampleServerRss() {
        std::shared_ptr<io::IAsyncProcess> sampledProcess;
        std::shared_ptr<sched::ResourceGovernor> gov;
        std::uint64_t sampledRound = 0;
        {
            LockGuard lock(mutex);
            gov = governorRef;
            sampledProcess = process;
            sampledRound = round;
        }
        const io::ProcessId pid = sampledProcess ? sampledProcess->pid() : 0;
        std::uint64_t sampledBytes = 0;
        bool sampleValid = false;
        std::string sampleError = "clangd process is not running";
        if (pid > 0) {
            auto sampled = gov->sampleProcessRss(static_cast<std::int64_t>(pid), sched::HeavyLegKind::BackgroundIndex);
            if (sampled) {
                sampledBytes = *sampled;
                sampleValid = true;
                sampleError.clear();
            } else {
                sampleError = sampled.error().describe();
            }
            sampledBytes = std::max(sampledBytes, gov->peakProcessRssBytes(static_cast<std::int64_t>(pid)));
        }
        sampledBytes = std::max(sampledBytes, gov->peakLegRssBytes(sched::HeavyLegKind::BackgroundIndex));
        LockGuard lock(mutex);
        if (round != sampledRound || process != sampledProcess || governorRef != gov) {
            return; // A sample from an older process/governor cannot validate the current round.
        }
        indexMetricsState.peakRssBytes = std::max(indexMetricsState.peakRssBytes, sampledBytes);
        indexMetricsState.peakRssSampleValid = sampleValid;
        indexMetricsState.peakRssSampleError = std::move(sampleError);
        if (!sampledProcess) {
            indexMetricsState.residentMemoryLimitBytes.reset();
        }
        indexMetricsState.governorRssCapBytes = gov->clangdRssCapBytes();
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
        if (options.onLog)
            options.onLog(line);
        LockGuard lock(mutex);
        stderrLines.emplace_back(line);
        while (stderrLines.size() > kStderrLines) {
            stderrLines.pop_front();
        }
    }

    void onDiagnostics(std::uint64_t fromRound, const Json& params) {
        sampleServerRss();
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
                OpenDocument& document = it->second;
                document.diagnosedVersion = decoded->lspVersion;
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

    // Calls the inactive-regions listeners with `regions`, outside the lock.
    void notifyInactive(const InactiveRegions& regions) {
        std::vector<ClangdService::InactiveRegionsListener> listeners;
        {
            LockGuard lock(mutex);
            for (const auto& [id, listener] : inactiveListeners) {
                listeners.push_back(listener);
            }
        }
        for (const ClangdService::InactiveRegionsListener& listener : listeners) {
            listener(regions);
        }
    }

    void onInactiveRegions(std::uint64_t fromRound, const Json& params) {
        auto decoded = decodeInactiveRegions(params);
        if (!decoded) {
            log::warn(kLogCategory, "dropped an inactiveRegions: {}", decoded.error().describe());
            return;
        }
        {
            LockGuard lock(mutex);
            if (fromRound != round) {
                return;
            }
            const std::string key = documentKey(decoded->file);
            auto it = documents.find(key);
            if (it == documents.end()) {
                return; // Closed meanwhile.
            }
            const OpenDocument& document = it->second;
            const text::Snapshot* seen = &document.latest;
            if (document.diagnosedVersion) {
                for (const auto& [version, snapshot] : document.history) {
                    if (version == *document.diagnosedVersion) {
                        seen = &snapshot;
                    }
                }
            }
            const PositionEncoding encoding = capabilities ? capabilities->positionEncoding : PositionEncoding::Utf16;
            decoded->file = document.path;
            mapInactiveRegions(*decoded, *seen, encoding);
            inactive.insert_or_assign(key, *decoded);
        }
        notifyInactive(*decoded);
    }

    // Decodes a $/progress notification ({token, value: {kind, title?, message?, percentage?}}),
    // updates background-index metrics & ResourceGovernor RSS tracking, and passes it to
    // options.onProgress; malformed ones and other rounds' are dropped.
    void onProgress(std::uint64_t fromRound, const Json& params) {
        {
            LockGuard lock(mutex);
            if (fromRound != round) {
                return;
            }
        }
        const Json* value = params.find("value");
        const Json* token = params.find("token");
        if (value == nullptr || !value->isObject() || token == nullptr) {
            return;
        }
        ClangdProgress progress;
        if (const std::string* text = token->asString()) {
            progress.token = *text;
        } else if (const auto number = token->asInteger()) {
            progress.token = std::to_string(*number);
        }
        const std::string_view kind = value->stringAt("kind");
        progress.kind = kind == "begin" ? ClangdProgress::Kind::Begin
                        : kind == "end" ? ClangdProgress::Kind::End
                                        : ClangdProgress::Kind::Report;
        progress.title = std::string(value->stringAt("title"));
        progress.message = std::string(value->stringAt("message"));
        if (const auto percentage = value->integerAt("percentage")) {
            progress.percentage = static_cast<int>(std::clamp<std::int64_t>(*percentage, 0, 100));
        }

        sampleServerRss();

        bool isIndexingProgress = false;
        std::optional<CompileCommandsLocation> currentLocation;
        const IClock::SteadyTime now = context.scheduler.clock().steadyNow();
        {
            LockGuard lock(mutex);
            if (progress.token == "backgroundIndexProgress" ||
                (!indexProgressToken.empty() && progress.token == indexProgressToken) ||
                (progress.kind == ClangdProgress::Kind::Begin && containsIgnoreCase(progress.title, "index"))) {
                isIndexingProgress = true;
                indexProgressToken = progress.token;
                currentLocation = compileCommands;
            }
        }

        if (isIndexingProgress) {
            switch (progress.kind) {
            case ClangdProgress::Kind::Begin: {
                governor()->setLegActive(sched::HeavyLegKind::BackgroundIndex, true);
                LockGuard lock(mutex);
                if (!indexStartedAt.has_value() || indexMetricsState.completed) {
                    indexStartedAt = now;
                }
                indexMetricsState.completed = false;
                break;
            }
            case ClangdProgress::Kind::Report: {
                LockGuard lock(mutex);
                if (!indexStartedAt.has_value()) {
                    indexStartedAt = now;
                }
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - *indexStartedAt).count();
                indexMetricsState.elapsedMs = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed));
                break;
            }
            case ClangdProgress::Kind::End: {
                governor()->setLegActive(sched::HeavyLegKind::BackgroundIndex, false);
                const std::size_t shards = countWorkspaceIndexShards(options.workspaceRoot, currentLocation);
                LockGuard lock(mutex);
                if (indexStartedAt.has_value()) {
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - *indexStartedAt).count();
                    indexMetricsState.elapsedMs = static_cast<std::uint64_t>(std::max<std::int64_t>(1, elapsed));
                }
                indexMetricsState.shardCount = std::max(indexMetricsState.shardCount, shards);
                indexMetricsState.completed = true;
                break;
            }
            }
        }

        if (options.onProgress) {
            options.onProgress(progress);
        }
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
        // One dispatcher for the notifications the client does not decode itself: clangd's
        // inactive regions and $/progress (both sides of the w1/unify merge).
        out.onNotification = [weak, forRound](std::string_view method, const Json& params) {
            if (method != kInactiveRegionsMethod && method != "$/progress") {
                return;
            }
            if (auto self = weak.lock()) {
                if (method == kInactiveRegionsMethod) {
                    self->onInactiveRegions(forRound, params);
                } else {
                    self->onProgress(forRound, params);
                }
            }
        };
        out.onMessage = [](int type, std::string_view message) {
            const log::Level level = type == 1 ? log::Level::Error : type == 2 ? log::Level::Warn : log::Level::Debug;
            log::write(level, kLogCategory, "{}", message);
        };
        out.onProtocolError = [](const Error& problem) { log::warn(kLogCategory, "protocol: {}", problem.describe()); };
        out.onApplyEdit = [weak](const Json& params, std::function<void(bool)> done) {
            if (auto self = weak.lock()) {
                ApplyEditHandler handler;
                {
                    LockGuard lock(self->mutex);
                    handler = self->applyEditHandler;
                }
                if (handler) {
                    handler(params, std::move(done));
                    return;
                }
            }
            done(false);
        };
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
        const std::shared_ptr<sched::ResourceGovernor> gov = governor();
        ClangdLaunchOptions governedLaunch = options.launch;
        io::ProcessSpec spec;
#ifdef _WIN32
        // Leave 16 MiB between the OS ceiling and the strict RSS gate. Small/invalid caps
        // request zero, which the launcher rejects instead of silently launching unbounded.
        constexpr std::uint64_t headroom = 16ULL * 1024ULL * 1024ULL;
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const auto page = static_cast<std::uint64_t>(system.dwPageSize);
        const auto cap = gov->clangdRssCapBytes();
        spec.residentMemoryLimitBytes = cap > headroom && page > 0 ? ((cap - headroom) / page) * page : 0;
#endif
        const unsigned governedJobs = gov->clangdJobs(spec.residentMemoryLimitBytes.has_value());
        if (governedLaunch.jobs == 0) {
            governedLaunch.jobs = governedJobs;
        } else if (gov->isLegActive(sched::HeavyLegKind::Build) || gov->isLegActive(sched::HeavyLegKind::Inspection) ||
                   gov->isCombinedOverRssCap()) {
            governedLaunch.jobs = std::min(governedLaunch.jobs, governedJobs);
        }

        spec.executable = options.clangdPath;
        spec.arguments = clangdArguments(governedLaunch, location);

        // Apply ResourceGovernor (FND-18, CPP-4) flags: --pch-storage=disk and
        // the platform's background-index priority.
        if (gov->pchStorageDisk()) {
            for (std::string& arg : spec.arguments) {
                if (arg == "--pch-storage=memory") {
                    arg = "--pch-storage=disk";
                }
            }
        }
        if (governedLaunch.backgroundIndex) {
            const std::string priorityFlag =
                std::format("--background-index-priority={}", gov->backgroundIndexPriority());
            const bool alreadyHasPriority = std::ranges::any_of(
                spec.arguments, [](const std::string& arg) { return arg.starts_with("--background-index-priority="); });
            if (!alreadyHasPriority) {
                auto it = std::ranges::find(spec.arguments, "--background-index");
                if (it != spec.arguments.end()) {
                    spec.arguments.insert(it + 1, priorityFlag);
                } else {
                    spec.arguments.insert(spec.arguments.begin(), priorityFlag);
                }
            }
        }

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
            {
                LockGuard lock(self->mutex);
                self->indexMetricsState.residentMemoryLimitBytes.reset();
            }
            std::optional<CompileCommandsLocation> location;
            if (self->options.compileCommands) {
                auto found = self->options.compileCommands->locate(stop);
                if (found) {
                    location = *found;
                } else {
                    log::warn(kLogCategory, "no compile commands: {}", found.error().describe());
                }
            }
            if (stop.stop_requested())
                co_return;
            if (location) {
                log::info(kLogCategory, "compile commands ({}): {}", toString(location->source), location->file);
            }
            const io::ProcessSpec launchSpec = self->processSpec(location);
            auto launched = self->launcher.launchAsync(launchSpec, self->context);
            if (!launched) {
                self->recordError(std::move(launched.error().withContext("starting clangd")), ClangdState::Failed);
                co_return;
            }
            std::shared_ptr<io::IAsyncProcess> process(std::move(*launched));
            auto processLease = location ? location->generatedLease : nullptr;
            if (processLease)
                processLease->beginUse();
            std::uint64_t thisRound = 0;
            {
                LockGuard lock(self->mutex);
                thisRound = ++self->round;
                self->indexMetricsState.peakRssSampleValid = false;
                self->indexMetricsState.peakRssSampleError = "new process round; OS peak not sampled yet";
            }
            auto client = std::make_shared<LspClient>(processByteStream(process), scheduler, self->initializeOptions(),
                                                      self->handlers(thisRound));
            {
                LockGuard lock(self->mutex);
                self->process = process;
                self->indexMetricsState.residentMemoryLimitBytes = launchSpec.residentMemoryLimitBytes;
                self->processLease = processLease;
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
            if (!roundError) {
                self->sampleServerRss();
            }
            if (!roundError && self->options.onLifecycle)
                self->options.onLifecycle();
            if (stop.stop_requested()) {
                co_return; // stop() shuts the round down.
            }
            if (!roundError) {
                auto exited = co_await process->exited(stop);
                if (!exited) {
                    co_return; // Stopped (or the reactor shut down): stop() takes over.
                }
                if (processLease)
                    processLease->confirmExit();
                roundError = Error(ErrorCode::IoError,
                                   std::format("clangd exited unexpectedly (exit code {})", exited->exitCode));
            } else {
                static_cast<void>(process->terminate());
                detail::Deadline reap(scheduler, std::chrono::seconds(5), stop);
                const auto exited = co_await process->exited(reap.token());
                if (processLease) {
                    if (exited)
                        processLease->confirmExit();
                    else
                        processLease->abandonUse();
                    LockGuard lock(self->mutex);
                    self->processLease.reset();
                }
                if (stop.stop_requested()) {
                    co_return;
                }
            }
            self->governor()->setLegActive(sched::HeavyLegKind::BackgroundIndex, false);
            self->governor()->unregisterProcess(static_cast<std::int64_t>(process->pid()));
            client->close();
            {
                LockGuard lock(self->mutex);
                ++self->restarts;
                self->client.reset();
                self->process.reset();
                self->indexMetricsState.residentMemoryLimitBytes.reset();
                self->processLease.reset();
                self->capabilities.reset();
            }
            if (self->options.onLifecycle)
                self->options.onLifecycle();
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

namespace {

struct ServiceRegistry {
    Mutex mutex;
    std::map<const ClangdService*, std::weak_ptr<ClangdService::Shared>> map FROST_GUARDED_BY(mutex);
};

[[nodiscard]] ServiceRegistry& serviceRegistry() noexcept {
    static auto* const instance = new ServiceRegistry();
    return *instance;
}

void registerServiceShared(const ClangdService* self, const std::shared_ptr<ClangdService::Shared>& shared) noexcept {
    auto& reg = serviceRegistry();
    LockGuard lock(reg.mutex);
    reg.map[self] = shared;
}

void unregisterServiceShared(const ClangdService* self) noexcept {
    auto& reg = serviceRegistry();
    LockGuard lock(reg.mutex);
    reg.map.erase(self);
}

[[nodiscard]] std::shared_ptr<ClangdService::Shared> lookupServiceShared(const ClangdService& self) noexcept {
    auto& reg = serviceRegistry();
    LockGuard lock(reg.mutex);
    auto it = reg.map.find(&self);
    return it != reg.map.end() ? it->second.lock() : nullptr;
}

} // namespace

ClangdService::ClangdService(io::IProcessLauncher& launcher, io::AsyncContext context, ClangdServiceOptions options)
    : m_shared(std::make_shared<Shared>(launcher, context, std::move(options))) {
    registerServiceShared(this, m_shared);
}

ClangdService::~ClangdService() {
    unregisterServiceShared(this);
    m_shared->stop.request_stop();
    const std::shared_ptr<sched::ResourceGovernor> gov = m_shared->governor();
    gov->setLegActive(sched::HeavyLegKind::BackgroundIndex, false);
    std::shared_ptr<LspClient> client;
    std::shared_ptr<io::IAsyncProcess> process;
    std::shared_ptr<GeneratedCommandsLease> processLease;
    {
        LockGuard lock(m_shared->mutex);
        client = std::move(m_shared->client);
        process = std::move(m_shared->process);
        m_shared->indexMetricsState.residentMemoryLimitBytes.reset();
        processLease = std::move(m_shared->processLease);
        m_shared->state = ClangdState::Stopped;
    }
    if (client) {
        client->close();
    }
    if (process) {
        gov->unregisterProcess(static_cast<std::int64_t>(process->pid()));
        static_cast<void>(process->terminate());
    }
    if (processLease)
        processLease->abandonUse();
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
    m_shared->stop.request_stop();
    return finishStop();
}

sched::Task<void> ClangdService::finishStop() {
    std::shared_ptr<Shared> self = m_shared;
    if (m_supervisor.valid()) {
        sched::JoinHandle<void> supervisor = std::move(m_supervisor);
        co_await std::move(supervisor);
    }
    self->sampleServerRss();
    const std::shared_ptr<sched::ResourceGovernor> gov = self->governor();
    gov->setLegActive(sched::HeavyLegKind::BackgroundIndex, false);
    std::shared_ptr<LspClient> client;
    std::shared_ptr<io::IAsyncProcess> process;
    std::shared_ptr<GeneratedCommandsLease> processLease;
    bool running = false;
    {
        LockGuard lock(self->mutex);
        client = std::move(self->client);
        process = std::move(self->process);
        self->indexMetricsState.residentMemoryLimitBytes.reset();
        processLease = std::move(self->processLease);
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
        const io::ProcessId pid = process->pid();
        const IClock::Duration spent = scheduler.clock().steadyNow() - begin;
        const IClock::Duration left =
            self->options.shutdownTimeout > spent ? self->options.shutdownTimeout - spent : IClock::Duration::zero();
        detail::Deadline deadline(scheduler, left);
        auto exited = co_await process->exited(deadline.token());
        if (!exited) {
            log::warn(kLogCategory, "clangd did not exit in time; killing it");
            static_cast<void>(process->terminate());
            detail::Deadline reap(scheduler, std::chrono::seconds(5));
            exited = co_await process->exited(reap.token());
        } else {
            log::info(kLogCategory, "clangd exited with {}", exited->exitCode);
        }
        gov->unregisterProcess(static_cast<std::int64_t>(pid));
        if (processLease) {
            if (exited)
                processLease->confirmExit();
            else
                processLease->abandonUse();
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

void setResourceGovernor(ClangdService& service, std::shared_ptr<sched::ResourceGovernor> governor) noexcept {
    if (auto shared = lookupServiceShared(service)) {
        shared->setGovernor(std::move(governor));
    }
}

std::shared_ptr<sched::ResourceGovernor> resourceGovernor(const ClangdService& service) noexcept {
    if (auto shared = lookupServiceShared(service)) {
        return shared->governor();
    }
    return nullptr;
}

IndexMetrics indexMetrics(const ClangdService& service) noexcept {
    auto shared = lookupServiceShared(service);
    if (!shared) {
        return {};
    }
    shared->sampleServerRss();
    std::optional<CompileCommandsLocation> location;
    std::shared_ptr<sched::ResourceGovernor> gov;
    IndexMetrics out;
    {
        LockGuard lock(shared->mutex);
        out = shared->indexMetricsState;
        location = shared->compileCommands;
        gov = shared->governorRef;
    }
    if (out.completed && out.shardCount == 0) {
        const std::size_t diskShards = countWorkspaceIndexShards(shared->options.workspaceRoot, location);
        LockGuard lock(shared->mutex);
        shared->indexMetricsState.shardCount = std::max(shared->indexMetricsState.shardCount, diskShards);
        out = shared->indexMetricsState;
    }
    out.governorRssCapBytes = gov->clangdRssCapBytes();
    return out;
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

std::optional<std::int64_t> ClangdService::documentVersion(std::string_view path) const {
    LockGuard lock(m_shared->mutex);
    const auto it = m_shared->documents.find(documentKey(path));
    if (it == m_shared->documents.end()) {
        return std::nullopt;
    }
    return it->second.version;
}

std::optional<std::int64_t> ClangdService::diagnosedVersion(std::string_view path) const {
    LockGuard lock(m_shared->mutex);
    const auto it = m_shared->documents.find(documentKey(path));
    if (it == m_shared->documents.end()) {
        return std::nullopt;
    }
    return it->second.diagnosedVersion;
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
    bool hadRegions = false;
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
        hadRegions = m_shared->inactive.erase(documentKey(cleared.file)) > 0;
    }
    if (hadRegions) {
        InactiveRegions none;
        none.file = cleared.file;
        none.uri = cleared.uri;
        m_shared->notifyInactive(none);
    }
    m_shared->diagnostics.publish(std::move(cleared));
    return {};
}

std::optional<InactiveRegions> ClangdService::inactiveRegions(std::string_view path) const {
    LockGuard lock(m_shared->mutex);
    auto it = m_shared->inactive.find(documentKey(path));
    return it != m_shared->inactive.end() ? std::optional<InactiveRegions>(it->second) : std::nullopt;
}

std::uint64_t ClangdService::addInactiveRegionsListener(InactiveRegionsListener listener) {
    LockGuard lock(m_shared->mutex);
    const std::uint64_t id = m_shared->nextInactiveListener++;
    m_shared->inactiveListeners.emplace(id, std::move(listener));
    return id;
}

void ClangdService::removeInactiveRegionsListener(std::uint64_t id) {
    LockGuard lock(m_shared->mutex);
    m_shared->inactiveListeners.erase(id);
}

Result<void> ClangdService::notify(const std::string& method, Json params) {
    std::shared_ptr<LspClient> client;
    {
        LockGuard lock(m_shared->mutex);
        client = m_shared->runningClient();
    }
    if (!client)
        return fail(ErrorCode::Busy, method + ": clangd is not running");
    return client->notify(method, std::move(params));
}

sched::Task<Result<Json>> ClangdService::request(std::string method, Json params, std::stop_token token) {
    m_shared->sampleServerRss();
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

void ClangdService::setApplyEditHandler(ApplyEditHandler handler) {
    LockGuard lock(m_shared->mutex);
    m_shared->applyEditHandler = std::move(handler);
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
