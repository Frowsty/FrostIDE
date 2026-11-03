// SPDX-License-Identifier: Apache-2.0
//
// SettingsStore: the loaded scope files, their effective values as immutable snapshots, writes that
// edit the files byte-exactly, live reload from file-watcher batches, the trust rule for `executes`
// keys, validation on save, and the model of the Settings-dialog stand-in actions (ADR 0021).
/*
 * Resolution. For each registry key, the effective value comes from the highest scope (registry.hpp)
 * whose file holds a valid value for it, provided the key allows that scope and, for `executes` keys
 * in ProjectShared or ProjectPersonal, the project is trusted. Otherwise it is the default. Invalid
 * values, disallowed scopes and untrusted executes values are skipped and reported as diagnostics.
 * File-level (.editorconfig) values are layered per file on top of a snapshot (editorconfig.hpp).
 *
 * Never blocking. open() never fails because of file contents: a missing file is an empty scope, a
 * broken one is reported and contributes nothing (on reload, its last good values stay in force until
 * it parses again). FrostIDE always starts and opens projects with the defaults at worst.
 *
 * Live reload. The shell owns the watches (ADR 0020: the project's recursive watch fans out, or
 * cheap non-recursive watches of watchDirectories()) and hands each batch to applyChanges(), or lets
 * follow() pump one watch. Changes are published as a new snapshot and reported to subscribers with
 * the keys whose effective value or source changed. A write by the store itself reloads at once; the
 * watcher's echo then changes nothing.
 *
 * Thread safety: every member may be called from any thread; one mutex guards the state, and file
 * I/O and listener calls run outside it. Listeners run on the thread that made the change and must
 * not block (the UI posts to its own thread). Snapshots are immutable and may be shared freely.
 * Lifetime: the registry and the file system outlive the store; the store outlives follow() tasks.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/task.hpp"
#include "frost/settings/document.hpp"
#include "frost/settings/keys.hpp"
#include "frost/settings/migration.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/validation.hpp"
#include "frost/settings/value.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace frost::settings {

namespace detail {
struct ListenerTable;
} // namespace detail

/*
 * The effective settings at one moment. Immutable; any thread. get() and value() take keys of the
 * snapshot's registry: a key that is not registered (or a typed key of the wrong type) is a
 * precondition violation, checked.
 */
class SettingsSnapshot {
public:
    struct Entry {
        Value value;
        Scope source = Scope::Default;
    };

    SettingsSnapshot(const Registry& registry, std::vector<Entry> entries, std::vector<Diagnostic> diagnostics,
                     std::vector<std::string> ignoredUntilTrusted, bool trusted, std::uint64_t generation);

    // The effective value of `key`. O(log n).
    template<SettingType T>
    [[nodiscard]] const T& get(Key<T> key) const {
        return std::get<T>(value(key.name)); // value() checks the key; the registry test checks the type.
    }
    [[nodiscard]] const Value& value(std::string_view key) const;
    // The scope the effective value came from. O(log n).
    [[nodiscard]] Scope source(std::string_view key) const;
    // By registry index (Registry::indexOf). Precondition: index < registry().size().
    [[nodiscard]] const Entry& entry(std::size_t index) const;

    // Every diagnostic of every loaded scope file, application first.
    [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept { return m_diagnostics; }
    // The `executes` keys a project file sets that are ignored because the project is not trusted
    // (for the trust banner, PLAN.md section 3.5). Sorted, unique.
    [[nodiscard]] std::span<const std::string> ignoredUntilTrusted() const noexcept { return m_ignored; }
    [[nodiscard]] bool trusted() const noexcept { return m_trusted; }
    // Increases with every published snapshot of one store.
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] const Registry& registry() const noexcept { return *m_registry; }

private:
    const Registry* m_registry;
    std::vector<Entry> m_entries;
    std::vector<Diagnostic> m_diagnostics;
    std::vector<std::string> m_ignored;
    bool m_trusted;
    std::uint64_t m_generation;
};

// What a subscriber is told after a change.
struct SettingsChange {
    std::shared_ptr<const SettingsSnapshot> snapshot;
    std::vector<std::string> keys; // Keys whose effective value or source changed, in registry order.
};

using SettingsListener = std::function<void(const SettingsChange&)>;

