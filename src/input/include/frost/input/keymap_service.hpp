// SPDX-License-Identifier: Apache-2.0
//
// KeymapService, the facade the shell uses (ADR 0016): it owns the parsed keymap, the
// current preferences, the BindingIndex, a copy of the active layout and the
// ShortcutMatcher, and answers key events and shortcut queries. Also IKeymapSettings, the
// seam through which the settings registry (m1/settings, bound in the composition root)
// supplies keymap preferences, and its in-memory double MemoryKeymapSettings.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/input/action.hpp"
#include "frost/input/key_event.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/shortcut_matcher.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace frost::input {

/*
 * Where keymap preferences live: the application-scope keys keymap.id,
 * keymap.doubleTapIntervalMs and keymap.overrides of the settings registry. Both calls
 * run on the UI thread and must not block on I/O: load() reads the in-memory registry;
 * store() updates it and schedules the write, whose failure the settings module reports
 * through its own notifications. The implementation lives in the composition root
 * (src/app), so frost::settings never depends on frost::input.
 */
class IKeymapSettings {
public:
    [[nodiscard]] virtual Result<KeymapPreferences> load() const = 0;
    [[nodiscard]] virtual Result<void> store(const KeymapPreferences& preferences) = 0;

protected:
    IKeymapSettings() = default;
    IKeymapSettings(const IKeymapSettings&) = default;
    IKeymapSettings& operator=(const IKeymapSettings&) = default;
    ~IKeymapSettings() = default;
};

enum class SettingsOp : std::uint8_t { Load, Store };

/*
 * The IKeymapSettings double: preferences in memory, with fault injection.
 * injectFault(op, code, count) makes the next `count` calls of `op` fail with `code`
 * before they read or change anything. Thread-safe: one mutex guards everything, like MemFs.
 */
class MemoryKeymapSettings final : public IKeymapSettings {
public:
    explicit MemoryKeymapSettings(KeymapPreferences initial = {});

    [[nodiscard]] Result<KeymapPreferences> load() const override;
    [[nodiscard]] Result<void> store(const KeymapPreferences& preferences) override;

    // Precondition: count >= 1.
    void injectFault(SettingsOp op, ErrorCode code, std::uint32_t count = 1);
    // Replaces the stored preferences directly (an external edit, as a live reload sees it).
    void set(KeymapPreferences preferences);
    [[nodiscard]] KeymapPreferences current() const;
    [[nodiscard]] std::uint32_t loadCount() const;
    [[nodiscard]] std::uint32_t storeCount() const;

private:
    struct Fault {
        SettingsOp op;
        ErrorCode code;
        std::uint32_t remaining;
    };
    [[nodiscard]] std::optional<ErrorCode> takeFault(SettingsOp op) const FROST_REQUIRES(m_mutex);

    mutable Mutex m_mutex;
    KeymapPreferences m_preferences FROST_GUARDED_BY(m_mutex);
    mutable std::deque<Fault> m_faults FROST_GUARDED_BY(m_mutex);
    mutable std::uint32_t m_loads FROST_GUARDED_BY(m_mutex) = 0;
    std::uint32_t m_stores FROST_GUARDED_BY(m_mutex) = 0;
};

/*
 * The facade. Not thread-safe: UI thread only. The registry and the settings are held by
 * pointer (so the service is movable) and must outlive it.
 *
 * Lifetimes: spans and references obtained through the service (shortcutsFor, bindings,
 * layout) stay valid until the next call that changes bindingsGeneration(): handle(),
 * peek(), shortcutsFor(), primaryShortcut(), bindings() or unreachableBindings() after
 * the registry grew, and setPreferences(), reloadPreferences() or setLayout(). Consumers
 * such as menus re-query when the generation changes.
 */
class KeymapService {
public:
    /*
     * Creates the service. The shell passes QStyleHints::mouseDoubleClickInterval() as the
     * platform interval (Windows GetDoubleClickTime, 500 ms by default). A settings load
     * failure gives the IntelliJ defaults with no overrides; loaded preferences are
     * sanitised (valid overrides apply even when others are invalid; an unknown keymap id
     * falls back to intellij with the overrides kept). Any problem is kept in
     * lastSettingsError(); create() never calls store(). Errors: Internal only, if the
     * built-in keymap does not parse. Budget: B1 (at most 1 ms in Release).
     */
    [[nodiscard]] static Result<KeymapService>
    create(const ActionRegistry& registry, IKeymapSettings& settings, KeymapPlatform platform,
           const LayoutTable& layout, IClock::Duration platformDoubleTapInterval = std::chrono::milliseconds(400));

