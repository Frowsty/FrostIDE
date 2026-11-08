// SPDX-License-Identifier: Apache-2.0
//
// CMakeService: an opened CMake folder, CLion-like (ADR 0046). It holds the folder's profiles
// (presets or settings), configures in the background once the folder is trusted, reads the File
// API reply into the CMake project model, reconfigures when a CMake input changes, and builds with
// `cmake --build` (Build, Rebuild, Clean) with streamed output and parsed diagnostics. Qt-free.
//
/*
 * Shape. It mirrors BuildService (build_service.hpp) so the W1 build tool windows can bind to it
 * through a small adapter: the same BuildState pipeline, BuildOutputKind lines, BuildDiagnostic
 * index and Save-All hook. A CMake folder's "configurations" are its profiles (profiles(),
 * activeProfileIndex(), setActiveProfile()) and its "projects" are the model's targets
 * (build(action, targetIndex)). The CMake tool window binds to the configure events
 * (onConfigureStarted, onConfigureOutput, onConfigureFinished) and to onProfilesChanged.
 *
 * Opening (openFolder). Reads the presets (cmake_presets.hpp) and builds the profiles
 * (cmake_profile.hpp) from them or from CMakeServiceOptions::profileRecords; the active profile is
 * the persisted one (stateFilePath; a persisted configure preset name picks that preset's first
 * profile, so a folder keeps its choice when one of its presets gains build presets) or the first
 * usable one. The active profile's existing File
 * API reply, if any, is read next on the service's thread (it is several MB of JSON for a large
 * project, too slow for the caller's GUI thread: the R0 stall criterion) and published through
 * onModelChanged, so the target tree shows before anything runs (restricted mode, PLAN.md section
 * 3.5). If the folder is trusted and configureOnOpen is set, a configure starts in the background,
 * after that read.
 *
 * Trust. Configure runs the project's CMake code and a build its build steps, so both pass the
 * gate first: the folder (the source directory) must be trusted (TrustStore); its profiles' build
 * directories are covered by that trust. An explicit configure() or build() of an untrusted folder
 * waits in NeedsTrust (onNeedsTrust names the folder) until trust() or cancel(); an automatic
 * reconfigure (file change, open) of an untrusted folder does nothing. Nothing is started before
 * trust: no cmake, no vcvarsall, no vswhere.
 *
 * A job (configure or build) runs on the service's thread through
 *
 *     Idle -> [NeedsTrust ->] [SavingDocuments (builds) ->] Starting -> Running [-> Cancelling] -> Idle
 *
 *   - Starting: finds cmake and ninja once (cmake_toolchain.hpp; the version must be 3.21 or
 *     later), on Windows captures the MSVC environment once per instance and architecture for
 *     generators other than Visual Studio ones (the profile's external architecture, else
 *     msvcArchitecture), and for a configure writes the File API query into the build directory.
 *   - Running: a build job runs its steps one after another: Configure (when there is no model or
 *     a CMake input changed since the last configure), then Clean (Clean, Rebuild) and Build
 *     (Build, Rebuild). Rebuild is Clean then Build. A failed step ends the job.
 * One job at a time: configure() and build() during a job fail with Busy; an automatic reconfigure
 * requested during a job runs after it.
 *
 * Child processes. cmake starts through IProcessLauncher at its absolute path, in the source
 * directory (configure) or the build directory (builds), with the environment: this process's,
 * then the MSVC changes, then the profile's (configure) or the build preset's (builds), then
 * VSLANG=1033 (English cl messages, so the parser's category words match) and
 * NoDefaultCurrentDirectoryInExePath removed (trusted user-script children, ADR 0011 E2).
 * cancel() ends the whole tree (BuildSession).
 *
 * Diagnostics. Each job clears the previous job's. Configure output is parsed for CMake's message
 * blocks ("CMake Error at CMakeLists.txt:12"), resolved against the source directory; build output
 * with the compiler parser (cl, clang-cl, MSBuild's format, then GCC/Clang's GNU format), resolved
 * against the build directory, where Ninja runs the compilers.
 *
 * Stale replies (CMK-1). A reply is the model only while it describes what the profile would
 * configure now:
 *   - a reply whose codemodel names another source directory (a build directory configured from
 *     another tree, or copied) is not read as this folder's model: the profile counts as never
 *     configured;
 *   - the reply read after a successful configure must be a new one (its index file is not the
 *     index that was current before cmake ran); otherwise the configure fails with "cmake wrote no
 *     new File API reply" and nothing is published;
 *   - a configure that fails or is cancelled keeps the last model but marks it stale (modelStale()),
 *     so the next build configures first;
 *   - an existing reply read at open or on a profile switch is stale when one of its project inputs
 *     (or a presets file) was modified after its index file was written (the inputs changed while
 *     FrostIDE was not watching);
 *   - when the profiles are recomputed (a presets file changed, setProfileRecords) and the active
 *     profile now has another build directory, generator or configuration, or is gone, the old
 *     model is dropped and the new active profile's reply is read, as on a switch.
 *
 * Reconfigure on change. filesChanged() (the workbench's fan-out of its recursive watch, ADR 0020)
 * or follow() (pumping one watch) receive file-watcher batches. A change to a watched file (the
 * model's project inputs, the presets files read, and the top-level CMakeLists.txt,
 * CMakePresets.json and CMakeUserPresets.json) marks the model stale and schedules a reconfigure
 * after reloadDebounce of quiet; a change to a presets file reloads the profiles first (the active
 * profile is kept by name).
 *
 * Threads. Every member may be called from any thread unless noted. Callbacks run on the service's
 * thread, one at a time, in event order; they may call every member except the destructor and
 * waitForIdle. Queries return snapshots. Lifetime: the file system, launcher and trust store
 * outlive the service; the destructor cancels a running job and joins the thread.
 */
