// SPDX-License-Identifier: Apache-2.0
//
// CMakeService (see frost/build/cmake_service.hpp): the folder state, the job pipeline (trust,
// Save All, tools and environment, configure, clean, build), the File API model, the debounced
// reconfigure and the persisted active profiles.

#include "frost/build/cmake_service.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/value.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <format>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::build {

namespace {

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;
constexpr std::string_view kStateHeader = "# FrostIDE CMake profiles v1";
constexpr std::string_view kStateFileName = "cmake-profiles.txt";
constexpr char kSeparator = kWindows ? '\\' : '/';

// "a, b, c".
std::string joinStrings(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += out.empty() ? item : ", " + item;
    }
    return out;
}

#ifdef _WIN32
// Numbers the private mspdbsrv endpoints (_MSPDBSRV_ENDPOINT_) of cl.exe children.
std::atomic<std::uint64_t> g_pdbEndpointCounter{0}; // Constant-initialised: no global constructor.
#endif

std::string seconds(IClock::Duration duration) {
    return std::format("{:.1f} s", std::chrono::duration<double>(duration).count());
}

std::string plural(std::size_t count, std::string_view word) {
    return std::format("{} {}{}", count, word, count == 1 ? "" : "s");
}

// A command line for the status line: arguments with spaces are quoted.
std::string commandLine(std::string_view executable, const std::vector<std::string>& arguments) {
    std::string text = std::string(io::fileName(executable));
    for (const std::string& argument : arguments) {
        text += ' ';
        text += argument.find(' ') != std::string::npos ? std::format("\"{}\"", argument) : argument;
    }
    return text;
}

// The persisted choices: "<source dir>\t<profile name>" lines under the header.
std::vector<std::pair<std::string, std::string>> readStateFile(io::IFileSystem& fs, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> entries;
    auto content = fs.readFile(path);
    if (!content) {
        return entries;
    }
    std::string_view text = *content;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (first) {
            first = false;
            if (line != kStateHeader) {
                return {};
            }
            continue;
        }
        const std::size_t tab = line.rfind('\t');
        if (tab != std::string_view::npos && tab > 0 && tab + 1 < line.size()) {
            entries.emplace_back(std::string(line.substr(0, tab)), std::string(line.substr(tab + 1)));
        }
    }
    return entries;
}

std::string fieldString(const settings::Record& record, std::string_view name) {
    const settings::FieldValue* value = settings::findField(record, name);
    return value != nullptr && std::holds_alternative<std::string>(*value) ? std::get<std::string>(*value)
                                                                           : std::string();
}

std::vector<std::string> fieldList(const settings::Record& record, std::string_view name) {
    const settings::FieldValue* value = settings::findField(record, name);
    return value != nullptr && std::holds_alternative<settings::StringList>(*value)
               ? std::get<settings::StringList>(*value)
               : std::vector<std::string>{};
}

// Whether `change` concerns a file whose documentKey is in `watched` (file_watcher.hpp's rules for
// a consumer of single paths).
bool touches(const io::FileChange& change, const std::vector<std::string>& watched) {
    const std::string key = documentKey(change.path);
    const std::string oldKey = change.oldPath.empty() ? std::string() : documentKey(change.oldPath);
    const bool subtree = change.kind == io::FileChangeKind::Created || change.kind == io::FileChangeKind::Removed ||
                         change.kind == io::FileChangeKind::Rescan || change.kind == io::FileChangeKind::Renamed;
    for (const std::string& file : watched) {
        if (file == key || (!oldKey.empty() && file == oldKey)) {
            return true;
        }
        if (subtree && !key.empty() && file.size() > key.size() && file.starts_with(key) &&
            file[key.size()] == kSeparator) {
            return true;
        }
        if (subtree && !oldKey.empty() && file.size() > oldKey.size() && file.starts_with(oldKey) &&
            file[oldKey.size()] == kSeparator) {
            return true;
        }
    }
    return false;
}

/*
 * A relative origin with a directory part ("../src/a.c", "sub/CMakeLists.txt") made absolute
 * against `base` and collapsed as CMake does (project::collapseCMakePath), so it names the file on
 * POSIX too, where io::normalizePath keeps "..". Bare names stay for the resolver (tool origins).
 */
Diagnostic withCMakePath(Diagnostic diagnostic, const std::string& base) {
    const std::string_view origin = diagnostic.file;
    if (origin.empty() || io::isAbsolutePath(origin) || origin.find_first_of("/\\") == std::string_view::npos) {
        return diagnostic;
    }
    if (auto joined = io::joinPath(base, origin)) {
        if (std::string collapsed = project::collapseCMakePath(*joined); !collapsed.empty()) {
            diagnostic.file = std::move(collapsed);
        }
    }
    return diagnostic;
}

bool isPresetsFileName(std::string_view path) {
    const std::string_view name = io::fileName(path);
    return name == project::kCMakePresetsFile || name == project::kCMakeUserPresetsFile;
}

// The greatest index-*.json name in `buildDirectory`'s reply directory (the current reply), or "".
std::string currentReplyIndex(io::IFileSystem& fs, std::string_view buildDirectory) {
    auto replyDir = project::cmakeFileApiReplyDirectory(buildDirectory);
    if (!replyDir) {
        return {};
    }
    auto entries = fs.listDirectory(*replyDir);
    if (!entries) {
        return {};
    }
    std::string newest;
    for (const io::DirEntry& entry : *entries) {
        if (entry.type == io::FileType::Regular && entry.name.starts_with("index-") && entry.name.ends_with(".json") &&
            entry.name > newest) {
            newest = entry.name;
        }
    }
    return newest.empty() ? std::string() : io::joinPath(*replyDir, newest).value_or(std::string());
}

// What identifies a profile's reply: the build directory, generator and the configuration it is read in.
bool sameReply(const project::CMakeProfile& a, const project::CMakeProfile& b) {
    return documentKey(a.buildDirectory) == documentKey(b.buildDirectory) && a.generator == b.generator &&
           (!project::isMultiConfigGenerator(a.generator) || a.effectiveConfiguration() == b.effectiveConfiguration());
}

} // namespace

std::string_view toString(CMakeStep step) noexcept {
    switch (step) {
    case CMakeStep::Configure: return "Configure";
    case CMakeStep::Clean: return "Clean";
    case CMakeStep::Build: return "Build";
    }
    return "Configure";
}

std::string_view toString(CMakeActivity activity) noexcept {
    switch (activity) {
    case CMakeActivity::Idle: return "Idle";
    case CMakeActivity::ReadingModel: return "Reading the CMake project model";
    case CMakeActivity::WaitingForTrust: return "Waiting for trust";
    case CMakeActivity::Preparing: return "Preparing";
    case CMakeActivity::Configuring: return "Configuring";
    case CMakeActivity::Generating: return "Generating";
    case CMakeActivity::Cleaning: return "Cleaning";
    case CMakeActivity::Building: return "Building";
    case CMakeActivity::Cancelling: return "Cancelling";
    }
    return "Idle";
}

std::vector<project::CMakeProfileRecord> cmakeProfileRecords(const settings::RecordList& records) {
    std::vector<project::CMakeProfileRecord> out;
    for (const settings::Record& record : records) {
        project::CMakeProfileRecord profile;
        profile.name = fieldString(record, "name");
        if (profile.name.empty()) {
            continue;
        }
        profile.buildType = fieldString(record, "buildType");
        profile.generator = fieldString(record, "generator");
        profile.buildDirectory = fieldString(record, "buildDirectory");
        profile.cacheVariables = fieldList(record, "cacheVariables");
        profile.environment = fieldList(record, "environment");
        profile.buildOptions = fieldList(record, "buildOptions");
        profile.root = fieldString(record, "root");
        if (const settings::FieldValue* jobs = settings::findField(record, "jobs");
            jobs != nullptr && std::holds_alternative<std::int64_t>(*jobs)) {
            profile.jobs = static_cast<int>(std::clamp<std::int64_t>(std::get<std::int64_t>(*jobs), 0, 1024));
        }
        out.push_back(std::move(profile));
    }
    return out;
}

std::vector<CMakeBuildWrapper> cmakeBuildWrappers(const settings::RecordList& records) {
    std::vector<CMakeBuildWrapper> out;
    for (const auto& record : records)
        out.push_back({.root = fieldString(record, "root"),
                       .profile = fieldString(record, "profile"),
                       .executable = fieldString(record, "executable"),
                       .defaultTarget = fieldString(record, "defaultTarget"),
                       .buildArguments = fieldList(record, "buildArguments"),
                       .cleanArguments = fieldList(record, "cleanArguments"),
                       .rebuildArguments = fieldList(record, "rebuildArguments")});
    return out;
}

struct CMakeService::State : std::enable_shared_from_this<CMakeService::State> {
    enum class JobKind : std::uint8_t { Configure, Build };

    struct AdmissionLease {
        std::function<void()> release;
        ~AdmissionLease() {
            if (release)
                release();
        }
    };
    struct Job {
        std::uint64_t id = 0;
        JobKind kind = JobKind::Configure;
        bool automatic = false; // A reconfigure nobody asked for: never prompts for trust.
        CMakeBuildRequest request{};
        project::CMakeProfile profile{};
        std::string sourceDir{};
        std::vector<CMakeStep> steps{}; // The steps still to run; front() is the current one once running.
        std::vector<std::string> untrusted{};
        std::unique_ptr<BuildSession> session{};
        std::optional<CMakeStep> current{};
        CMakeTools tools{};
        project::CMakeEnvironment msvc{};
        BuildResult lastResult{};
        CMakeBuildCompletion done{};
        std::shared_ptr<AdmissionLease> admission;
        std::optional<CMakeBuildWrapper> wrapper{};
        std::uint64_t trustRevision = 0;
        std::string replyIndexBefore{}; // The current reply's index when the configure step started.
        bool generating = false;        // cmake printed "-- Configuring done" in this configure step.
        bool readingReply = false;      // The configure step's reply is being read.
    };