// Unsubscribes on destruction or reset(). Move-only; may outlive the store (then does nothing).
class Subscription {
public:
    Subscription() = default;
    ~Subscription() { reset(); }
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete ("a subscription has one owner");
    Subscription& operator=(const Subscription&) = delete ("a subscription has one owner");
    void reset() noexcept;

private:
    friend class SettingsStore;
    Subscription(std::weak_ptr<detail::ListenerTable> table, std::uint64_t id) noexcept
        : m_table(std::move(table)), m_id(id) {}
    std::weak_ptr<detail::ListenerTable> m_table;
    std::uint64_t m_id = 0;
};

// Where the scope files are.
struct SettingsPaths {
    std::string applicationFile;          // Absolute; "" for none (tests, a portable profile).
    std::optional<ProjectLayout> project; // nullopt when no project is open.
};

struct StoreOptions {
    bool projectTrusted = false;              // The open project's trust state (the project layer's TrustStore).
    const MigrationSet* migrations = nullptr; // nullptr: builtinMigrations().
    io::PathStyle style = io::kNativePathStyle;
};

// The result of validation on save.
struct SaveCheck {
    Scope scope = Scope::Application;
    std::vector<Diagnostic> diagnostics;
    std::vector<SettingsNotification> notifications; // toNotifications(diagnostics).
};

// One optimistic file edit. expectedRaw is the previously read JSON value, or nullopt
// when the key was absent. value == nullopt removes the application override.
struct ConditionalSettingEdit {
    std::string key;
    std::optional<std::string> expectedRaw;
    std::optional<Value> value;
};

class SettingsStore {
public:
    /*
     * Loads every scope file in `paths` (see "Never blocking"). Errors: InvalidArgument for a path
     * that is not absolute under options.style. O(total file size + keys).
     */
    [[nodiscard]] static Result<std::unique_ptr<SettingsStore>> open(const Registry& registry, io::IFileSystem& fs,
                                                                     SettingsPaths paths, StoreOptions options = {});
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete ("listeners and tasks refer to the store");
    SettingsStore& operator=(const SettingsStore&) = delete ("listeners and tasks refer to the store");

    // The current effective settings. O(1).
    [[nodiscard]] std::shared_ptr<const SettingsSnapshot> snapshot() const;
    [[nodiscard]] const Registry& registry() const noexcept { return *m_registry; }

    // The file of Application, ProjectShared or ProjectPersonal; "" when that scope has none.
    [[nodiscard]] std::string pathOf(Scope scope) const;
    // The scope whose file is `path` (compared after normalisation), if any.
    [[nodiscard]] std::optional<Scope> scopeOf(std::string_view path) const;
    // The existing-or-not parent directories of the scope files, for non-recursive watches. Unique.
    [[nodiscard]] std::vector<std::string> watchDirectories() const;

    // Switches the open project (nullopt closes it) and reloads both project scopes. Errors:
    // InvalidArgument (paths). Subscribers hear about every changed key.
    [[nodiscard]] Result<void> setProject(std::optional<ProjectLayout> project);
    // Applies a new trust state: project `executes` values start or stop counting.
    void setProjectTrusted(bool trusted);

    /*
     * Writes `value` for `key` into `scope`'s file and publishes the result. The file is re-read
     * first (so edits made meanwhile are kept), migrated, edited byte-exactly and written with
     * writeFileAtomic; a project scope gets ensureProjectLayout() first. Errors: NotFound (unknown
     * key, or no file for that scope), InvalidArgument (scope not allowed for the key, Default or
     * FileLevel, value fails the schema), Corrupt (the file does not parse: it is left untouched),
     * Unsupported (the file is newer than this build), and the file system's errors.
     */
    [[nodiscard]] Result<void> set(Scope scope, std::string_view key, const Value& value);
    // Typed convenience for set().
    template<SettingType T>
    [[nodiscard]] Result<void> set(Scope scope, Key<T> key, T value) {
        return set(scope, key.name, Value(std::move(value)));
    }
    // Removes `key` from `scope`'s file (the next lower scope applies again). Absent: success, no
    // write. Errors: as set(), without the value checks.
    [[nodiscard]] Result<void> reset(Scope scope, std::string_view key);

