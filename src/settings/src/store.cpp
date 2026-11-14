// SPDX-License-Identifier: Apache-2.0
//
// SettingsStore and SettingsSnapshot (store.hpp): layer loading, resolution with the trust rule,
// byte-exact writes, live reload from watcher batches, validation on save, and the stand-in actions.

#include "frost/settings/store.hpp"

#include "frost/core/assert.hpp"

#include "validation_internal.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

namespace frost::settings {

namespace detail {

struct ListenerTable {
    Mutex mutex;
    std::uint64_t nextId FROST_GUARDED_BY(mutex) = 1;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const SettingsListener>>> listeners FROST_GUARDED_BY(mutex);
};

} // namespace detail

namespace {

constexpr std::array<Scope, 3> kFileScopes = {Scope::Application, Scope::ProjectShared, Scope::ProjectPersonal};

[[nodiscard]] constexpr std::optional<std::size_t> layerIndex(Scope scope) noexcept {
    switch (scope) {
    case Scope::Application: return 0;
    case Scope::ProjectShared: return 1;
    case Scope::ProjectPersonal: return 2;
    case Scope::Default:
    case Scope::FileLevel: break;
    }
    return std::nullopt;
}

// True when `ancestor` is a proper ancestor directory of `path` (either separator).
[[nodiscard]] bool isAncestor(std::string_view ancestor, std::string_view path) noexcept {
    if (ancestor.empty() || path.size() <= ancestor.size() || !path.starts_with(ancestor)) {
        return false;
    }
    const char last = ancestor.back();
    if (last == '/' || last == '\\') {
        return true; // A root such as "/" or "C:\".
    }
    const char next = path[ancestor.size()];
    return next == '/' || next == '\\';
}

// file_watcher.hpp's rule for a consumer interested in the single path `target`.
[[nodiscard]] bool touches(const io::FileChange& change, std::string_view target) noexcept {
    if (change.path == target) {
        return true;
    }
    if (change.kind == io::FileChangeKind::Renamed &&
        (change.oldPath == target || isAncestor(change.oldPath, target))) {
        return true;
    }
    if (!isAncestor(change.path, target)) {
        return false;
    }
    switch (change.kind) {
    case io::FileChangeKind::Created:
    case io::FileChangeKind::Removed:
    case io::FileChangeKind::Rescan:
    case io::FileChangeKind::Renamed: return true;
    case io::FileChangeKind::Modified: return change.type.has_value() && *change.type != io::FileType::Directory;
    }
    return false;
}

[[nodiscard]] Result<void> checkFileScope(Scope scope) {
    if (!layerIndex(scope)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} settings have no file FrostIDE writes", toString(scope)));
    }
    return {};
}

} // namespace

// --- Subscription ---

Subscription::Subscription(Subscription&& other) noexcept
    : m_table(std::move(other.m_table)), m_id(std::exchange(other.m_id, 0)) {}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        m_table = std::move(other.m_table);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

void Subscription::reset() noexcept {
    if (const auto table = m_table.lock()) {
        const LockGuard lock(table->mutex);
        std::erase_if(table->listeners, [&](const auto& entry) { return entry.first == m_id; });
    }
    m_table.reset();
    m_id = 0;
}

// --- SettingsSnapshot ---

SettingsSnapshot::SettingsSnapshot(const Registry& registry, std::vector<Entry> entries,
                                   std::vector<Diagnostic> diagnostics, std::vector<std::string> ignoredUntilTrusted,
                                   bool trusted, std::uint64_t generation)
    : m_registry(&registry), m_entries(std::move(entries)), m_diagnostics(std::move(diagnostics)),
      m_ignored(std::move(ignoredUntilTrusted)), m_trusted(trusted), m_generation(generation) {
    FROST_PRE(m_entries.size() == registry.size());
}

const Value& SettingsSnapshot::value(std::string_view key) const {
    const auto index = m_registry->indexOf(key);
    FROST_PRE(index.has_value()); // Only registered keys are read.
    return m_entries[*index].value;
}