    /*
     * Matches one key event; the gate is registry.isEnabled(id, context). On Fire the
     * caller runs registry.tryInvoke. If the registry grew since the index was built, the
     * index is rebuilt first, once: that allocates (the documented exception to "no
     * allocation") and increments bindingsGeneration(). Budget: B5.
     */
    [[nodiscard]] MatchResult handle(const KeyEvent& event, const ActionContext& context) noexcept;
    // handle()'s outcome without changing match state (it may rebuild the index, like
    // handle(), so peek and handle always see the same bindings). Budget: B5.
    [[nodiscard]] MatchOutcome peek(const KeyEvent& event, const ActionContext& context) noexcept;
    void reset() noexcept { m_matcher.reset(); }
    [[nodiscard]] std::optional<Stroke> pendingPrefix() const noexcept { return m_matcher.pendingPrefix(); }

    // Copies `layout` and resets the matcher (a pending stroke is dropped).
    void setLayout(const LayoutTable& layout) noexcept;

    // An explicit user change: validates strictly, then stores, then applies. On a
    // validation or store error nothing is applied and the error is returned.
    [[nodiscard]] Result<void> setPreferences(KeymapPreferences preferences);
    // A live reload (the composition root calls it on the UI thread): loads and sanitises.
    // On a load error the current preferences stay and the error is recorded. Never stores.
    void reloadPreferences();

    /*
     * Default shortcuts that modules contribute for actions the keymap does not name (the build
     * commands' table, ADR 0039): a binding here applies only when the resolved keymap has no
     * line for its action (an explicit unbinding counts as a line), and a user override of the
     * action still replaces it. Never stored: the preferences keep only the user's overrides.
     * Replaces the previous defaults, rebuilds the index and increments bindingsGeneration().
     * O(B log B).
     */
    void setModuleDefaults(std::vector<Binding> defaults);
    [[nodiscard]] std::span<const Binding> moduleDefaults() const noexcept { return m_moduleDefaults; }

    // The binding queries rebuild the index first if the registry grew since it was built
    // (as handle() does, incrementing bindingsGeneration()), so a menu or Find Action never
    // reads bindings that miss newly registered actions.
    [[nodiscard]] std::span<const Sequence> shortcutsFor(ActionId id) noexcept;
    [[nodiscard]] std::optional<Sequence> primaryShortcut(ActionId id);
    [[nodiscard]] const BindingIndex& bindings() noexcept;
    [[nodiscard]] std::vector<Sequence> unreachableBindings();
    [[nodiscard]] const LayoutTable& layout() const noexcept { return m_layout; }
    [[nodiscard]] const Keymap& keymap() const noexcept { return m_keymap; }
    [[nodiscard]] const KeymapPreferences& preferences() const noexcept { return m_preferences; }
    [[nodiscard]] KeymapPlatform platform() const noexcept { return m_platform; }
    [[nodiscard]] std::uint64_t bindingsGeneration() const noexcept { return m_bindingsGeneration; }
    [[nodiscard]] const std::optional<Error>& lastSettingsError() const noexcept { return m_lastSettingsError; }

private:
    KeymapService(const ActionRegistry& registry, IKeymapSettings& settings, KeymapPlatform platform,
                  const LayoutTable& layout, IClock::Duration platformInterval, Keymap keymap);

    // Applies already-sanitised preferences: keymap, overrides, interval; rebuilds the index.
    void apply(KeymapPreferences preferences, Keymap keymap);
    void rebuildIndex();
    void ensureIndex() noexcept;

    const ActionRegistry* m_registry;
    IKeymapSettings* m_settings;
    KeymapPlatform m_platform;
    LayoutTable m_layout;
    IClock::Duration m_platformInterval;
    Keymap m_keymap;
    KeymapPreferences m_preferences;
    std::vector<Binding> m_moduleDefaults;
    BindingIndex m_index;
    ShortcutMatcher m_matcher;
    std::uint64_t m_indexRegistryGeneration = 0;
    std::uint64_t m_bindingsGeneration = 0;
    std::optional<Error> m_lastSettingsError;
};

} // namespace frost::input