    // Reads a migrated scope document without creating a missing file. Errors: as set(),
    // including parse, migration and filesystem failures. Blocking; thread-safe.
    [[nodiscard]] Result<SettingsDocument> readScopeDocument(Scope scope) const;
    /*
     * Applies validated edits to a freshly read scope document in one atomic replacement.
     * If any edited key's raw JSON no longer matches expectedRaw, returns Busy without writing.
     * Unrelated members are preserved. The underlying filesystem has no cross-process conditional
     * replace: another writer racing between the read and atomic replacement can still win or lose.
     * Blocking; thread-safe subject to that filesystem limitation. O(edits * file size).
     */
    [[nodiscard]] Result<void> applyConditional(Scope scope, std::span<const ConditionalSettingEdit> edits);

    /*
     * Reloads every scope file the batch touches, following file_watcher.hpp's rules for a consumer
     * of single paths. Returns the changed keys (also sent to subscribers). Never fails: read errors
     * become diagnostics. Batches from any watch are accepted; unrelated paths are ignored.
     */
    std::vector<std::string> applyChanges(const io::FileChangeBatch& batch);
    // Reloads one scope's file from disk now. Returns the changed keys.
    std::vector<std::string> reload(Scope scope);

    /*
     * Pumps `watch` until it ends: every batch goes to applyChanges(). Completes with the watch's
     * error (Cancelled through `token` or shutdown; NotFound when the watched directory went away).
     * Precondition: the store and the watch outlive the task.
     */
    [[nodiscard]] sched::Task<Result<void>> follow(io::IDirectoryWatch& watch, std::stop_token token = {});

    // Registers `listener` for published changes; it is called after the change, outside the lock.
    [[nodiscard]] Subscription subscribe(SettingsListener listener);

    /*
     * Validation on save. Call after the editor saved `bytes` to `path` (the bytes on disk, unchanged):
     * when `path` is a scope file, the bytes are checked against the registry, the scope is reloaded
     * from them, and the diagnostics and notifications are returned; nullopt for other files.
     * Never writes anything.
     */
    [[nodiscard]] std::optional<SaveCheck> checkSavedFile(std::string_view path, std::string_view bytes);

    /*
     * The Settings-dialog stand-in (until M15): makes sure `scope`'s file exists so the editor can
     * open it, creating it as SettingsDocument::create(kCurrentSchemaVersion) with its directory
     * (and, for project scopes, ensureProjectLayout()) when missing, and returns its path. An
     * existing file is never touched. Errors: NotFound (no file for that scope: no project open),
     * InvalidArgument (Default or FileLevel), the file system's errors.
     */
    [[nodiscard]] Result<std::string> prepareForEditing(Scope scope);

private:
    struct Layer;
    struct State;

    SettingsStore(const Registry& registry, io::IFileSystem& fs, StoreOptions options);
    [[nodiscard]] Result<void> setPaths(SettingsPaths paths) FROST_REQUIRES(m_mutex);
    [[nodiscard]] Layer loadLayer(Scope scope, const std::string& path, const Layer* previous) const;
    [[nodiscard]] Layer layerFromBytes(Scope scope, const std::string& path, std::string_view bytes,
                                       const Layer* previous) const;
    // Rebuilds the snapshot from the layers; returns the change to publish (empty keys: nothing changed).
    [[nodiscard]] SettingsChange republish() FROST_REQUIRES(m_mutex);
    void notify(const SettingsChange& change);
    [[nodiscard]] SettingsChange installLayer(Scope scope, Layer layer);
    [[nodiscard]] Result<void> editFile(Scope scope, const std::function<Result<bool>(SettingsDocument&)>& edit);

    const Registry* m_registry;
    io::IFileSystem* m_fs;
    const MigrationSet* m_migrations;
    io::PathStyle m_style;
    // Serializes this store's file read/modify/write operations; listeners run after release.
    mutable Mutex m_editMutex;
    mutable Mutex m_mutex;
    std::unique_ptr<State> m_state FROST_PT_GUARDED_BY(m_mutex);
    std::shared_ptr<detail::ListenerTable> m_listeners;
};

// The stand-in actions' descriptions, for the shell to register in frost::input's ActionRegistry
// (frost::settings does not depend on frost::input). The handler calls prepareForEditing(scope) and
// opens the returned path in an editor tab, then reports errors as notifications.
struct SettingsActionInfo {
    std::string_view id;   // "settings.openApplicationFile" ...
    std::string_view name; // Find Action text.
    std::string_view description;
    std::string_view group; // "File".
    Scope scope;
    bool needsProject; // Disabled while no project is open.
};

[[nodiscard]] std::span<const SettingsActionInfo> settingsActions() noexcept;

} // namespace frost::settings