#pragma once

#include "frost/build/build_diagnostics.hpp"
#include "frost/build/build_service.hpp"
#include "frost/build/build_session.hpp"
#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_driver.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/cmake_presets.hpp"
#include "frost/project/cmake_profile.hpp"
#include "frost/sched/task.hpp"
#include "frost/settings/value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io

namespace frost::project {
class TrustStore;
} // namespace frost::project

namespace frost::build {

// What a job does at the moment.
enum class CMakeStep : std::uint8_t { Configure, Clean, Build };

// "Configure", "Clean" or "Build".
[[nodiscard]] std::string_view toString(CMakeStep step) noexcept;

/*
 * What the folder is doing, for the progress display (CMK-15): the CMake tool window and the
 * status bar show it with an elapsed time and a Stop button. CMake prints no percentage while it
 * configures, so the phases are what can be told apart truthfully from the job's state and
 * cmake's own output: Configuring until cmake prints "-- Configuring done", then Generating until
 * it exits, then ReadingModel while the File API reply is read (also for the reply read of an
 * open or a profile switch).
 */
enum class CMakeActivity : std::uint8_t {
    Idle,
    ReadingModel,    // A File API reply is being read (service thread).
    WaitingForTrust, // NeedsTrust.
    Preparing,       // Save All, finding cmake and ninja, capturing the MSVC environment.
    Configuring,     // cmake's configure step.
    Generating,      // cmake's generate step (after "-- Configuring done").
    Cleaning,        // cmake --build --target clean.
    Building,        // cmake --build.
    Cancelling,      // cancel() was called; the process tree is ending.
};

// "Idle", "Reading the CMake project model", "Waiting for trust", "Preparing", "Configuring",
// "Generating", "Cleaning", "Building" or "Cancelling".
[[nodiscard]] std::string_view toString(CMakeActivity activity) noexcept;

struct CMakeProgress {
    CMakeActivity activity = CMakeActivity::Idle;
    std::string profile{}; // The profile it concerns; "" when Idle.
    // When `activity` began (steady clock), for the elapsed time.
    std::chrono::steady_clock::time_point since{};
    // Whether cancel() can stop it now (a configure or build that is preparing or running).
    bool cancellable = false;
    // Grows with each change; a receiver keeps the value with the greatest one (onProgress may
    // deliver an older value after a newer one).
    std::uint64_t sequence = 0;

    friend bool operator==(const CMakeProgress&, const CMakeProgress&) = default;
};

// What a build job builds.
struct CMakeBuildRequest {
    BuildAction action = BuildAction::Build;
    std::optional<std::size_t> target{}; // Index into the model's targets; empty: the profile's default (all).
    std::string targetName{};            // Its name, or "".
    std::string profile{};               // The profile's name.
};

// An explicitly configured personal command for one project root and profile.
struct CMakeBuildWrapper {
    std::string root{};
    std::string profile{};
    std::string executable{};
    std::string defaultTarget{};
    std::vector<std::string> buildArguments{};
    std::vector<std::string> cleanArguments{};
    std::vector<std::string> rebuildArguments{};
};

// Decodes personal settings records; malformed records are retained for validation at use.
[[nodiscard]] std::vector<CMakeBuildWrapper> cmakeBuildWrappers(const settings::RecordList& records);

struct CMakeConfigureOutcome {
    std::string profile{};
    BuildResult result{};         // cmake's run; success = false when it did not start.
    std::optional<Error> error{}; // Why it did not start, or why the reply could not be read.
    DiagnosticCounts counts{};
    bool modelLoaded = false; // A new model was read from the reply.