Scope SettingsSnapshot::source(std::string_view key) const {
    const auto index = m_registry->indexOf(key);
    FROST_PRE(index.has_value());
    return m_entries[*index].source;
}

const SettingsSnapshot::Entry& SettingsSnapshot::entry(std::size_t index) const {
    FROST_PRE(index < m_entries.size());
    return m_entries[index];
}

// --- SettingsStore ---

struct SettingsStore::Layer {
    std::string path; // "" when the scope has no file.
    bool exists = false;
    bool parsed = false;
    std::string bytes;
    std::vector<std::optional<Value>> values;
    std::vector<std::pair<std::size_t, std::pair<std::uint32_t, std::uint32_t>>> positions;
    std::vector<Diagnostic> diagnostics;
};

struct SettingsStore::State {
    SettingsPaths paths;
    bool trusted = false;
    std::array<Layer, 3> layers;
    std::shared_ptr<const SettingsSnapshot> snapshot;
    std::uint64_t generation = 0;
};

SettingsStore::SettingsStore(const Registry& registry, io::IFileSystem& fs, StoreOptions options)
    : m_registry(&registry), m_fs(&fs),
      m_migrations(options.migrations != nullptr ? options.migrations : &builtinMigrations()), m_style(options.style),
      m_state(std::make_unique<State>()), m_listeners(std::make_shared<detail::ListenerTable>()) {
    m_state->trusted = options.projectTrusted;
}

SettingsStore::~SettingsStore() = default;

Result<std::unique_ptr<SettingsStore>> SettingsStore::open(const Registry& registry, io::IFileSystem& fs,
                                                           SettingsPaths paths, StoreOptions options) {
    std::unique_ptr<SettingsStore> store(new SettingsStore(registry, fs, options));
    {
        const LockGuard lock(store->m_mutex);
        if (auto ok = store->setPaths(std::move(paths)); !ok) {
            return std::unexpected(ok.error());
        }
    }
    // Nothing else can see the store yet, so loading layer by layer needs no care about races.
    for (const Scope scope : kFileScopes) {
        (void)store->reload(scope);
    }
    {
        const LockGuard lock(store->m_mutex);
        if (!store->m_state->snapshot) {
            (void)store->republish();
        }
    }
    return store;
}

Result<void> SettingsStore::setPaths(SettingsPaths paths) {
    if (!paths.applicationFile.empty()) {
        auto normalized = io::normalizePath(paths.applicationFile, m_style);
        if (!normalized) {
            return std::unexpected(normalized.error());
        }
        paths.applicationFile = std::move(*normalized);
    }
    if (paths.project) {
        auto layout = ProjectLayout::forRoot(paths.project->root, m_style);
        if (!layout) {
            return std::unexpected(layout.error());
        }
        paths.project = std::move(*layout);
    }
    State& state = *m_state;
    state.layers = {};
    state.layers[0].path = paths.applicationFile;
    state.layers[1].path = paths.project ? paths.project->sharedSettings : std::string();
    state.layers[2].path = paths.project ? paths.project->personalSettings : std::string();
    state.paths = std::move(paths);
    return {};
}

SettingsStore::Layer SettingsStore::layerFromBytes(Scope scope, const std::string& path, std::string_view bytes,
                                                   const Layer* previous) const {
    detail::CheckedDocument checked = detail::checkDocument(*m_registry, scope, path, bytes, *m_migrations);
    Layer layer{.path = path,
                .exists = true,
                .parsed = checked.parsed,
                .bytes = std::string(bytes),
                .values = std::move(checked.values),
                .positions = std::move(checked.positions),
                .diagnostics = std::move(checked.diagnostics)};
    if (!layer.parsed && previous != nullptr && previous->parsed && previous->path == path) {
        // A half-typed edit must not flip every setting back to its default: keep the last good values.
        layer.values = previous->values;
        layer.positions = previous->positions;
        layer.diagnostics.push_back(Diagnostic{.severity = Severity::Info,
                                               .scope = scope,
                                               .path = path,
                                               .key = {},
                                               .line = 0,
                                               .column = 0,
                                               .message = "the last valid settings of this file stay in force"});
    }
    return layer;
}