    State(io::IFileSystem& fileSystem, io::IProcessLauncher& processLauncher, project::TrustStore& store,
          CMakeServiceOptions serviceOptions, CMakeServiceCallbacks serviceCallbacks)
        : fs(fileSystem), launcher(processLauncher), trustStore(store), options(std::move(serviceOptions)),
          callbacks(std::move(serviceCallbacks)), msvcCache(fileSystem, processLauncher, options.msvcCacheDirectory) {
        records = options.profileRecords;
        wrappers = options.buildWrappers;
        recordSource = options.profileRecordSource;
    }

    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    project::TrustStore& trustStore;
    const CMakeServiceOptions options;
    const CMakeServiceCallbacks callbacks;
    MsvcEnvironmentCache msvcCache;

    mutable Mutex mutex;
    std::string sourceDir FROST_GUARDED_BY(mutex);
    std::optional<project::CMakePresets> presets FROST_GUARDED_BY(mutex);
    std::optional<std::string> presetsProblem FROST_GUARDED_BY(mutex);
    std::vector<project::CMakeProfileRecord> records FROST_GUARDED_BY(mutex);
    std::vector<CMakeBuildWrapper> wrappers FROST_GUARDED_BY(mutex);
    project::CMakeProfileSource recordSource FROST_GUARDED_BY(mutex) = project::CMakeProfileSource::ApplicationSettings;
    std::vector<project::CMakeProfile> profiles FROST_GUARDED_BY(mutex);
    std::optional<std::size_t> active FROST_GUARDED_BY(mutex);
    std::shared_ptr<const project::CMakeProject> model FROST_GUARDED_BY(mutex);
    bool stale FROST_GUARDED_BY(mutex) = false;
    bool configuredOnce FROST_GUARDED_BY(mutex) = false; // A configure of the active profile ran since it was chosen.
    bool presetsTouched FROST_GUARDED_BY(mutex) = false;
    bool reloadPending FROST_GUARDED_BY(mutex) = false;       // Reconfigure once the running job ends.
    std::vector<std::string> watched FROST_GUARDED_BY(mutex); // documentKeys.
    std::vector<std::string> watchedPaths FROST_GUARDED_BY(mutex);
    BuildState state FROST_GUARDED_BY(mutex) = BuildState::Idle;
    std::optional<Job> job FROST_GUARDED_BY(mutex);
    std::uint64_t lastJobId FROST_GUARDED_BY(mutex) = 0;
    std::uint64_t profileGeneration FROST_GUARDED_BY(mutex) = 0;
    // Changes when the reply the model comes from changes (open, close, profile switch), so a
    // queued loadReply of an earlier folder or profile publishes nothing.
    std::uint64_t replyGeneration FROST_GUARDED_BY(mutex) = 0;
    // loadReply tasks queued and not yet run (CMakeService::modelLoading).
    int replyLoadsPending FROST_GUARDED_BY(mutex) = 0;
    bool replyLoadRunning FROST_GUARDED_BY(mutex) = false; // A loadReply is reading now.
    CMakeProgress progressValue FROST_GUARDED_BY(mutex);
    DiagnosticIndex diagnostics FROST_GUARDED_BY(mutex);
    std::optional<CMakeConfigureOutcome> lastConfigure FROST_GUARDED_BY(mutex);
    std::optional<CMakeBuildOutcome> lastBuild FROST_GUARDED_BY(mutex);
    std::optional<CMakeTools> tools FROST_GUARDED_BY(mutex);
    std::optional<std::vector<VsInstance>> instances FROST_GUARDED_BY(mutex);
    std::string announcedEnvironment FROST_GUARDED_BY(mutex); // The MSVC environment last named in the output.
    SaveAllHook saveAll FROST_GUARDED_BY(mutex);
    std::atomic<BuildState> publishedState{BuildState::Idle};

    Mutex persistMutex;

    // The service thread with its task queue and the debounced reconfigure's deadline.
    mutable Mutex queueMutex;
    mutable std::condition_variable queueChanged;
    std::deque<std::function<void()>> tasks FROST_GUARDED_BY(queueMutex);
    bool taskRunning FROST_GUARDED_BY(queueMutex) = false;
    bool stopping FROST_GUARDED_BY(queueMutex) = false;
    std::optional<std::chrono::steady_clock::time_point> reloadAt FROST_GUARDED_BY(queueMutex);
    std::thread worker;