    [[nodiscard]] bool succeeded() const noexcept { return result.success && !error; }
};

struct CMakeBuildOutcome {
    CMakeBuildRequest request{};
    BuildResult result{};         // The last step that ran.
    std::optional<Error> error{}; // Why a step did not start (Save All failed, no cmake, a launch error).
    DiagnosticCounts counts{};    // Over the job's diagnostics.
    // The model at completion, for resolving this build's target even after a profile switch.
    std::shared_ptr<const project::CMakeProject> model{};
    std::string sourceDirectory{};
    std::uint64_t trustRevision = 0;

    [[nodiscard]] bool succeeded() const noexcept { return result.success && !error; }
};

// Called once for the accepted build on the service thread, after the shared onFinished callback.
using CMakeBuildCompletion = std::function<void(const CMakeBuildOutcome&)>;

// Every callback is optional and runs on the service's thread (header comment).
struct CMakeServiceCallbacks {
    std::function<void(BuildState state)> onStateChanged{};
    std::function<void(const std::vector<std::string>& untrustedRoots)> onNeedsTrust{};
    // `cmake --build` output, and the service's status lines for builds.
    std::function<void(BuildOutputKind kind, std::string_view line)> onOutput{};
    // Configure output and its status lines (the CMake tool window).
    std::function<void(BuildOutputKind kind, std::string_view line)> onConfigureOutput{};
    std::function<void(const BuildDiagnostic& diagnostic)> onDiagnostic{};
    std::function<void()> onDiagnosticsCleared{};
    std::function<void(const std::string& profile)> onConfigureStarted{};
    std::function<void(const CMakeConfigureOutcome& outcome)> onConfigureFinished{};
    // A new model (a reply was read), or nullptr (the folder closed, or the profile has no reply).
    std::function<void(std::shared_ptr<const project::CMakeProject> model)> onModelChanged{};
    std::function<void(const std::vector<project::CMakeProfile>& profiles, std::optional<std::size_t> active)>
        onProfilesChanged{};
    std::function<void(const CMakeBuildOutcome& outcome)> onFinished{}; // A build job ended.
    std::function<void(BuildOutputKind, std::string_view, std::function<bool()>)> onOutputFenced{};
    std::function<void(const BuildDiagnostic&, std::function<bool()>)> onDiagnosticFenced{};
    std::function<void(const CMakeBuildOutcome&, std::function<bool()>)> onFinishedFenced{};
    // The activity changed (CMakeProgress); in event order with the other callbacks.
    std::function<void(const CMakeProgress& progress)> onProgress{};
};

struct CMakeServiceOptions {
    // Optional thread-safe shared admission lease. Both callbacks must be supplied together;
    // the lease covers each configure/build and profile switch through completion.
    std::function<Result<void>()> reserveProjectCode;
    std::function<void()> releaseProjectCode;
    std::function<Result<std::uint64_t>(std::uint64_t)> reserveOwnedProjectCode;
    std::function<void(std::uint64_t, std::uint64_t)> releaseOwnedProjectCode;
    std::string cmakePath{};            // tools.cmake.path; "" detects (cmake_toolchain.hpp).
    std::string ninjaPath{};            // tools.ninja.path; "" detects.
    std::string pathVariable{};         // The PATH searched for tools; "" uses this process's.
    bool useVisualStudio = true;        // Windows: vswhere for the bundled tools and the MSVC environment.
    bool captureMsvcEnvironment = true; // Windows: vcvarsall for non-Visual Studio generators.
    std::string vsInstanceId{};         // The instance for vcvarsall; "" takes the newest with C++ tools.
    std::string msvcArchitecture = "x64";
    std::string msvcCacheDirectory{}; // MsvcEnvironmentCache's directory; "" caches in memory only.
    std::string stateFilePath{};      // The persisted active profiles (absolute); "" persists nothing.
    // The `cmake.profiles` setting's records (cmakeProfileRecords) and the scope they came from.
    std::vector<project::CMakeProfileRecord> profileRecords{};
    std::vector<CMakeBuildWrapper> buildWrappers{};
    project::CMakeProfileSource profileRecordSource = project::CMakeProfileSource::ApplicationSettings;
    bool configureOnOpen = true; // Configure in the background when a trusted folder opens or becomes trusted.
    std::chrono::milliseconds reloadDebounce{500};
    unsigned buildJobs = 0; // -j for builds; 0 leaves it to the build tool.
    project::CMakePresetsContext presetsContext{};
    // The instances vswhere reports; empty uses listVsInstances. Called once, at the first job.
    std::function<Result<std::vector<VsInstance>>()> listInstances{};
};

// The `cmake.profiles` setting's records (frost/settings/keys.hpp) as profile records; records
// without a name are skipped.
[[nodiscard]] std::vector<project::CMakeProfileRecord> cmakeProfileRecords(const settings::RecordList& records);

class CMakeService {
public:
    [[nodiscard]] static std::unique_ptr<CMakeService> create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                              project::TrustStore& trustStore,
                                                              CMakeServiceOptions options = {},
                                                              CMakeServiceCallbacks callbacks = {});