SettingsStore::Layer SettingsStore::loadLayer(Scope scope, const std::string& path, const Layer* previous) const {
    if (path.empty()) {
        return Layer{};
    }
    auto bytes = m_fs->readFile(path);
    if (bytes) {
        return layerFromBytes(scope, path, *bytes, previous);
    }
    Layer layer;
    layer.path = path;
    if (bytes.error().code() == ErrorCode::NotFound) {
        return layer; // No file: the scope sets nothing.
    }
    if (previous != nullptr && previous->path == path) {
        layer = *previous; // Unreadable for now (locked, say): keep what was loaded.
    }
    layer.diagnostics.push_back(Diagnostic{.severity = Severity::Error,
                                           .scope = scope,
                                           .path = path,
                                           .key = {},
                                           .line = 0,
                                           .column = 0,
                                           .message = "cannot be read: " + bytes.error().describe()});
    return layer;
}

SettingsChange SettingsStore::republish() {
    State& state = *m_state;
    const std::size_t count = m_registry->size();
    std::vector<SettingsSnapshot::Entry> entries;
    entries.reserve(count);
    std::vector<std::string> ignored;
    std::vector<Diagnostic> trustWarnings;
    for (std::size_t i = 0; i < count; ++i) {
        const KeySpec& spec = m_registry->at(i);
        SettingsSnapshot::Entry entry{.value = spec.defaultValue, .source = Scope::Default};
        // Highest scope first: project-personal, project-shared, application.
        for (const Scope scope : std::views::reverse(kFileScopes)) {
            const Layer& layer = state.layers[*layerIndex(scope)];
            if (i >= layer.values.size() || !layer.values[i] || !spec.scopes.has(scope)) {
                continue;
            }
            if (spec.executes && isProjectScope(scope) && !state.trusted) {
                // The trust rule (PLAN.md section 3.5): a project's executes values wait for trust.
                ignored.push_back(spec.key);
                const auto position =
                    std::ranges::find_if(layer.positions, [i](const auto& entry) { return entry.first == i; });
                trustWarnings.push_back(
                    Diagnostic{.severity = Severity::Warning,
                               .scope = scope,
                               .path = layer.path,
                               .key = spec.key,
                               .line = position != layer.positions.end() ? position->second.first : 0,
                               .column = position != layer.positions.end() ? position->second.second : 0,
                               .message = "runs code, so it is ignored until the project is trusted"});
                continue;
            }
            entry = SettingsSnapshot::Entry{.value = *layer.values[i], .source = scope};
            break;
        }
        entries.push_back(std::move(entry));
    }
    std::ranges::sort(ignored);
    const auto duplicates = std::ranges::unique(ignored);
    ignored.erase(duplicates.begin(), duplicates.end());

    std::vector<Diagnostic> diagnostics;
    for (const Layer& layer : state.layers) {
        diagnostics.insert(diagnostics.end(), layer.diagnostics.begin(), layer.diagnostics.end());
    }
    diagnostics.insert(diagnostics.end(), trustWarnings.begin(), trustWarnings.end());

    SettingsChange change;
    const SettingsSnapshot* old = state.snapshot.get();
    for (std::size_t i = 0; i < count; ++i) {
        if (old == nullptr || old->entry(i).value != entries[i].value || old->entry(i).source != entries[i].source) {
            change.keys.push_back(m_registry->at(i).key);
        }
    }
    const bool sameRest = old != nullptr && old->trusted() == state.trusted &&
                          std::ranges::equal(old->diagnostics(), diagnostics) &&
                          std::ranges::equal(old->ignoredUntilTrusted(), ignored);
    if (change.keys.empty() && sameRest) {
        return change; // Nothing to publish.
    }
    state.snapshot = std::make_shared<const SettingsSnapshot>(*m_registry, std::move(entries), std::move(diagnostics),
                                                              std::move(ignored), state.trusted, ++state.generation);
    change.snapshot = state.snapshot;
    return change;
}