    void post(std::function<void()> task) {
        {
            LockGuard lock(queueMutex);
            if (stopping) {
                return;
            }
            tasks.push_back(std::move(task));
        }
        queueChanged.notify_all();
    }

    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                UniqueLock lock(queueMutex);
                for (;;) {
                    if (stopping) {
                        return;
                    }
                    if (!tasks.empty()) {
                        task = std::move(tasks.front());
                        tasks.pop_front();
                        break;
                    }
                    if (reloadAt && std::chrono::steady_clock::now() >= *reloadAt) {
                        reloadAt.reset();
                        task = [this] { fireReload(); };
                        break;
                    }
                    if (reloadAt) {
                        queueChanged.wait_until(lock.native(), *reloadAt);
                    } else {
                        queueChanged.wait(lock.native());
                    }
                }
                taskRunning = true;
            }
            task();
            task = nullptr;
            {
                LockGuard lock(queueMutex);
                taskRunning = false;
            }
            queueChanged.notify_all();
        }
    }

    // ---- Events (each a task, so callbacks run in order on the service thread).

    template<class F>
    void emit(F&& call) {
        post(std::forward<F>(call));
    }

    void emitState(BuildState value) {
        emit([this, value] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(value);
            }
        });
    }

    void emitOutput(bool configure, BuildOutputKind kind, std::string line) {
        emit([this, configure, kind, line = std::move(line)] {
            const auto& callback = configure ? callbacks.onConfigureOutput : callbacks.onOutput;
            if (callback) {
                callback(kind, line);
            }
        });
    }

    void emitModel(std::shared_ptr<const project::CMakeProject> value) {
        emit([this, value = std::move(value)] {
            if (callbacks.onModelChanged) {
                callbacks.onModelChanged(value);
            }
        });
    }

    void emitProfilesLocked() FROST_REQUIRES(mutex) {
        emit([this, list = profiles, index = active] {
            if (callbacks.onProfilesChanged) {
                callbacks.onProfilesChanged(list, index);
            }
        });
    }

    void emitDiagnosticsCleared() {
        emit([this] {
            if (callbacks.onDiagnosticsCleared) {
                callbacks.onDiagnosticsCleared();
            }
        });
    }

    void setStateLocked(BuildState value) FROST_REQUIRES(mutex) {
        state = value;
        publishedState.store(value);
        emitState(value);
        refreshProgressLocked();
    }

    /*
     * Recomputes the activity (CMakeProgress) from the state and the job, and reports a change:
     * posted as an event, or with `direct` returned for the caller, a task on the service thread,
     * to deliver itself once it has released the lock (an event posted by a task runs only after
     * the task, too late for "reading the reply" said by the task that reads it). Sequence
     * numbers let the receiver drop an older value that arrives after a newer one.
     */
    std::optional<CMakeProgress> refreshProgressLocked(bool direct = false) FROST_REQUIRES(mutex) {
        CMakeProgress next;
        switch (state) {
        case BuildState::Idle:
            next.activity = replyLoadRunning ? CMakeActivity::ReadingModel : CMakeActivity::Idle;
            break;
        case BuildState::NeedsTrust:
            next.activity = CMakeActivity::WaitingForTrust;
            next.cancellable = true;
            break;
        case BuildState::SavingDocuments:
        case BuildState::Starting:
            next.activity = CMakeActivity::Preparing;
            next.cancellable = true;
            break;
        case BuildState::Running:
            if (job && job->readingReply) {
                next.activity = CMakeActivity::ReadingModel;
            } else if (job && job->current == CMakeStep::Configure) {
                next.activity = job->generating ? CMakeActivity::Generating : CMakeActivity::Configuring;
                next.cancellable = true;
            } else {
                next.activity =
                    job && job->current == CMakeStep::Clean ? CMakeActivity::Cleaning : CMakeActivity::Building;
                next.cancellable = true;
            }
            break;
        case BuildState::Cancelling: next.activity = CMakeActivity::Cancelling; break;
        }
        if (next.activity != CMakeActivity::Idle) {
            if (job) {
                next.profile = job->profile.name;
            } else if (auto profile = activeProfileLocked()) {
                next.profile = profile->name;
            }
        }
        if (next.activity == progressValue.activity && next.profile == progressValue.profile &&
            next.cancellable == progressValue.cancellable) {
            return std::nullopt;
        }
        next.since = std::chrono::steady_clock::now();
        next.sequence = progressValue.sequence + 1;
        progressValue = next;
        if (direct) {
            return next;
        }
        emit([this, next] { deliverProgress(next); });
        return std::nullopt;
    }

    void deliverProgress(const std::optional<CMakeProgress>& value) const {
        if (value && callbacks.onProgress) {
            callbacks.onProgress(*value);
        }
    }

    // ---- Folder state.

    bool isCurrentLocked(std::uint64_t id) const FROST_REQUIRES(mutex) { return job && job->id == id; }

    bool wrapperTrustCurrentLocked() const FROST_REQUIRES(mutex) {
        if (!job || !job->wrapper)
            return true;
        const auto trust = trustStore.snapshot(job->sourceDir);
        return trust.trusted && trust.revision == job->trustRevision;
    }

    std::function<bool()> outputFenceLocked(std::uint64_t id) FROST_REQUIRES(mutex) {
        const auto root = job->sourceDir;
        const auto revision = job->trustRevision;
        const auto generation = profileGeneration;
        std::weak_ptr<State> weak = weak_from_this();
        return [weak, id, root, revision, generation] {
            auto state = weak.lock();
            if (!state)
                return false;
            LockGuard lock(state->mutex);
            const auto trust = state->trustStore.snapshot(root);
            return state->lastJobId == id && state->sourceDir == root && state->profileGeneration == generation &&
                   trust.trusted && trust.revision == revision;
        };
    }

    void emitWrapperOutputLocked(BuildOutputKind kind, std::string line) FROST_REQUIRES(mutex) {
        auto fence = outputFenceLocked(job->id);
        emit([this, kind, line = std::move(line), fence = std::move(fence)] {
            if (!fence())
                return;
            if (callbacks.onOutputFenced)
                callbacks.onOutputFenced(kind, line, fence);
            else if (callbacks.onOutput)
                callbacks.onOutput(kind, line);
        });
    }

    std::optional<project::CMakeProfile> activeProfileLocked() const FROST_REQUIRES(mutex) {
        if (!active || *active >= profiles.size()) {
            return std::nullopt;
        }
        return profiles[*active];
    }

    void recomputeWatchedLocked() FROST_REQUIRES(mutex) {
        watched.clear();
        watchedPaths.clear();
        const auto add = [this](const std::string& path) FROST_NO_THREAD_SAFETY_ANALYSIS {
            std::string key = documentKey(path);
            if (!key.empty() && std::ranges::find(watched, key) == watched.end()) {
                watched.push_back(std::move(key));
                watchedPaths.push_back(path);
            }
        };
        if (sourceDir.empty()) {
            return;
        }
        for (std::string_view name :
             {std::string_view("CMakeLists.txt"), project::kCMakePresetsFile, project::kCMakeUserPresetsFile}) {
            if (auto path = io::joinPath(sourceDir, name)) {
                add(*path);
            }
        }
        if (presets) {
            for (const std::string& file : presets->files) {
                add(file);
            }
        }
        if (model) {
            for (const std::string& input : model->projectInputs()) {
                add(input);
            }
        }
    }

    // The configuration a reply is read in for `profile`.
    static std::string replyConfiguration(const project::CMakeProfile& profile) {
        return project::isMultiConfigGenerator(profile.generator) ? profile.effectiveConfiguration() : std::string();
    }

    /*
     * Reads `profile`'s existing reply (blocking); nullptr when there is none, it does not parse,
     * or its codemodel names a source directory other than `folder` (header comment, "Stale
     * replies": a build directory configured from another tree is not this folder's model).
     */
    std::shared_ptr<const project::CMakeProject> readReply(const project::CMakeProfile& profile,
                                                           const std::string& folder) {
        auto reply = project::readCMakeFileApiReply(fs, profile.buildDirectory, replyConfiguration(profile));
        if (!reply && reply.error().code() == ErrorCode::NotFound) {
            reply = project::readCMakeFileApiReply(fs, profile.buildDirectory);
        }
        if (!reply || !*reply) {
            return nullptr;
        }
        if (documentKey((*reply)->sourceDirectory) != documentKey(folder)) {
            return nullptr;
        }
        return std::make_shared<const project::CMakeProject>(std::move(**reply));
    }

    /*
     * Whether `reply` is older than one of its project inputs or the presets files
     * `presetFiles` (modified after its index file was written: the inputs changed while nobody
     * watched). A status that cannot be read counts as not newer. O(inputs) status calls.
     */
    bool replyOlderThanInputs(const project::CMakeProject& reply, const std::vector<std::string>& presetFiles) {
        auto index = fs.status(reply.replyIndex);
        if (!index) {
            return false;
        }
        std::vector<std::string> inputs = reply.projectInputs();
        inputs.insert(inputs.end(), presetFiles.begin(), presetFiles.end());
        return std::ranges::any_of(inputs, [&](const std::string& input) {
            auto status = fs.status(input);
            return status && status->mtime > index->mtime;
        });
    }

    /*
     * Service thread: reads `profile`'s existing reply and publishes it as the model, then calls
     * onModelChanged with it (or nullptr). The read is blocking (the File API reply of a large
     * project is several MB of JSON: about 500 ms for FrostEngine in Debug), which is why
     * openFolder and setActiveProfile queue it here instead of reading on their caller's (the GUI)
     * thread (R0 exit criterion, FND-9a). It runs in queue order, before any job those calls start,
     * and publishes nothing when the folder or profile changed since it was queued (`generation`)
     * or a configure published a model first. `lease` keeps the profile switch's admission until
     * the read ends. Each call was counted in replyLoadsPending when it was queued; the count drops
     * under the same lock that publishes the model, so modelLoading() and state() never both read
     * "nothing pending" before the model and any configure it starts are in place.
     *
     * With `configureIfMissing` (a profile switch whose reply directory existed when it was made,
     * but whose reply did not parse), an idle, trusted folder configures it (configureOnOpen). A
     * switch to a profile with no reply directory at all starts that configure synchronously in
     * setActiveProfile instead, so state() is Configuring when it returns (R0 follow-up 10). A job
     * the user started between the switch and this read makes startJobLocked answer Busy, and the
     * configure is dropped on purpose: that job began while model() was nullptr, so build() gave
     * it a Configure step of its own (and configure() is one), and a second configure after it
     * would only repeat that work.
     *
     * openFolder queues this read and then starts configure-on-open at once, so a trusted open may
     * read the old reply while cmake rewrites it. That is intended: the configure publishes its own
     * model (configuredOnce, then this read publishes nothing), and a torn read only yields nullptr
     * or a parse error, which the configure's publication replaces.
     */
    void loadReply(std::uint64_t generation, const project::CMakeProfile& profile, bool configureIfMissing,
                   const std::shared_ptr<AdmissionLease>& lease) {
        static_cast<void>(lease);
        std::string folder;
        std::vector<std::string> presetFiles;
        std::optional<CMakeProgress> reading;
        {
            LockGuard lock(mutex);
            folder = sourceDir;
            presetFiles = presets ? presets->files : std::vector<std::string>{};
            if (replyGeneration == generation) {
                replyLoadRunning = true;
                reading = refreshProgressLocked(true);
            }
        }
        deliverProgress(reading);
        auto reply = readReply(profile, folder);
        const bool older = reply && replyOlderThanInputs(*reply, presetFiles);
        std::optional<CMakeProgress> done;
        bool publish = false;
        {
            LockGuard lock(mutex);
            --replyLoadsPending;
            if (replyLoadRunning) {
                replyLoadRunning = false;
                done = refreshProgressLocked(true);
            }
            if (replyGeneration == generation && !configuredOnce && !model) {
                publish = true;
                model = reply;
                stale = older;
                recomputeWatchedLocked();
                if (configureIfMissing && !reply && options.configureOnOpen) {
                    // Busy only for a job started since the switch, which configures itself (above).
                    static_cast<void>(startJobLocked(JobKind::Configure, true, {}, {CMakeStep::Configure}));
                }
            }
        }
        deliverProgress(done);
        // Directly, not through emit: this task holds the place in the event order where the model
        // of the open or switch belongs (before the events of the job it may have started).
        if (publish && callbacks.onModelChanged) {
            callbacks.onModelChanged(reply);
        }
    }

    std::optional<std::string> persistedProfile(const std::string& folder) {
        if (options.stateFilePath.empty()) {
            return std::nullopt;
        }
        LockGuard lock(persistMutex);
        const std::string key = documentKey(folder);
        for (const auto& [path, name] : readStateFile(fs, options.stateFilePath)) {
            if (documentKey(path) == key) {
                return name;
            }
        }
        return std::nullopt;
    }

    Result<void> persist(const std::string& folder, const std::string& name) {
        if (options.stateFilePath.empty()) {
            return {};
        }
        LockGuard lock(persistMutex);
        auto entries = readStateFile(fs, options.stateFilePath);
        const std::string key = documentKey(folder);
        std::erase_if(entries, [&](const auto& entry) { return documentKey(entry.first) == key; });
        entries.emplace_back(folder, name);
        std::string text = std::format("{}\n", kStateHeader);
        for (const auto& [path, profile] : entries) {
            text += std::format("{}\t{}\n", path, profile);
        }
        if (auto created = fs.createDirectories(io::parentPath(options.stateFilePath)); !created) {
            return created;
        }
        return fs.writeFileAtomic(options.stateFilePath, text);
    }

    // The index of the profile named `name`, else the first profile of configure preset `name`,
    // else the first usable profile, else 0.
    static std::optional<std::size_t> chooseProfile(const std::vector<project::CMakeProfile>& list,
                                                    const std::optional<std::string>& name) {
        if (list.empty()) {
            return std::nullopt;
        }
        if (name) {
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].name == *name) {
                    return i;
                }
            }
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (list[i].source == project::CMakeProfileSource::Preset && list[i].configurePreset == *name) {
                    return i;
                }
            }
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].usable()) {
                return i;
            }
        }
        return 0;
    }

    /*
     * Recomputes the profiles from the presets and records, keeping the active one by name. When
     * the active profile's reply changes (another build directory, generator or configuration, or
     * the profile is gone), the old model is dropped and the new active profile's reply is read
     * on the service thread, as a switch reads it (header comment, "Stale replies"; CMK-1).
     * `configureIfMissing` is loadReply's. Emits the profiles.
     */
    void recomputeProfilesLocked(bool configureIfMissing) FROST_REQUIRES(mutex) {
        const std::optional<project::CMakeProfile> before = activeProfileLocked();
        profiles = project::cmakeProfilesFor(sourceDir, presets, records, recordSource);
        active = chooseProfile(profiles, before ? std::optional<std::string>(before->name) : std::nullopt);
        const std::optional<project::CMakeProfile> after = activeProfileLocked();
        const bool replyChanged = before.has_value() != after.has_value() ||
                                  (before && after && (before->name != after->name || !sameReply(*before, *after)));
        if (replyChanged) {
            const std::uint64_t load = ++replyGeneration;
            model = nullptr;
            stale = false;
            configuredOnce = false;
            if (after) {
                ++replyLoadsPending;
                post([this, load, profile = *after, configureIfMissing] {
                    loadReply(load, profile, configureIfMissing, nullptr);
                });
            } else {
                emitModel(nullptr);
            }
        }
        recomputeWatchedLocked();
        emitProfilesLocked();
    }

    // ---- Jobs.

    bool folderTrustedLocked() const FROST_REQUIRES(mutex) { return trustStore.isTrusted(sourceDir); }

    /*
     * Starts a job for the active profile. An automatic job of an untrusted folder or an unusable
     * profile is dropped. Errors: InvalidArgument (no folder, unusable profile), Busy.
     */
    Result<void> startJobLocked(JobKind kind, bool automatic, CMakeBuildRequest request, std::vector<CMakeStep> steps,
                                CMakeBuildCompletion done = {}, std::uint64_t owner = 0) FROST_REQUIRES(mutex) {
        if (sourceDir.empty()) {
            return fail(ErrorCode::InvalidArgument, "no CMake folder is open");
        }
        if (state != BuildState::Idle) {
            return fail(ErrorCode::Busy, std::format("a CMake job is already in progress ({})", toString(state)));
        }
        auto profile = activeProfileLocked();
        if (!profile) {
            return fail(ErrorCode::InvalidArgument, "the folder has no CMake profile");
        }
        if (!profile->usable()) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("profile {} cannot be used: {}", profile->name, profile->problem));
        }
        if (kind == JobKind::Configure) {
            const std::string folder = sourceDir;
            const bool wrapped = std::ranges::any_of(wrappers, [&](const CMakeBuildWrapper& candidate) {
                return documentKey(candidate.root) == documentKey(folder) && candidate.profile == profile->name;
            });
            if (wrapped && automatic)
                return {};
        }
        const bool trusted = folderTrustedLocked();
        if (automatic && !trusted) {
            return {}; // Nothing runs before trust, and nothing asks for it unprompted.
        }
        std::optional<CMakeBuildWrapper> wrapper;
        if (kind == JobKind::Build) {
            for (const auto& candidate : wrappers) {
                if (documentKey(candidate.root) != documentKey(sourceDir) || candidate.profile != profile->name)
                    continue;
                if (wrapper)
                    return fail(ErrorCode::InvalidArgument, "multiple build wrappers match this root and profile");
                wrapper = candidate;
            }
            if (wrapper) {
                if (!model)
                    return fail(ErrorCode::InvalidArgument,
                                "configure the selected profile before using its build wrapper");
                if (!io::isAbsolutePath(wrapper->root) || !io::isAbsolutePath(wrapper->executable))
                    return fail(ErrorCode::InvalidArgument, "build wrapper root and executable must be absolute");
                const auto& arguments = request.action == BuildAction::Build   ? wrapper->buildArguments
                                        : request.action == BuildAction::Clean ? wrapper->cleanArguments
                                                                               : wrapper->rebuildArguments;
                if (arguments.empty())
                    return fail(ErrorCode::Unsupported, "the selected build wrapper does not define this action");
                if (!request.targetName.empty() && std::ranges::find(arguments, "{target}") == arguments.end())
                    return fail(ErrorCode::InvalidArgument, "the wrapper does not forward the selected target");
                steps = {request.action == BuildAction::Clean ? CMakeStep::Clean : CMakeStep::Build};
            }
        }
        bool admissionReserved = false;
        std::uint64_t admissionChild = 0;
        if (owner) {
            if (!options.reserveOwnedProjectCode || !options.releaseOwnedProjectCode)
                return fail(ErrorCode::InvalidArgument, "owned admission is unavailable");
            auto child = options.reserveOwnedProjectCode(owner);
            if (!child)
                return std::unexpected(std::move(child.error()));
            admissionChild = *child;
        } else if (options.reserveProjectCode && options.releaseProjectCode) {
            auto reserved = options.reserveProjectCode();
            if (!reserved) {
                if (automatic && reserved.error().code() == ErrorCode::Busy)
                    reloadPending = true;
                return std::unexpected(std::move(reserved.error()));
            }
            admissionReserved = true;
        }
        std::shared_ptr<AdmissionLease> admission;
        if (admissionChild) {
            auto release = options.releaseOwnedProjectCode;
            admission = std::make_shared<AdmissionLease>();
            admission->release = [release = std::move(release), owner, admissionChild] {
                release(owner, admissionChild);
            };
        } else if (admissionReserved) {
            admission = std::make_shared<AdmissionLease>();
            admission->release = options.releaseProjectCode;
        }
        if (admission) {
            auto release = std::move(admission->release);
            auto weak = weak_from_this();
            admission->release = [release = std::move(release), weak] {
                release();
                if (auto state = weak.lock()) {
                    state->post([weak] {
                        if (auto state = weak.lock()) {
                            bool retry;
                            {
                                LockGuard lock(state->mutex);
                                retry = state->reloadPending && state->state == BuildState::Idle;
                            }
                            if (retry)
                                state->fireReload();
                        }
                    });
                }
            };
        }
        request.profile = profile->name;
        diagnostics.clear();
        emitDiagnosticsCleared();
        job = Job{.id = ++lastJobId,
                  .kind = kind,
                  .automatic = automatic,
                  .request = std::move(request),
                  .profile = std::move(*profile),
                  .sourceDir = sourceDir,
                  .steps = std::move(steps),
                  .untrusted = {},
                  .session = nullptr,
                  .current = std::nullopt,
                  .tools = {},
                  .msvc = {},
                  .lastResult = {},
                  .done = std::move(done),
                  .admission = std::move(admission),
                  .wrapper = std::move(wrapper),
                  .trustRevision = 0};
        if (!trusted) {
            job->untrusted = {sourceDir};
            setStateLocked(BuildState::NeedsTrust);
            emit([this, roots = job->untrusted] {
                if (callbacks.onNeedsTrust) {
                    callbacks.onNeedsTrust(roots);
                }
            });
            return {};
        }
        afterTrustLocked();
        return {};
    }

    void afterTrustLocked() FROST_REQUIRES(mutex) {
        job->untrusted.clear();
        job->trustRevision = trustStore.snapshot(job->sourceDir).revision;
        const std::uint64_t id = job->id;
        if (job->kind == JobKind::Build) {
            setStateLocked(BuildState::SavingDocuments);
            post([this, id] { saveDocuments(id); });
        } else {
            setStateLocked(BuildState::Starting);
            post([this, id] { prepare(id); });
        }
    }

    void saveDocuments(std::uint64_t id) {
        SaveAllHook hook;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id) || state != BuildState::SavingDocuments) {
                return;
            }
            hook = saveAll;
            if (!hook) {
                setStateLocked(BuildState::Starting);
                post([this, id] { prepare(id); });
                return;
            }
        }
        auto called = std::make_shared<std::atomic<bool>>(false);
        hook([weak = weak_from_this(), id, called](Result<void> saved) {
            if (called->exchange(true)) {
                return;
            }
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            State* raw = self.get();
            raw->post([raw, id, saved = std::move(saved)]() mutable {
                LockGuard lock(raw->mutex);
                if (!raw->isCurrentLocked(id) || raw->state != BuildState::SavingDocuments) {
                    return;
                }
                if (!saved) {
                    raw->finishJobLocked(
                        std::move(saved.error()).withContext("while saving the documents before the build"));
                    return;
                }
                raw->setStateLocked(BuildState::Starting);
                raw->post([raw, id] { raw->prepare(id); });
            });
        });
    }

    // Service thread, state Starting: tools, the MSVC environment, then the first step.
    void prepare(std::uint64_t id) {
        project::CMakeProfile profile;
        std::optional<CMakeTools> cachedTools;
        std::optional<std::vector<VsInstance>> cachedInstances;
        std::string announced;
        bool wrapperJob = false;
        std::shared_ptr<AdmissionLease> admission;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                return;
            }
            admission = job->admission;
            profile = job->profile;
            wrapperJob = job->wrapper.has_value();
            cachedTools = tools;
            cachedInstances = instances;
            announced = announcedEnvironment;
        }
        if (wrapperJob) {
            LockGuard lock(mutex);
            if (isCurrentLocked(id))
                runNextStepLocked();
            return;
        }
        // Blocking work runs without the lock; cancel() may end the job meanwhile.
        if (!cachedInstances) {
            std::vector<VsInstance> found;
            if (kWindows && options.useVisualStudio) {
                auto listed = options.listInstances ? options.listInstances() : listVsInstances(launcher, fs);
                if (listed) {
                    found = std::move(*listed);
                }
            }
            cachedInstances = std::move(found);
        }
        std::vector<std::string> notes;
        if (!cachedTools) {
            CMakeToolOptions toolOptions{.cmakePath = options.cmakePath,
                                         .ninjaPath = options.ninjaPath,
                                         .vsInstances = *cachedInstances,
                                         .pathVariable = options.pathVariable.empty() ? processPathVariable()
                                                                                      : options.pathVariable};
            auto found = findCMakeTools(fs, toolOptions);
            if (!found) {
                LockGuard lock(mutex);
                if (isCurrentLocked(id)) {
                    finishJobLocked(std::move(found.error()).withContext("while looking for cmake"));
                }
                return;
            }
            auto version = cmakeVersion(launcher, found->cmake);
            if (!version) {
                LockGuard lock(mutex);
                if (isCurrentLocked(id)) {
                    finishJobLocked(std::move(version.error()).withContext("while asking cmake for its version"));
                }
                return;
            }
            if (!versionAtLeast(*version, 3, 21)) {
                LockGuard lock(mutex);
                if (isCurrentLocked(id)) {
                    finishJobLocked(
                        Error(ErrorCode::Unsupported, std::format("{} is CMake {}; FrostIDE needs CMake 3.21 or later",
                                                                  found->cmake, *version)));
                }
                return;
            }
            notes.push_back(
                std::format("Using CMake {} ({}, from {})", *version, found->cmake, toString(found->cmakeOrigin)));
            if (!found->ninja.empty()) {
                notes.push_back(std::format("Using Ninja {} (from {})", found->ninja, toString(found->ninjaOrigin)));
            }
            cachedTools = std::move(*found);
        }
        if (project::isNinjaGenerator(profile.generator) && cachedTools->ninja.empty() &&
            std::ranges::none_of(profile.cacheVariables,
                                 [](const project::CMakeCacheVariable& v) { return v.name == "CMAKE_MAKE_PROGRAM"; })) {
            LockGuard lock(mutex);
            if (isCurrentLocked(id)) {
                finishJobLocked(Error(ErrorCode::NotFound, "the profile uses Ninja, but no ninja was found: set "
                                                           "tools.ninja.path or put ninja on PATH"));
            }
            return;
        }
        project::CMakeEnvironment msvc;
        if (kWindows && options.captureMsvcEnvironment && !project::isVisualStudioGenerator(profile.generator)) {
            const VsInstance* chosen = nullptr;
            for (const VsInstance& instance : *cachedInstances) {
                const bool wanted = options.vsInstanceId.empty() || instance.instanceId == options.vsInstanceId;
                auto vcvarsall = vcvarsallPath(instance.installationPath);
                if (wanted && vcvarsall && fs.exists(*vcvarsall).value_or(false)) {
                    chosen = &instance;
                    break;
                }
            }
            if (chosen == nullptr) {
                notes.emplace_back("No Visual Studio C++ tools were found; the compilers come from PATH");
            } else {
                const std::string architecture = profile.architectureExternal && !profile.architecture.empty()
                                                     ? profile.architecture
                                                     : options.msvcArchitecture;
                auto environment = msvcCache.get(*chosen, architecture);
                if (!environment) {
                    LockGuard lock(mutex);
                    if (isCurrentLocked(id)) {
                        finishJobLocked(
                            std::move(environment.error()).withContext("while setting up the MSVC environment"));
                    }
                    return;
                }
                msvc = std::move(environment->changes);
                std::string note = std::format("Using the MSVC environment of {} {} ({})", chosen->displayName,
                                               chosen->installationVersion, architecture);
                if (note != announced) { // Once, and again when it changes.
                    announced = note;
                    notes.push_back(std::move(note));
                }
            }
        }
        LockGuard lock(mutex);
        tools = cachedTools;
        instances = cachedInstances;
        announcedEnvironment = announced;
        if (!isCurrentLocked(id)) {
            return;
        }
        job->tools = *cachedTools;
        job->msvc = std::move(msvc);
        const bool configureFirst = job->kind == JobKind::Configure || job->steps.front() == CMakeStep::Configure;
        for (std::string& note : notes) {
            emitOutput(configureFirst, BuildOutputKind::Status, std::move(note));
        }
        runNextStepLocked();
    }

    project::CMakeEnvironment environmentFor(const Job& current, bool configure) const {
        project::CMakeEnvironment environment = current.msvc;
        const project::CMakeEnvironment& own = configure || !current.profile.buildEnvironment
                                                   ? current.profile.environment
                                                   : *current.profile.buildEnvironment;
        environment.insert(environment.end(), own.begin(), own.end());
        if (kWindows) {
            environment.emplace_back("VSLANG", std::string("1033"));
            environment.emplace_back("NoDefaultCurrentDirectoryInExePath", std::nullopt);
#ifdef _WIN32
            environment.emplace_back("_MSPDBSRV_ENDPOINT_",
                                     std::format("FrostIDE-CMake-{}-{}", GetCurrentProcessId(),
                                                 g_pdbEndpointCounter.fetch_add(1, std::memory_order_relaxed)));
#endif
        }
        return environment;
    }

    void runNextStepLocked() FROST_REQUIRES(mutex) {
        if (job->wrapper) {
            const auto trust = trustStore.snapshot(job->sourceDir);
            if (!trust.trusted || trust.revision != job->trustRevision) {
                finishJobLocked(Error(ErrorCode::Cancelled, "project trust changed before wrapper execution"));
                return;
            }
        }
        if (job->steps.empty()) {
            finishJobLocked(std::nullopt);
            return;
        }
        const CMakeStep step = job->steps.front();
        job->current = step;
        job->readingReply = false;
        const bool configure = step == CMakeStep::Configure;
        io::ProcessSpec spec;
        spec.executable = job->tools.cmake;
        spec.stdinMode = io::Stdio::Null;
        spec.killOnClose = true;
        spec.environment = environmentFor(*job, configure);
        if (configure) {
            if (auto created = fs.createDirectories(job->profile.buildDirectory); !created) {
                finishJobLocked(std::move(created.error()).withContext("while creating the build directory"));
                return;
            }
            if (auto query = project::writeCMakeFileApiQuery(fs, job->profile.buildDirectory); !query) {
                finishJobLocked(std::move(query.error()).withContext("while writing the CMake File API query"));
                return;
            }
            // The reply read after this configure must be newer (header comment, "Stale replies").
            job->replyIndexBefore = currentReplyIndex(fs, job->profile.buildDirectory);
            job->generating = false;
            auto arguments = project::cmakeConfigureArguments(job->profile, job->sourceDir, job->tools.ninja);
            if (!arguments) {
                finishJobLocked(std::move(arguments.error()));
                return;
            }
            spec.arguments = std::move(*arguments);
            spec.workingDirectory = job->sourceDir;
            emit([this, profile = job->profile.name] {
                if (callbacks.onConfigureStarted) {
                    callbacks.onConfigureStarted(profile);
                }
            });
            emitOutput(true, BuildOutputKind::Status,
                       std::format("Configure started: profile {}, build directory {}", job->profile.name,
                                   job->profile.buildDirectory));
        } else if (job->wrapper) {
            const auto& wrapper = *job->wrapper;
            spec.executable = wrapper.executable;
            spec.workingDirectory = job->sourceDir;
            const auto& configured = job->request.action == BuildAction::Build   ? wrapper.buildArguments
                                     : job->request.action == BuildAction::Clean ? wrapper.cleanArguments
                                                                                 : wrapper.rebuildArguments;
            const std::string target =
                job->request.targetName.empty() ? wrapper.defaultTarget : job->request.targetName;
            for (const auto& argument : configured) {
                if (argument == "{target}") {
                    if (target.empty()) {
                        finishJobLocked(Error(ErrorCode::InvalidArgument, "the wrapper needs a target"));
                        return;
                    }
                    spec.arguments.push_back(target);
                } else if (argument == "{profile}")
                    spec.arguments.push_back(job->profile.name);
                else
                    spec.arguments.push_back(argument);
            }
            emitWrapperOutputLocked(BuildOutputKind::Status,
                                    std::format("{} started with the configured wrapper", toString(step)));
        } else {
            std::vector<std::string> targets;
            if (!job->request.targetName.empty()) {
                targets.push_back(job->request.targetName);
            }
            spec.arguments = project::cmakeBuildArguments(job->profile, targets, step == CMakeStep::Clean,
                                                          static_cast<int>(options.buildJobs));
            spec.workingDirectory = job->profile.buildDirectory;
            // Without a chosen target the profile's build preset decides (its "targets").
            std::string what = std::format("target {}", job->request.targetName);
            if (job->request.targetName.empty()) {
                what = job->profile.buildTargets.empty()
                           ? std::string("all targets")
                           : std::format("the build preset's targets ({})", joinStrings(job->profile.buildTargets));
            }
            emitOutput(false, BuildOutputKind::Status,
                       std::format("{} started: {}, profile {}", toString(step), what, job->profile.name));
        }
        if (job->wrapper)
            emitWrapperOutputLocked(BuildOutputKind::Status, commandLine(spec.executable, spec.arguments));
        else
            emitOutput(configure, BuildOutputKind::Status, commandLine(spec.executable, spec.arguments));

        const std::uint64_t id = job->id;
        const std::string base = (configure || job->wrapper) ? job->sourceDir : job->profile.buildDirectory;
        auto resolver = std::make_shared<DiagnosticResolver>(fs, base);
        BuildCallbacks sessionCallbacks{
            .onLine =
                [this, id, configure](OutputStream stream, std::string_view line) {
                    LockGuard lock(mutex);
                    if (isCurrentLocked(id) && wrapperTrustCurrentLocked()) {
                        const auto kind =
                            stream == OutputStream::Stdout ? BuildOutputKind::Stdout : BuildOutputKind::Stderr;
                        if (job->wrapper) {
                            auto fence = outputFenceLocked(id);
                            emit([this, kind, text = std::string(line), fence = std::move(fence)] {
                                if (!fence())
                                    return;
                                if (callbacks.onOutputFenced)
                                    callbacks.onOutputFenced(kind, text, fence);
                                else if (callbacks.onOutput)
                                    callbacks.onOutput(kind, text);
                            });
                        } else
                            emitOutput(configure, kind, std::string(line));
                        // cmake's own phase marker (CMakeActivity::Generating).
                        if (configure && !job->generating && line.starts_with("-- Configuring done")) {
                            job->generating = true;
                            refreshProgressLocked();
                        }
                    }
                },
            .onDiagnostic =
                [this, id, resolver, base](const Diagnostic& diagnostic) {
                    BuildDiagnostic resolved = resolver->resolve(withCMakePath(diagnostic, base));
                    LockGuard lock(mutex);
                    if (!isCurrentLocked(id) || !wrapperTrustCurrentLocked()) {
                        return;
                    }
                    diagnostics.add(resolved);
                    auto fence = job->wrapper ? outputFenceLocked(id) : std::function<bool()>{};
                    emit([this, fence = std::move(fence), resolved = std::move(resolved)] {
                        if (fence && !fence())
                            return;
                        if (fence && callbacks.onDiagnosticFenced)
                            callbacks.onDiagnosticFenced(resolved, fence);
                        else if (callbacks.onDiagnostic)
                            callbacks.onDiagnostic(resolved);
                    });
                },
            .onFinished = [this,
                           id](const BuildResult& result) { post([this, id, result] { finishStep(id, result); }); },
        };
        BuildSessionOptions sessionOptions;
        sessionOptions.parser =
            configure ? OutputParserFactory(&makeCMakeOutputParser) : OutputParserFactory(&makeCompilerOutputParser);
        auto session = BuildSession::start(launcher, spec, true, std::move(sessionCallbacks), systemClock(),
                                           std::move(sessionOptions));
        if (!session) {
            finishJobLocked(std::move(session.error()));
            return;
        }
        job->session = std::move(*session);
        setStateLocked(BuildState::Running);
    }

    // Service thread: the session of the current step published its result.
    void finishStep(std::uint64_t id, const BuildResult& result) {
        std::unique_ptr<BuildSession> session;
        std::optional<project::CMakeProfile> configured;
        std::string folder;
        std::string indexBefore;
        std::optional<CMakeProgress> reading;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                return;
            }
            session = std::move(job->session);
            job->lastResult = result;
            if (job->current == CMakeStep::Configure && result.success) {
                configured = job->profile;
                folder = job->sourceDir;
                indexBefore = job->replyIndexBefore;
                job->readingReply = true;
                reading = refreshProgressLocked(true);
            }
        }
        deliverProgress(reading);
        session.reset(); // Joins the session's waiter, which has just returned from onFinished.
        std::shared_ptr<const project::CMakeProject> reply;
        std::optional<Error> replyError;
        if (configured) {
            reply = readReply(*configured, folder);
            if (!reply) {
                replyError = Error(ErrorCode::NotFound,
                                   std::format("cmake wrote no File API reply into {}", configured->buildDirectory));
            } else if (!indexBefore.empty() && documentKey(reply->replyIndex) == documentKey(indexBefore)) {
                // A reply from before this configure would show targets cmake did not just report.
                replyError = Error(ErrorCode::NotFound, std::format("cmake wrote no new File API reply into {}",
                                                                    configured->buildDirectory));
                reply = nullptr;
            }
        }
        LockGuard lock(mutex);
        if (!isCurrentLocked(id)) {
            return;
        }
        // readingReply stays set until the next step starts or the job ends (runNextStepLocked).
        if (job->current == CMakeStep::Configure && !reply && model) {
            stale = true; // The configure failed or was cancelled: the last model no longer matches.
        }
        if (!wrapperTrustCurrentLocked()) {
            finishJobLocked(Error(ErrorCode::Cancelled, "project trust changed during wrapper execution"));
            return;
        }
        if (job->wrapper && result.success) {
            stale = true;
            emitWrapperOutputLocked(BuildOutputKind::Status,
                                    "Build wrapper completed; reload the CMake project to refresh its targets.");
        }
        if (job->current == CMakeStep::Configure) {
            CMakeConfigureOutcome outcome{.profile = job->profile.name,
                                          .result = result,
                                          .error = replyError,
                                          .counts = diagnostics.counts(),
                                          .modelLoaded = reply != nullptr};
            if (reply) {
                model = reply;
                stale = false;
                configuredOnce = true;
                recomputeWatchedLocked();
                emitModel(reply);
            }
            publishConfigureLocked(std::move(outcome));
        }
        if (!result.success || replyError) {
            finishJobLocked(replyError);
            return;
        }
        job->steps.erase(job->steps.begin());
        runNextStepLocked();
    }

    void publishConfigureLocked(CMakeConfigureOutcome outcome) FROST_REQUIRES(mutex) {
        std::string line;
        if (outcome.error && !outcome.result.success && outcome.result.duration == IClock::Duration{}) {
            line = std::format("Configure not started: {}", outcome.error->message());
        } else if (outcome.result.cancelled) {
            line = std::format("Configure cancelled ({})", seconds(outcome.result.duration));
        } else if (outcome.error) {
            line = std::format("Configure failed: {}", outcome.error->message());
        } else {
            line = std::format("Configure {}: {}, {} ({})", outcome.result.success ? "finished" : "failed",
                               plural(outcome.counts.errors, "error"), plural(outcome.counts.warnings, "warning"),
                               seconds(outcome.result.duration));
        }
        emitOutput(true, BuildOutputKind::Status, std::move(line));
        lastConfigure = outcome;
        emit([this, outcome = std::move(outcome)] {
            if (callbacks.onConfigureFinished) {
                callbacks.onConfigureFinished(outcome);
            }
        });
    }

    // Ends the job: `error` says why a step did not start (or the reply could not be read).
    void finishJobLocked(std::optional<Error> error) FROST_REQUIRES(mutex) {
        Job& current = *job;
        const bool configureNotRun = current.kind == JobKind::Configure && !current.current;
        if (configureNotRun || (current.current == CMakeStep::Configure && error && !current.lastResult.success &&
                                current.lastResult.duration == IClock::Duration{})) {
            // A configure that never started still reports how it ended.
            CMakeConfigureOutcome outcome{.profile = current.profile.name,
                                          .result = current.lastResult,
                                          .error = error,
                                          .counts = diagnostics.counts(),
                                          .modelLoaded = false};
            publishConfigureLocked(std::move(outcome));
        }
        if (current.kind == JobKind::Build) {
            CMakeBuildOutcome outcome{.request = current.request,
                                      .result = current.lastResult,
                                      .error = std::move(error),
                                      .counts = diagnostics.counts(),
                                      .model = model,
                                      .sourceDirectory = current.sourceDir,
                                      .trustRevision = current.trustRevision};
            std::string line;
            const std::string_view action = toString(outcome.request.action);
            if (outcome.error) {
                line = std::format("{} failed: {}", action, outcome.error->message());
            } else if (outcome.result.cancelled) {
                line = std::format("{} cancelled ({})", action, seconds(outcome.result.duration));
            } else {
                line = std::format("{} {}: {}, {} ({})", action, outcome.result.success ? "succeeded" : "failed",
                                   plural(outcome.counts.errors, "error"), plural(outcome.counts.warnings, "warning"),
                                   seconds(outcome.result.duration));
            }
            auto fence = current.wrapper ? outputFenceLocked(current.id) : std::function<bool()>{};
            if (current.wrapper)
                emitWrapperOutputLocked(BuildOutputKind::Status, std::move(line));
            else
                emitOutput(false, BuildOutputKind::Status, std::move(line));
            lastBuild = outcome;
            emit([this, outcome = std::move(outcome), done = std::move(current.done), fence = std::move(fence)] {
                if (!fence || fence()) {
                    if (fence && callbacks.onFinishedFenced)
                        callbacks.onFinishedFenced(outcome, fence);
                    else if (callbacks.onFinished)
                        callbacks.onFinished(outcome);
                }
                if (done) {
                    done(outcome);
                }
            });
        }
        job.reset();
        setStateLocked(BuildState::Idle);
        if (reloadPending) {
            reloadPending = false;
            static_cast<void>(startJobLocked(JobKind::Configure, true, {}, {CMakeStep::Configure}));
        }
    }

    // Service thread: the debounce of a watched change ran out.
    void fireReload() {
        // Acquire before reading presets: a deferred reload must retain the
        // notification until project-code admission becomes available.
        bool reserved = false;
        if (options.reserveProjectCode && options.releaseProjectCode) {
            auto admission = options.reserveProjectCode();
            if (!admission) {
                LockGuard lock(mutex);
                reloadPending = true;
                return;
            }
            reserved = true;
        }
        struct Release {
            State* state;
            bool reserved;
            ~Release() {
                if (reserved)
                    state->options.releaseProjectCode();
            }
        } release{this, reserved};
        std::string folder;
        bool reloadPresets = false;
        {
            LockGuard lock(mutex);
            folder = sourceDir;
            reloadPresets = presetsTouched;
            presetsTouched = false;
        }
        if (folder.empty()) {
            return;
        }
        std::optional<project::CMakePresets> loaded;
        std::optional<std::string> problem;
        if (reloadPresets) {
            auto read = project::loadCMakePresets(fs, folder, options.presetsContext);
            if (read) {
                loaded = std::move(*read);
            } else {
                problem = read.error().describe();
            }
        }
        LockGuard lock(mutex);
        if (sourceDir != folder) {
            return;
        }
        if (reloadPresets) {
            ++profileGeneration;
            presets = std::move(loaded);
            presetsProblem = std::move(problem);
            recomputeProfilesLocked(false); // The configure below follows.
        }
        if (state != BuildState::Idle) {
            reloadPending = true;
            return;
        }
        reloadPending = false;
        static_cast<void>(startJobLocked(JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
};

std::unique_ptr<CMakeService> CMakeService::create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                   project::TrustStore& trustStore, CMakeServiceOptions options,
                                                   CMakeServiceCallbacks callbacks) {
    auto state = std::make_shared<State>(fs, launcher, trustStore, std::move(options), std::move(callbacks));
    State* raw = state.get();
    state->worker = std::thread([raw] { raw->workerLoop(); });
    return std::unique_ptr<CMakeService>(new CMakeService(std::move(state)));
}

Result<std::string> CMakeService::defaultStatePath() {
    auto trustPath = project::TrustStore::defaultStorePath();
    if (!trustPath) {
        return std::unexpected(std::move(trustPath.error()));
    }
    return io::joinPath(io::parentPath(*trustPath), kStateFileName);
}

CMakeService::CMakeService(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

CMakeService::~CMakeService() {
    std::unique_ptr<BuildSession> session;
    std::shared_ptr<State::AdmissionLease> admission;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->job) {
            admission = m_state->job->admission;
            session = std::move(m_state->job->session);
            m_state->job.reset();
        }
        m_state->state = BuildState::Idle;
        m_state->publishedState.store(BuildState::Idle);
    }
    if (session) {
        session->cancel();
        session.reset();
    }
    {
        LockGuard lock(m_state->queueMutex);
        m_state->stopping = true;
    }
    m_state->queueChanged.notify_all();
    if (m_state->worker.joinable()) {
        m_state->worker.join();
    }
    admission.reset();
    LockGuard lock(m_state->queueMutex);
    m_state->tasks.clear();
}