    // "cmake-profiles.txt" in the trust store's directory. Errors: those of TrustStore::defaultStorePath.
    [[nodiscard]] static Result<std::string> defaultStatePath();

    ~CMakeService();
    CMakeService(const CMakeService&) = delete ("owns a thread and a job");
    CMakeService& operator=(const CMakeService&) = delete ("owns a thread and a job");

    /*
     * Opens the CMake folder `sourceDir` (absolute, holding CMakeLists.txt), as the header comment
     * says. Reads the presets (blocking, small files). The active profile's existing reply is read
     * on the service's thread: model() is nullptr until onModelChanged publishes it (waitForIdle
     * waits for that too). A presets file that does not parse leaves the settings profiles and is
     * reported by presetsError(). Errors: InvalidArgument (not absolute), NotFound (no
     * CMakeLists.txt), Busy (a job is running).
     */
    [[nodiscard]] Result<void> openFolder(std::string_view sourceDir);
    // Closes the folder and clears the model and diagnostics. Errors: Busy.
    [[nodiscard]] Result<void> closeFolder();
    [[nodiscard]] std::string sourceDirectory() const; // "" without a folder.

    [[nodiscard]] std::vector<project::CMakeProfile> profiles() const;
    [[nodiscard]] std::optional<std::size_t> activeProfileIndex() const;
    [[nodiscard]] std::optional<project::CMakeProfile> activeProfile() const;
    /*
     * Makes profiles()[index] active, persists it and clears the model; that profile's existing
     * reply is then read on the service's thread and published through onModelChanged (nullptr
     * without one). When the folder is trusted and configureOnOpen is set, a profile without a
     * reply directory starts its configure before this returns (state() is Configuring), and one
     * whose reply does not parse configures once the read finds that out. Blocking: one status
     * call on the reply directory. Errors: InvalidArgument (no folder), OutOfRange, Busy
     * (including a folder or profile change by another thread during the call), the state file's
     * errors.
     */
    [[nodiscard]] Result<void> setActiveProfile(std::size_t index);
    [[nodiscard]] Result<void> setActiveProfile(std::string_view name); // NotFound for an unknown name.
    // The active profile's build directory (where compile_commands.json is written); "" without one.
    [[nodiscard]] std::string activeBuildDirectory() const;
    // Changes when the folder or active profile is replaced, including a presets reload.
    [[nodiscard]] std::uint64_t profileGeneration() const;
    // Replaces the settings profiles (the `cmake.profiles` setting changed) and recomputes the
    // profiles, keeping the active one by name. Errors: Busy.
    [[nodiscard]] Result<void> setProfileRecords(std::vector<project::CMakeProfileRecord> records,
                                                 project::CMakeProfileSource source);
    // Why the presets could not be read, if they could not.
    [[nodiscard]] std::optional<std::string> presetsError() const;
    // The presets files read (CMakePresets::files, absolute); empty without presets.
    [[nodiscard]] std::vector<std::string> presetFiles() const;
    // What `cmake --list-presets=all` lists for the folder (CMakePresets::listing); empty without
    // presets or when they could not be read.
    [[nodiscard]] project::CMakePresetListing presetListing() const;

