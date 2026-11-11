// SPDX-License-Identifier: Apache-2.0
//
// Internal CMakeService state and helpers shared across cmake_service*.cpp.

#pragma once

#include "frost/build/cmake_service.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/value.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace frost::build {

inline constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;
inline constexpr std::string_view kStateHeader = "# FrostIDE CMake profiles v1";
inline constexpr std::string_view kStateFileName = "cmake-profiles.txt";
inline constexpr char kSeparator = kWindows ? '\\' : '/';

// "a, b, c".
[[nodiscard]] inline std::string joinStrings(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += out.empty() ? item : ", " + item;
    }
    return out;
}

[[nodiscard]] inline std::string seconds(IClock::Duration duration) {
    return std::format("{:.1f} s", std::chrono::duration<double>(duration).count());
}

[[nodiscard]] inline std::string plural(std::size_t count, std::string_view word) {
    return std::format("{} {}{}", count, word, count == 1 ? "" : "s");
}

// A command line for the status line: arguments with spaces are quoted.
[[nodiscard]] inline std::string commandLine(std::string_view executable, const std::vector<std::string>& arguments) {
    std::string text = std::string(io::fileName(executable));
    for (const std::string& argument : arguments) {
        text += ' ';
        text += argument.find(' ') != std::string::npos ? std::format("\"{}\"", argument) : argument;
    }
    return text;
}

// The persisted choices: "<source dir>\t<profile name>" lines under the header.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> readStateFile(io::IFileSystem& fs,
                                                                                    const std::string& path) {
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

[[nodiscard]] inline std::string fieldString(const settings::Record& record, std::string_view name) {
    const settings::FieldValue* value = settings::findField(record, name);
    return value != nullptr && std::holds_alternative<std::string>(*value) ? std::get<std::string>(*value)
                                                                           : std::string();
}

[[nodiscard]] inline std::vector<std::string> fieldList(const settings::Record& record, std::string_view name) {
    const settings::FieldValue* value = settings::findField(record, name);
    return value != nullptr && std::holds_alternative<settings::StringList>(*value)
               ? std::get<settings::StringList>(*value)
               : std::vector<std::string>{};
}

// Whether `change` concerns a file whose documentKey is in `watched` (file_watcher.hpp's rules for
// a consumer of single paths).
[[nodiscard]] inline bool touches(const io::FileChange& change, const std::vector<std::string>& watched) {
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
[[nodiscard]] inline Diagnostic withCMakePath(Diagnostic diagnostic, const std::string& base) {
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

[[nodiscard]] inline bool isPresetsFileName(std::string_view path) {
    const std::string_view name = io::fileName(path);
    return name == project::kCMakePresetsFile || name == project::kCMakeUserPresetsFile;
}

// The greatest index-*.json name in `buildDirectory`'s reply directory (the current reply), or "".
[[nodiscard]] inline std::string currentReplyIndex(io::IFileSystem& fs, std::string_view buildDirectory) {
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
[[nodiscard]] inline bool sameReply(const project::CMakeProfile& a, const project::CMakeProfile& b) {
    return documentKey(a.buildDirectory) == documentKey(b.buildDirectory) && a.generator == b.generator &&
           (!project::isMultiConfigGenerator(a.generator) || a.effectiveConfiguration() == b.effectiveConfiguration());
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
          callbacks(std::move(serviceCallbacks)), msvcCache(fileSystem, processLauncher, options.msvcCacheDirectory),
          records(options.profileRecords), wrappers(options.buildWrappers), recordSource(options.profileRecordSource) {}

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
    // `state`'s copy, stored with it under the lock and read without it (CMakeService::state, waitForIdle).
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

    template<class F>
    void emit(F&& call) {
        post(std::forward<F>(call));
    }

    bool isCurrentLocked(std::uint64_t id) const FROST_REQUIRES(mutex) { return job && job->id == id; }

    bool folderTrustedLocked() const FROST_REQUIRES(mutex) { return trustStore.isTrusted(sourceDir); }

    std::optional<project::CMakeProfile> activeProfileLocked() const FROST_REQUIRES(mutex) {
        if (!active || *active >= profiles.size()) {
            return std::nullopt;
        }
        return profiles[*active];
    }

    // Defined in cmake_service.cpp
    void workerLoop();
    void emitState(BuildState value);
    void emitOutput(bool configure, BuildOutputKind kind, std::string line);
    void emitModel(std::shared_ptr<const project::CMakeProject> value);
    void emitProfilesLocked() FROST_REQUIRES(mutex);
    void emitDiagnosticsCleared();
    void setStateLocked(BuildState value) FROST_REQUIRES(mutex);
    std::optional<CMakeProgress> refreshProgressLocked(bool direct = false) FROST_REQUIRES(mutex);
    void deliverProgress(const std::optional<CMakeProgress>& value) const;
    bool wrapperTrustCurrentLocked() const FROST_REQUIRES(mutex);
    std::function<bool()> outputFenceLocked(std::uint64_t id) FROST_REQUIRES(mutex);
    void emitWrapperOutputLocked(BuildOutputKind kind, std::string line) FROST_REQUIRES(mutex);

    // Defined in cmake_service_profiles.cpp
    void recomputeWatchedLocked() FROST_REQUIRES(mutex);
    static std::string replyConfiguration(const project::CMakeProfile& profile);
    std::shared_ptr<const project::CMakeProject> readReply(const project::CMakeProfile& profile,
                                                           const std::string& folder);
    bool replyOlderThanInputs(const project::CMakeProject& reply, const std::vector<std::string>& presetFiles);
    void loadReply(std::uint64_t generation, const project::CMakeProfile& profile, bool configureIfMissing,
                   const std::shared_ptr<AdmissionLease>& lease);
    std::optional<std::string> persistedProfile(const std::string& folder);
    Result<void> persist(const std::string& folder, const std::string& name);
    static std::optional<std::size_t> chooseProfile(const std::vector<project::CMakeProfile>& list,
                                                    const std::optional<std::string>& name);
    void recomputeProfilesLocked(bool configureIfMissing) FROST_REQUIRES(mutex);
    void fireReload();

    // Defined in cmake_service_jobs.cpp
    Result<void> startJobLocked(JobKind kind, bool automatic, CMakeBuildRequest request, std::vector<CMakeStep> steps,
                                CMakeBuildCompletion done = {}, std::uint64_t owner = 0) FROST_REQUIRES(mutex);
    void afterTrustLocked() FROST_REQUIRES(mutex);
    void saveDocuments(std::uint64_t id);
    void prepare(std::uint64_t id);
    project::CMakeEnvironment environmentFor(const Job& current, bool configure) const;
    void runNextStepLocked() FROST_REQUIRES(mutex);
    void finishStep(std::uint64_t id, const BuildResult& result);
    void publishConfigureLocked(CMakeConfigureOutcome outcome) FROST_REQUIRES(mutex);
    void finishJobLocked(std::optional<Error> error) FROST_REQUIRES(mutex);
};

} // namespace frost::build