void SettingsStore::notify(const SettingsChange& change) {
    if (!change.snapshot) {
        return;
    }
    std::vector<std::shared_ptr<const SettingsListener>> listeners;
    {
        const LockGuard lock(m_listeners->mutex);
        for (const auto& entry : m_listeners->listeners) {
            listeners.push_back(entry.second);
        }
    }
    for (const auto& listener : listeners) {
        (*listener)(change);
    }
}

SettingsChange SettingsStore::installLayer(Scope scope, Layer layer) {
    SettingsChange change;
    {
        const LockGuard lock(m_mutex);
        Layer& slot = m_state->layers[*layerIndex(scope)];
        if (slot.path != layer.path) {
            return {}; // The project changed while the file was read.
        }
        slot = std::move(layer);
        change = republish();
    }
    return change;
}

std::shared_ptr<const SettingsSnapshot> SettingsStore::snapshot() const {
    const LockGuard lock(m_mutex);
    return m_state->snapshot;
}

std::string SettingsStore::pathOf(Scope scope) const {
    const auto index = layerIndex(scope);
    if (!index) {
        return {};
    }
    const LockGuard lock(m_mutex);
    return m_state->layers[*index].path;
}

std::optional<Scope> SettingsStore::scopeOf(std::string_view path) const {
    auto normalized = io::normalizePath(path, m_style);
    if (!normalized) {
        return std::nullopt;
    }
    const LockGuard lock(m_mutex);
    for (const Scope scope : kFileScopes) {
        const std::string& candidate = m_state->layers[*layerIndex(scope)].path;
        if (!candidate.empty() && candidate == *normalized) {
            return scope;
        }
    }
    return std::nullopt;
}

std::vector<std::string> SettingsStore::watchDirectories() const {
    std::vector<std::string> directories;
    const LockGuard lock(m_mutex);
    for (const Layer& layer : m_state->layers) {
        if (layer.path.empty()) {
            continue;
        }
        std::string parent(io::parentPath(layer.path, m_style));
        if (!parent.empty() && std::ranges::find(directories, parent) == directories.end()) {
            directories.push_back(std::move(parent));
        }
    }
    return directories;
}

Result<void> SettingsStore::setProject(std::optional<ProjectLayout> project) {
    SettingsChange change;
    {
        const LockGuard editLock(m_editMutex);
        if (project) {
            auto layout = ProjectLayout::forRoot(project->root, m_style);
            if (!layout) {
                return std::unexpected(layout.error());
            }
            project = std::move(*layout);
        }
        Layer shared = loadLayer(Scope::ProjectShared, project ? project->sharedSettings : std::string(), nullptr);
        Layer personal =
            loadLayer(Scope::ProjectPersonal, project ? project->personalSettings : std::string(), nullptr);
        {
            const LockGuard lock(m_mutex);
            m_state->paths.project = std::move(project);
            m_state->layers[1] = std::move(shared);
            m_state->layers[2] = std::move(personal);
            change = republish();
        }
    }
    notify(change);
    return {};
}

void SettingsStore::setProjectTrusted(bool trusted) {
    SettingsChange change;
    {
        const LockGuard lock(m_mutex);
        m_state->trusted = trusted;
        change = republish();
    }
    notify(change);
}

std::vector<std::string> SettingsStore::reload(Scope scope) {
    const auto index = layerIndex(scope);
    if (!index) {
        return {};
    }
    SettingsChange change;
    {
        const LockGuard editLock(m_editMutex);
        Layer previous;
        {
            const LockGuard lock(m_mutex);
            previous = m_state->layers[*index];
        }
        Layer layer = loadLayer(scope, previous.path, &previous);
        change = installLayer(scope, std::move(layer));
    }
    notify(change);
    return change.keys;
}

