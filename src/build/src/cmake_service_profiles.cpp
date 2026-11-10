// SPDX-License-Identifier: Apache-2.0
//
// CMakeService (see frost/build/cmake_service.hpp): folder lifecycle, profile selection and
// persistence, CMake File API reply loading, and watched-file reconfigure debounce.

#include "cmake_service_internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::build {

void CMakeService::State::recomputeWatchedLocked() {
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
std::string CMakeService::State::replyConfiguration(const project::CMakeProfile& profile) {
    return project::isMultiConfigGenerator(profile.generator) ? profile.effectiveConfiguration() : std::string();
}

/*
 * Reads `profile`'s existing reply (blocking); nullptr when there is none, it does not parse,
 * or its codemodel names a source directory other than `folder` (header comment, "Stale
 * replies": a build directory configured from another tree is not this folder's model).
 */
std::shared_ptr<const project::CMakeProject> CMakeService::State::readReply(const project::CMakeProfile& profile,
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
bool CMakeService::State::replyOlderThanInputs(const project::CMakeProject& reply,
                                               const std::vector<std::string>& presetFiles) {
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
void CMakeService::State::loadReply(std::uint64_t generation, const project::CMakeProfile& profile,
                                    bool configureIfMissing, const std::shared_ptr<AdmissionLease>& lease) {
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

std::optional<std::string> CMakeService::State::persistedProfile(const std::string& folder) {
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

Result<void> CMakeService::State::persist(const std::string& folder, const std::string& name) {
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
std::optional<std::size_t> CMakeService::State::chooseProfile(const std::vector<project::CMakeProfile>& list,
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
void CMakeService::State::recomputeProfilesLocked(bool configureIfMissing) {
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

// Service thread: the debounce of a watched change ran out.
void CMakeService::State::fireReload() {
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

} // namespace frost::build