Result<void> CMakeService::openFolder(std::string_view sourceDir) {
    State& s = *m_state;
    auto folder = io::normalizePath(sourceDir);
    if (!folder) {
        return std::unexpected(std::move(folder.error()));
    }
    auto lists = io::joinPath(*folder, "CMakeLists.txt");
    if (!lists || !s.fs.exists(*lists).value_or(false)) {
        return fail(ErrorCode::NotFound, std::format("{} has no CMakeLists.txt", *folder));
    }
    std::optional<project::CMakePresets> presets;
    std::optional<std::string> problem;
    if (auto read = project::loadCMakePresets(s.fs, *folder, s.options.presetsContext)) {
        presets = std::move(*read);
    } else {
        problem = read.error().describe();
    }
    std::vector<project::CMakeProfileRecord> records;
    project::CMakeProfileSource recordSource{};
    {
        LockGuard lock(s.mutex);
        records = s.records;
        recordSource = s.recordSource;
    }
    std::vector<project::CMakeProfile> profiles = project::cmakeProfilesFor(*folder, presets, records, recordSource);
    const std::optional<std::size_t> active = State::chooseProfile(profiles, s.persistedProfile(*folder));

    LockGuard lock(s.mutex);
    if (s.state != BuildState::Idle) {
        return fail(ErrorCode::Busy, "a CMake job is in progress; cancel it before opening another folder");
    }
    s.sourceDir = std::move(*folder);
    ++s.profileGeneration;
    s.presets = std::move(presets);
    s.presetsProblem = std::move(problem);
    s.profiles = std::move(profiles);
    s.active = active;
    const std::uint64_t load = ++s.replyGeneration;
    s.model = nullptr; // Until loadReply publishes the existing reply, on the service thread.
    s.stale = false;
    s.configuredOnce = false;
    s.presetsTouched = false;
    s.reloadPending = false;
    s.diagnostics.clear();
    s.lastConfigure.reset();
    s.lastBuild.reset();
    s.recomputeWatchedLocked();
    s.emitDiagnosticsCleared();
    s.emitProfilesLocked();
    if (active) {
        ++s.replyLoadsPending;
        s.post([state = &s, load, profile = s.profiles[*active]] { state->loadReply(load, profile, false, nullptr); });
    } else {
        s.emitModel(nullptr);
    }
    if (s.options.configureOnOpen) {
        static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
    return {};
}

Result<void> CMakeService::closeFolder() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (s.state != BuildState::Idle) {
        return fail(ErrorCode::Busy, "a CMake job is in progress; cancel it before closing the folder");
    }
    if (s.sourceDir.empty()) {
        return {};
    }
    s.sourceDir.clear();
    ++s.profileGeneration;
    ++s.replyGeneration;
    s.presets.reset();
    s.presetsProblem.reset();
    s.profiles.clear();
    s.active.reset();
    s.model.reset();
    s.diagnostics.clear();
    s.lastConfigure.reset();
    s.lastBuild.reset();
    s.recomputeWatchedLocked();
    s.emitDiagnosticsCleared();
    s.emitProfilesLocked();
    s.emitModel(nullptr);
    return {};
}

