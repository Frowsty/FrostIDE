// SPDX-License-Identifier: Apache-2.0
//
// CMakeService (see frost/build/cmake_service.hpp): the job execution pipeline (trust, Save All,
// tool and MSVC environment discovery, configure/clean/build steps, and outcome publication).

#include "cmake_service_internal.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::build {

namespace {

#ifdef _WIN32
// Numbers the private mspdbsrv endpoints (_MSPDBSRV_ENDPOINT_) of cl.exe children.
std::atomic<std::uint64_t> g_pdbEndpointCounter{0}; // Constant-initialised: no global constructor.
#endif

} // namespace

/*
 * Starts a job for the active profile. An automatic job of an untrusted folder or an unusable
 * profile is dropped. Errors: InvalidArgument (no folder, unusable profile), Busy.
 */
Result<void> CMakeService::State::startJobLocked(JobKind kind, bool automatic, CMakeBuildRequest request,
                                                 std::vector<CMakeStep> steps, CMakeBuildCompletion done,
                                                 std::uint64_t owner) {
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
        admission->release = [release = std::move(release), owner, admissionChild] { release(owner, admissionChild); };
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

void CMakeService::State::afterTrustLocked() {
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

void CMakeService::State::saveDocuments(std::uint64_t id) {
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
void CMakeService::State::prepare(std::uint64_t id) {
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
        if (!job->debuggerEndpoint.empty() && !job->debuggerSaved) {
            job->debuggerSaved = true;
            setStateLocked(BuildState::SavingDocuments);
            post([this, id] { saveDocuments(id); });
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
                                     .pathVariable =
                                         options.pathVariable.empty() ? processPathVariable() : options.pathVariable,
                                     .sdkDirectory = options.sdkDirectory};
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
                    Error(ErrorCode::Unsupported,
                          std::format("{} is CMake {}; FrostIDE needs CMake 3.21 or later", found->cmake, *version)));
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
            const VcvarsSelection selection{.toolsetVersion = options.msvcToolsetVersion,
                                            .sdkVersion = options.windowsSdkVersion};
            auto environment = msvcCache.get(*chosen, architecture, selection);
            if (!environment) {
                LockGuard lock(mutex);
                if (isCurrentLocked(id)) {
                    finishJobLocked(
                        std::move(environment.error()).withContext("while setting up the MSVC environment"));
                }
                return;
            }
            msvc = std::move(environment->changes);
            // The toolset and SDK vcvarsall actually set up (SLN-14), not the ones asked for.
            std::string note =
                std::format("Using the MSVC environment of {} {} ({}, MSVC toolset {}, Windows SDK {})",
                            chosen->displayName, chosen->installationVersion, architecture,
                            environment->toolsetVersion.empty() ? "unknown" : environment->toolsetVersion,
                            environment->sdkVersion.empty() ? "unknown" : environment->sdkVersion);
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

project::CMakeEnvironment CMakeService::State::environmentFor(const Job& current, bool configure) const {
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

void CMakeService::State::runNextStepLocked() {
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
        if (!job->debuggerEndpoint.empty()) {
            spec.arguments.emplace_back("--debugger");
            spec.arguments.emplace_back("--debugger-pipe");
            spec.arguments.push_back(job->debuggerEndpoint);
        }
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
        const std::string target = job->request.targetName.empty() ? wrapper.defaultTarget : job->request.targetName;
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
        .onFinished = [this, id](const BuildResult& result) { post([this, id, result] { finishStep(id, result); }); },
    };
    BuildSessionOptions sessionOptions;
    sessionOptions.parser =
        configure ? OutputParserFactory(&makeCMakeOutputParser) : OutputParserFactory(&makeCompilerOutputParser);
    const auto launchTrust = trustStore.snapshot(job->sourceDir);
    if (!launchTrust.trusted || launchTrust.revision != job->trustRevision) {
        finishJobLocked(Error(ErrorCode::Cancelled, "project trust changed before CMake process launch"));
        return;
    }
    auto session = BuildSession::start(launcher, spec, true, std::move(sessionCallbacks), systemClock(),
                                       std::move(sessionOptions));
    if (!session) {
        finishJobLocked(std::move(session.error()));
        return;
    }
    job->session = std::move(*session);
    if (configure && job->debuggerReady) emit(std::move(job->debuggerReady));
    setStateLocked(BuildState::Running);
}

// Service thread: the session of the current step published its result.
void CMakeService::State::finishStep(std::uint64_t id, const BuildResult& result) {
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
            replyError = Error(ErrorCode::NotFound,
                               std::format("cmake wrote no new File API reply into {}", configured->buildDirectory));
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

void CMakeService::State::publishConfigureLocked(CMakeConfigureOutcome outcome) {
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
void CMakeService::State::finishJobLocked(std::optional<Error> error) {
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

} // namespace frost::build