    // The current model; nullptr before the first reply.
    [[nodiscard]] std::shared_ptr<const project::CMakeProject> model() const;
    // The files whose change triggers a reconfigure (header comment), absolute.
    [[nodiscard]] std::vector<std::string> watchedFiles() const;
    // Whether a CMake input changed since the model was read.
    [[nodiscard]] bool modelStale() const;

    // Reload: configures the active profile in the background. Errors: InvalidArgument (no folder,
    // an unusable profile: CMakeProfile::problem), Busy.
    [[nodiscard]] Result<void> configure();
    [[nodiscard]] Result<void> configureOwned(std::uint64_t owner);
    // Retry a deferred automatic configure after an external admission lease is released.
    void admissionAvailable();
    [[nodiscard]] std::optional<CMakeConfigureOutcome> lastConfigure() const;

    // The UI's Save All before builds (BuildService::setSaveAllHook).
    void setSaveAllHook(SaveAllHook hook);
    // Replaces personal wrappers while idle; an active job keeps its admitted command.
    [[nodiscard]] Result<void> setBuildWrappers(std::vector<CMakeBuildWrapper> wrappers);
    /*
     * Starts `action` for target `target` (index into model()->targets) or for the profile's default
     * targets. On acceptance, `done` receives this job's outcome and completion-time model once on
     * the service thread; no callback is made when the request is refused or the service is destroyed.
     * Errors: InvalidArgument (no folder, an unusable profile, `target` out of range or
     * without a model), Busy (including when a supplied `expectedModel` is no longer current).
     */
    [[nodiscard]] Result<void> build(BuildAction action, std::optional<std::size_t> target = std::nullopt,
                                     CMakeBuildCompletion done = {},
                                     std::shared_ptr<const project::CMakeProject> expectedModel = {});
    // Cancels the job in progress (BuildService::cancel). Idempotent.
    void cancel();
    // Trusts `root` and lets a job waiting in NeedsTrust go on; once the folder is trusted and has no
    // configured model yet, configures it (configureOnOpen). Errors: those of TrustStore::trust.
    [[nodiscard]] Result<void> trust(std::string_view root);
    /*
     * The trust store changed by another route (Settings, another instance, a revocation): what
     * trust() does after its grant, and nothing else. It never writes the store, so it can neither
     * re-grant a root revoked meanwhile nor save a folder covered by a trusted parent as a root of
     * its own (R0 follow-up 15). A job waiting in NeedsTrust goes on when the folder and every root
     * it waits for are trusted now; an idle, trusted folder without a configured model configures
     * (configureOnOpen). Any thread; reads the store once per root.
     */
    void trustChanged();

    [[nodiscard]] BuildState state() const;
    /*
     * Whether a reply read that openFolder or setActiveProfile queued has not run yet: model() and
     * state() may still change without a job (the read publishes the model, and a profile switch's
     * read may start a configure). It drops under the lock that publishes that model or starts that
     * configure, so a caller that sees false and then state() Idle sees the switch's final state. Callers
     * that react to the active build directory (clangd's restart) treat it like a running job.
     */
    [[nodiscard]] bool modelLoading() const;
    // The current activity (CMakeProgress); onProgress reports each change.
    [[nodiscard]] CMakeProgress progress() const;
    [[nodiscard]] std::optional<CMakeStep> currentStep() const;
    [[nodiscard]] std::vector<std::string> untrustedRoots() const;
    [[nodiscard]] std::optional<CMakeBuildOutcome> lastOutcome() const;
    [[nodiscard]] std::vector<BuildDiagnostic> diagnostics() const;
    [[nodiscard]] std::vector<DiagnosticFileGroup> diagnosticsByFile() const;
    [[nodiscard]] DiagnosticCounts diagnosticCounts() const;
    [[nodiscard]] std::vector<BuildDiagnostic> diagnosticsForDocument(std::string_view path) const;

    // A file-watcher batch (header comment, "Reconfigure on change"); unrelated paths are ignored.
    void filesChanged(const io::FileChangeBatch& batch);
    /*
     * Pumps `watch` into filesChanged() until it ends; completes with the watch's error (Cancelled
     * through `token`). Precondition: the service and the watch outlive the task.
     */
    [[nodiscard]] sched::Task<Result<void>> follow(io::IDirectoryWatch& watch, std::stop_token token = {});

    // Blocks until no job runs, no reconfigure is pending and every queued callback has run, or
    // `timeout` passes; returns whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(IClock::Duration timeout) const;

    struct State; // Shared with the service's thread and the sessions' callbacks.

private:
    explicit CMakeService(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::build