std::string CMakeService::sourceDirectory() const {
    LockGuard lock(m_state->mutex);
    return m_state->sourceDir;
}

std::vector<project::CMakeProfile> CMakeService::profiles() const {
    LockGuard lock(m_state->mutex);
    return m_state->profiles;
}

std::optional<std::size_t> CMakeService::activeProfileIndex() const {
    LockGuard lock(m_state->mutex);
    return m_state->active;
}

std::optional<project::CMakeProfile> CMakeService::activeProfile() const {
    LockGuard lock(m_state->mutex);
    return m_state->activeProfileLocked();
}

Result<void> CMakeService::setActiveProfile(std::size_t index) {
    State& s = *m_state;
    // The admission covers the switch through its reply read on the service thread (loadReply).
    std::shared_ptr<State::AdmissionLease> lease;
    if (s.options.reserveProjectCode && s.options.releaseProjectCode) {
        auto reserved = s.options.reserveProjectCode();
        if (!reserved)
            return std::unexpected(std::move(reserved.error()));
        lease = std::make_shared<State::AdmissionLease>();
        lease->release = s.options.releaseProjectCode;
    }
    project::CMakeProfile profile;
    std::string folder;
    std::uint64_t generation = 0;
    {
        LockGuard lock(s.mutex);
        if (s.sourceDir.empty()) {
            return fail(ErrorCode::InvalidArgument, "no CMake folder is open");
        }
        if (s.state != BuildState::Idle) {
            return fail(ErrorCode::Busy, "the profile cannot change while a CMake job is in progress");
        }
        if (index >= s.profiles.size()) {
            return fail(ErrorCode::OutOfRange,
                        std::format("profile {} of {} does not exist", index, s.profiles.size()));
        }
        if (s.active == index) {
            return {};
        }
        profile = s.profiles[index];
        folder = s.sourceDir;
        generation = s.profileGeneration;
    }
    /* R0 follow-up 10: whether the profile needs its configure is decided here, with one status
     * call (the reply itself is read on the service thread, loadReply). A profile without a reply
     * directory starts its configure before this returns, so state() is Configuring at once and
     * clangd's trust watch never sees an idle service with the new, database-less build directory
     * (which restarted clangd once without a database and once more on the configure's). An
     * unreadable status counts as a reply: loadReply then decides on what it reads. */
    bool hasReply = true;
    if (auto replyDir = project::cmakeFileApiReplyDirectory(profile.buildDirectory)) {
        hasReply = s.fs.exists(*replyDir).value_or(true);
    }
    {
        LockGuard lock(s.mutex);
        if (s.sourceDir != folder || s.profileGeneration != generation || s.state != BuildState::Idle) {
            return fail(ErrorCode::Busy, "the folder changed while the profile was being switched");
        }
        if (s.active == index) {
            return {};
        }
        s.active = index;
        ++s.profileGeneration;
        const std::uint64_t load = ++s.replyGeneration;
        s.model = nullptr; // Until loadReply publishes this profile's reply.
        s.stale = false;
        s.configuredOnce = false;
        s.recomputeWatchedLocked();
        s.emitProfilesLocked();
        // Queued before the configure below starts, so its model (nullptr) precedes the job's events.
        ++s.replyLoadsPending;
        s.post([state = &s, load, profile, hasReply, lease = std::move(lease)] {
            state->loadReply(load, profile, hasReply, lease);
        });
        if (!hasReply && s.options.configureOnOpen) {
            static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
        }
    }
    return s.persist(folder, profile.name);
}