std::vector<std::string> SettingsStore::applyChanges(const io::FileChangeBatch& batch) {
    std::array<std::string, 3> paths;
    {
        const LockGuard lock(m_mutex);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            paths[i] = m_state->layers[i].path;
        }
    }
    std::vector<std::string> changed;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].empty()) {
            continue;
        }
        const bool affected =
            std::ranges::any_of(batch.changes, [&](const io::FileChange& change) { return touches(change, paths[i]); });
        if (affected) {
            for (std::string& key : reload(kFileScopes[i])) {
                if (std::ranges::find(changed, key) == changed.end()) {
                    changed.push_back(std::move(key));
                }
            }
        }
    }
    return changed;
}

sched::Task<Result<void>> SettingsStore::follow(io::IDirectoryWatch& watch, std::stop_token token) {
    for (;;) {
        auto batch = co_await watch.next(token);
        if (!batch) {
            co_return std::unexpected(batch.error());
        }
        (void)applyChanges(*batch);
    }
}

Subscription SettingsStore::subscribe(SettingsListener listener) {
    const LockGuard lock(m_listeners->mutex);
    const std::uint64_t id = m_listeners->nextId++;
    m_listeners->listeners.emplace_back(id, std::make_shared<const SettingsListener>(std::move(listener)));
    return Subscription(m_listeners, id);
}

Result<void> SettingsStore::editFile(Scope scope, const std::function<Result<bool>(SettingsDocument&)>& edit) {
    if (auto ok = checkFileScope(scope); !ok) {
        return ok;
    }
    SettingsChange change;
    {
        const LockGuard editLock(m_editMutex);
        std::string path;
        std::optional<ProjectLayout> project;
        {
            const LockGuard lock(m_mutex);
            path = m_state->layers[*layerIndex(scope)].path;
            project = m_state->paths.project;
        }
        if (path.empty()) {
            return fail(ErrorCode::NotFound,
                        std::format("there is no {} settings file (no project is open)", toString(scope)));
        }
        auto bytes = m_fs->readFile(path);
        SettingsDocument document;
        if (bytes) {
            auto parsed = SettingsDocument::parse(std::move(*bytes));
            if (!parsed) {
                Error error = parsed.error();
                return std::unexpected(std::move(error).withContext(std::format("{} was left unchanged", path)));
            }
            document = std::move(*parsed);
        } else if (bytes.error().code() == ErrorCode::NotFound) {
            document = SettingsDocument::create(m_migrations->currentVersion());
        } else {
            return std::unexpected(bytes.error());
        }
        auto migrated = m_migrations->apply(document);
        if (!migrated) {
            return std::unexpected(migrated.error());
        }
        if (migrated->newer) {
            return fail(ErrorCode::Unsupported,
                        std::format("{} was written by a newer FrostIDE (settings version {}); it is left unchanged",
                                    path, migrated->fromVersion));
        }
        auto edited = edit(document);
        if (!edited) {
            return std::unexpected(edited.error());
        }
        if (!*edited) {
            return {}; // Nothing to change on disk.
        }
        if (isProjectScope(scope) && project) {
            if (auto ok = ensureProjectLayout(*m_fs, *project); !ok) {
                return ok;
            }
        } else if (auto made = m_fs->createDirectories(io::parentPath(path, m_style)); !made) {
            return made;
        }
        if (auto written = m_fs->writeFileAtomic(path, document.bytes()); !written) {
            return written;
        }
        change = installLayer(scope, layerFromBytes(scope, path, document.bytes(), nullptr));
    }
    notify(change);
    return {};
}

Result<void> SettingsStore::set(Scope scope, std::string_view key, const Value& value) {
    if (auto ok = checkFileScope(scope); !ok) {
        return ok;
    }
    const KeySpec* spec = m_registry->find(key);
    if (spec == nullptr) {
        return fail(ErrorCode::NotFound, std::format("{} is not a known setting", key));
    }
    if (!spec->scopes.has(scope)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} cannot be set in {} settings", key, toString(scope)));
    }
    if (auto ok = validateValue(spec->schema, value); !ok) {
        return fail(ErrorCode::InvalidArgument, std::format("{} {}", key, ok.error().message()));
    }
    return editFile(scope, [&](SettingsDocument& document) -> Result<bool> {
        if (auto ok = document.set(key, value); !ok) {
            return std::unexpected(ok.error());
        }
        return true;
    });
}