Result<void> CMakeService::setActiveProfile(std::string_view name) {
    std::optional<std::size_t> index;
    {
        LockGuard lock(m_state->mutex);
        for (std::size_t i = 0; i < m_state->profiles.size(); ++i) {
            if (m_state->profiles[i].name == name) {
                index = i;
            }
        }
    }
    if (!index) {
        return fail(ErrorCode::NotFound, std::format("the folder has no CMake profile '{}'", name));
    }
    return setActiveProfile(*index);
}

std::string CMakeService::activeBuildDirectory() const {
    LockGuard lock(m_state->mutex);
    auto profile = m_state->activeProfileLocked();
    return profile ? profile->buildDirectory : std::string();
}

std::uint64_t CMakeService::profileGeneration() const {
    LockGuard lock(m_state->mutex);
    return m_state->profileGeneration;
}

Result<void> CMakeService::setProfileRecords(std::vector<project::CMakeProfileRecord> records,
                                             project::CMakeProfileSource source) {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (s.state != BuildState::Idle) {
        return fail(ErrorCode::Busy, "the profiles cannot change while a CMake job is in progress");
    }
    s.records = std::move(records);
    s.recordSource = source;
    ++s.profileGeneration;
    if (s.sourceDir.empty()) {
        return {};
    }
    s.recomputeProfilesLocked(true);
    return {};
}

std::optional<std::string> CMakeService::presetsError() const {
    LockGuard lock(m_state->mutex);
    return m_state->presetsProblem;
}

std::vector<std::string> CMakeService::presetFiles() const {
    LockGuard lock(m_state->mutex);
    return m_state->presets ? m_state->presets->files : std::vector<std::string>{};
}

project::CMakePresetListing CMakeService::presetListing() const {
    LockGuard lock(m_state->mutex);
    return m_state->presets ? m_state->presets->listing() : project::CMakePresetListing{};
}

std::shared_ptr<const project::CMakeProject> CMakeService::model() const {
    LockGuard lock(m_state->mutex);
    return m_state->model;
}

std::vector<std::string> CMakeService::watchedFiles() const {
    LockGuard lock(m_state->mutex);
    return m_state->watchedPaths;
}

bool CMakeService::modelStale() const {
    LockGuard lock(m_state->mutex);
    return m_state->stale;
}

Result<void> CMakeService::configure() {
    LockGuard lock(m_state->mutex);
    return m_state->startJobLocked(State::JobKind::Configure, false, {}, {CMakeStep::Configure});
}

Result<void> CMakeService::configureOwned(std::uint64_t owner) {
    if (!owner)
        return fail(ErrorCode::InvalidArgument, "owned configure requires an owner");
    LockGuard lock(m_state->mutex);
    return m_state->startJobLocked(State::JobKind::Configure, false, {}, {CMakeStep::Configure}, {}, owner);
}