Result<void> SettingsStore::reset(Scope scope, std::string_view key) {
    if (auto ok = checkFileScope(scope); !ok) {
        return ok;
    }
    if (m_registry->find(key) == nullptr) {
        return fail(ErrorCode::NotFound, std::format("{} is not a known setting", key));
    }
    if (pathOf(scope).empty()) {
        return fail(ErrorCode::NotFound,
                    std::format("there is no {} settings file (no project is open)", toString(scope)));
    }
    return editFile(scope, [&](SettingsDocument& document) -> Result<bool> { return document.remove(key); });
}

Result<SettingsDocument> SettingsStore::readScopeDocument(Scope scope) const {
    const LockGuard editLock(m_editMutex);
    if (auto ok = checkFileScope(scope); !ok) {
        return std::unexpected(ok.error());
    }
    const std::string path = pathOf(scope);
    if (path.empty()) {
        return fail(ErrorCode::NotFound, "there is no settings file for this scope");
    }
    auto bytes = m_fs->readFile(path);
    if (!bytes && bytes.error().code() != ErrorCode::NotFound) {
        return std::unexpected(bytes.error());
    }
    auto document = bytes ? SettingsDocument::parse(std::move(*bytes))
                          : Result<SettingsDocument>(SettingsDocument::create(m_migrations->currentVersion()));
    if (!document) {
        return std::unexpected(document.error());
    }
    auto migrated = m_migrations->apply(*document);
    if (!migrated) {
        return std::unexpected(migrated.error());
    }
    if (migrated->newer) {
        return fail(ErrorCode::Unsupported, std::format("{} was written by a newer FrostIDE", path));
    }
    return document;
}

Result<void> SettingsStore::applyConditional(Scope scope, std::span<const ConditionalSettingEdit> edits) {
    if (edits.empty()) {
        return {};
    }
    if (auto ok = checkFileScope(scope); !ok) {
        return ok;
    }
    for (std::size_t i = 0; i < edits.size(); ++i) {
        const auto& edit = edits[i];
        const KeySpec* spec = m_registry->find(edit.key);
        if (spec == nullptr) {
            return fail(ErrorCode::NotFound, std::format("{} is not a known setting", edit.key));
        }
        if (!spec->scopes.has(scope)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{} cannot be set in {} settings", edit.key, toString(scope)));
        }
        if (std::ranges::any_of(edits.first(i), [&](const auto& previous) { return previous.key == edit.key; })) {
            return fail(ErrorCode::InvalidArgument, std::format("{} is edited twice", edit.key));
        }
        if (edit.value) {
            if (auto valid = validateValue(spec->schema, *edit.value); !valid) {
                return fail(ErrorCode::InvalidArgument, std::format("{} {}", edit.key, valid.error().message()));
            }
        }
    }
    return editFile(scope, [&](SettingsDocument& document) -> Result<bool> {
        for (const auto& edit : edits) {
            const auto* member = document.find(edit.key);
            const std::optional<std::string> current =
                member ? std::optional<std::string>(document.rawValue(*member)) : std::nullopt;
            if (current != edit.expectedRaw) {
                return fail(ErrorCode::Busy, std::format("{} changed since the dialog was opened", edit.key));
            }
        }
        bool changed = false;
        for (const auto& edit : edits) {
            if (edit.value) {
                if (auto set = document.set(edit.key, *edit.value); !set)
                    return std::unexpected(set.error());
                changed = true;
            } else {
                auto removed = document.remove(edit.key);
                if (!removed)
                    return std::unexpected(removed.error());
                changed = changed || *removed;
            }
        }
        return changed;
    });
}