void CMakeService::admissionAvailable() {
    bool pending;
    {
        LockGuard lock(m_state->mutex);
        pending = m_state->reloadPending && m_state->state == BuildState::Idle;
    }
    if (pending) {
        auto* state = m_state.get();
        state->post([state] { state->fireReload(); });
    }
}

std::optional<CMakeConfigureOutcome> CMakeService::lastConfigure() const {
    LockGuard lock(m_state->mutex);
    return m_state->lastConfigure;
}

void CMakeService::setSaveAllHook(SaveAllHook hook) {
    LockGuard lock(m_state->mutex);
    m_state->saveAll = std::move(hook);
}

Result<void> CMakeService::setBuildWrappers(std::vector<CMakeBuildWrapper> wrappers) {
    LockGuard lock(m_state->mutex);
    if (m_state->state != BuildState::Idle)
        return fail(ErrorCode::Busy, "cannot change build wrappers while a CMake job is active");
    m_state->wrappers = std::move(wrappers);
    return {};
}

Result<void> CMakeService::build(BuildAction action, std::optional<std::size_t> target, CMakeBuildCompletion done,
                                 std::shared_ptr<const project::CMakeProject> expectedModel) {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (expectedModel && expectedModel != s.model)
        return fail(ErrorCode::Busy, "CMake targets changed; wait for the project view to refresh");
    CMakeBuildRequest request{.action = action, .target = target, .targetName = {}, .profile = {}};
    if (target) {
        if (!s.model || *target >= s.model->targets.size()) {
            return fail(ErrorCode::InvalidArgument, std::format("target {} does not exist", *target));
        }
        request.targetName = s.model->targets[*target].name;
    }
    std::vector<CMakeStep> steps;
    if (!s.model || s.stale) {
        steps.push_back(CMakeStep::Configure);
    }
    if (action == BuildAction::Clean || action == BuildAction::Rebuild) {
        steps.push_back(CMakeStep::Clean);
    }
    if (action == BuildAction::Build || action == BuildAction::Rebuild) {
        steps.push_back(CMakeStep::Build);
    }
    return s.startJobLocked(State::JobKind::Build, false, std::move(request), std::move(steps), std::move(done));
}

void CMakeService::cancel() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    s.reloadPending = false;
    switch (s.state) {
    case BuildState::Idle:
    case BuildState::Cancelling: return;
    case BuildState::Running:
        if (s.job && s.job->session) {
            s.setStateLocked(BuildState::Cancelling);
            s.job->session->cancel(); // Its result arrives through finishStep.
        }
        return;
    case BuildState::NeedsTrust:
    case BuildState::SavingDocuments:
    case BuildState::Starting:
        if (s.job) {
            s.job->lastResult.cancelled = true;
            s.finishJobLocked(std::nullopt);
        }
        return;
    }
}

Result<void> CMakeService::trust(std::string_view root) {
    State& s = *m_state;
    if (auto trusted = s.trustStore.trust(root); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    LockGuard lock(s.mutex);
    if (s.state == BuildState::NeedsTrust && s.job) {
        if (s.folderTrustedLocked()) {
            s.afterTrustLocked();
        }
        return {};
    }
    if (s.state == BuildState::Idle && !s.sourceDir.empty() && s.options.configureOnOpen && !s.configuredOnce &&
        s.folderTrustedLocked()) {
        static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
    return {};
}

void CMakeService::trustChanged() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (s.state == BuildState::NeedsTrust && s.job) {
        const bool all = std::ranges::all_of(s.job->untrusted,
                                             [&](const std::string& root) { return s.trustStore.isTrusted(root); });
        if (all && s.folderTrustedLocked()) {
            s.afterTrustLocked();
        }
        return;
    }
    if (s.state == BuildState::Idle && !s.sourceDir.empty() && s.options.configureOnOpen && !s.configuredOnce &&
        s.folderTrustedLocked()) {
        static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
}

BuildState CMakeService::state() const {
    LockGuard lock(m_state->mutex);
    return m_state->state;
}

bool CMakeService::modelLoading() const {
    LockGuard lock(m_state->mutex);
    return m_state->replyLoadsPending > 0;
}

CMakeProgress CMakeService::progress() const {
    LockGuard lock(m_state->mutex);
    return m_state->progressValue;
}

std::optional<CMakeStep> CMakeService::currentStep() const {
    LockGuard lock(m_state->mutex);
    return m_state->job ? m_state->job->current : std::nullopt;
}

std::vector<std::string> CMakeService::untrustedRoots() const {
    LockGuard lock(m_state->mutex);
    if (m_state->state != BuildState::NeedsTrust || !m_state->job) {
        return {};
    }
    return m_state->job->untrusted;
}

std::optional<CMakeBuildOutcome> CMakeService::lastOutcome() const {
    LockGuard lock(m_state->mutex);
    return m_state->lastBuild;
}

std::vector<BuildDiagnostic> CMakeService::diagnostics() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.all();
}

std::vector<DiagnosticFileGroup> CMakeService::diagnosticsByFile() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.byFile();
}

DiagnosticCounts CMakeService::diagnosticCounts() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.counts();
}

std::vector<BuildDiagnostic> CMakeService::diagnosticsForDocument(std::string_view path) const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.forDocument(path);
}

void CMakeService::filesChanged(const io::FileChangeBatch& batch) {
    State& s = *m_state;
    bool touched = false;
    {
        LockGuard lock(s.mutex);
        if (s.sourceDir.empty()) {
            return;
        }
        std::vector<std::string> presetKeys;
        if (s.presets) {
            for (const std::string& file : s.presets->files) {
                presetKeys.push_back(documentKey(file));
            }
        }
        for (const io::FileChange& change : batch.changes) {
            if (!touches(change, s.watched)) {
                continue;
            }
            touched = true;
            const std::string key = documentKey(change.path);
            if (isPresetsFileName(change.path) || (!change.oldPath.empty() && isPresetsFileName(change.oldPath)) ||
                std::ranges::find(presetKeys, key) != presetKeys.end() || change.kind == io::FileChangeKind::Rescan) {
                s.presetsTouched = true;
            }
        }
        if (touched) {
            s.stale = true;
        }
    }
    if (touched) {
        {
            LockGuard lock(s.queueMutex);
            s.reloadAt = std::chrono::steady_clock::now() + s.options.reloadDebounce;
        }
        s.queueChanged.notify_all();
    }
}

sched::Task<Result<void>> CMakeService::follow(io::IDirectoryWatch& watch, std::stop_token token) {
    for (;;) {
        auto batch = co_await watch.next(token);
        if (!batch) {
            co_return std::unexpected(batch.error());
        }
        filesChanged(*batch);
    }
}

bool CMakeService::waitForIdle(IClock::Duration timeout) const {
    const State& s = *m_state;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(s.queueMutex);
    for (;;) {
        if (s.tasks.empty() && !s.taskRunning && !s.reloadAt && s.publishedState.load() == BuildState::Idle) {
            return true;
        }
        if (s.queueChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return s.tasks.empty() && !s.taskRunning && !s.reloadAt && s.publishedState.load() == BuildState::Idle;
        }
    }
}

} // namespace frost::build