std::optional<SaveCheck> SettingsStore::checkSavedFile(std::string_view path, std::string_view bytes) {
    SettingsChange change;
    std::optional<SaveCheck> result;
    {
        const LockGuard editLock(m_editMutex);
        const auto scope = scopeOf(path);
        if (!scope) {
            return std::nullopt;
        }
        Layer previous;
        {
            const LockGuard lock(m_mutex);
            previous = m_state->layers[*layerIndex(*scope)];
        }
        Layer saved = layerFromBytes(*scope, previous.path, bytes, &previous);
        SaveCheck check{.scope = *scope, .diagnostics = saved.diagnostics, .notifications = {}};
        bool trusted = false;
        {
            const LockGuard lock(m_mutex);
            trusted = m_state->trusted;
        }
        if (isProjectScope(*scope) && !trusted) {
            for (std::size_t i = 0; i < saved.values.size(); ++i) {
                const KeySpec& spec = m_registry->at(i);
                if (!saved.values[i] || !spec.executes || !spec.scopes.has(*scope))
                    continue;
                const auto position =
                    std::ranges::find_if(saved.positions, [i](const auto& entry) { return entry.first == i; });
                check.diagnostics.push_back(
                    Diagnostic{.severity = Severity::Warning,
                               .scope = *scope,
                               .path = previous.path,
                               .key = spec.key,
                               .line = position != saved.positions.end() ? position->second.first : 0,
                               .column = position != saved.positions.end() ? position->second.second : 0,
                               .message = "runs code, so it is ignored until the project is trusted"});
            }
        }
        check.notifications = toNotifications(check.diagnostics);
        auto disk = m_fs->readFile(previous.path);
        if (disk && *disk == bytes) {
            change = installLayer(*scope, std::move(saved));
        } else {
            change = installLayer(*scope, loadLayer(*scope, previous.path, &previous));
        }
        result = std::move(check);
    }
    notify(change);
    return result;
}

Result<std::string> SettingsStore::prepareForEditing(Scope scope) {
    if (auto ok = checkFileScope(scope); !ok) {
        return std::unexpected(ok.error());
    }
    std::string path;
    SettingsChange change;
    {
        const LockGuard editLock(m_editMutex);
        path = pathOf(scope);
        std::optional<ProjectLayout> project;
        {
            const LockGuard lock(m_mutex);
            project = m_state->paths.project;
        }
        if (path.empty()) {
            return fail(ErrorCode::NotFound,
                        std::format("there is no {} settings file (no project is open)", toString(scope)));
        }
        if (isProjectScope(scope) && project) {
            if (auto ok = ensureProjectLayout(*m_fs, *project); !ok) {
                return std::unexpected(ok.error());
            }
        } else if (auto made = m_fs->createDirectories(io::parentPath(path, m_style)); !made) {
            return std::unexpected(made.error());
        }
        auto exists = m_fs->exists(path);
        if (!exists) {
            return std::unexpected(exists.error());
        }
        if (!*exists) {
            if (auto written =
                    m_fs->writeFileAtomic(path, SettingsDocument::create(m_migrations->currentVersion()).bytes());
                !written) {
                return std::unexpected(written.error());
            }
            change = installLayer(scope, loadLayer(scope, path, nullptr));
        }
    }
    notify(change);
    return path;
}

std::span<const SettingsActionInfo> settingsActions() noexcept {
    static constexpr std::array<SettingsActionInfo, 3> kActions = {{
        {.id = "settings.openApplicationFile",
         .name = "Open Application Settings File",
         .description = "Opens your settings.json, which applies to every project.",
         .group = "File",
         .scope = Scope::Application,
         .needsProject = false},
        {.id = "settings.openProjectFile",
         .name = "Open Project Settings File",
         .description = "Opens .frost/settings.json, the project settings shared through version control.",
         .group = "File",
         .scope = Scope::ProjectShared,
         .needsProject = true},
        {.id = "settings.openPersonalProjectFile",
         .name = "Open Personal Project Settings File",
         .description = "Opens .frost/workspace.json, your own settings for this project (not committed).",
         .group = "File",
         .scope = Scope::ProjectPersonal,
         .needsProject = true},
    }};
    return kActions;
}

} // namespace frost::settings
