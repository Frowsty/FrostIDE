// SPDX-License-Identifier: Apache-2.0
//
// The settings adapters of the workbench (W1, ADR 0044): the shell's, the keymap's and the
// appearance's settings seams (IShellSettings, IKeymapSettings, IAppearanceSettings) over the
// typed settings store (frost::settings, ADR 0021), and the recent-projects list
// (general.recentProjects) that File > Open Recent and the Welcome screen show.
/*
 * Keys. Shell layout: ui.toolbar.visible, ui.statusBar.visible and ui.toolWindows (records {id,
 * visible, size}). Keymap: keymap.id, keymap.doubleTapIntervalMs and keymap.overrides (records
 * {action, shortcuts} in frost::input's Strict syntax). Appearance: ui.theme, ui.theme.sync*,
 * ui.compactMode and ui.zoom. Everything is written to the application scope.
 *
 * Reads take the store's current snapshot; a value the store holds but the seam cannot use (an
 * override whose shortcut does not parse, a theme id validate() refuses) is skipped field by field,
 * so a hand-edited settings file never stops the shell from starting.
 *
 * GUI thread only (the stores are thread-safe, the listener lists are not). The store outlives the
 * adapters.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/model/listener.hpp"
#include "frost/ui/shell/shell_settings.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

// The shell layout over ui.toolbar.visible, ui.statusBar.visible and ui.toolWindows.
class StoreShellSettings final : public ui::IShellSettings {
public:
    explicit StoreShellSettings(settings::SettingsStore& store) noexcept : m_store(store) {}
    [[nodiscard]] Result<ui::ShellLayout> load() const override;
    // Writes the three keys (one file write each). Errors: the store's.
    [[nodiscard]] Result<void> store(const ui::ShellLayout& layout) override;

private:
    settings::SettingsStore& m_store;
};

// The keymap preferences over keymap.id, keymap.doubleTapIntervalMs and keymap.overrides.
class StoreKeymapSettings final : public input::IKeymapSettings {
public:
    explicit StoreKeymapSettings(settings::SettingsStore& store) noexcept : m_store(store) {}
    [[nodiscard]] Result<input::KeymapPreferences> load() const override;
    // Errors: the store's (InvalidArgument for an unknown keymap id).
    [[nodiscard]] Result<void> store(const input::KeymapPreferences& preferences) override;

private:
    settings::SettingsStore& m_store;
};

/*
 * The appearance settings over ui.theme, ui.theme.sync*, ui.compactMode and ui.zoom. store()
 * validates, writes and notifies; a change that reached the store some other way (the settings
 * file edited) is passed on by storeChanged(), which the workbench calls on the GUI thread.
 */
class StoreAppearanceSettings final : public ui::IAppearanceSettings {
public:
    explicit StoreAppearanceSettings(settings::SettingsStore& store);
    [[nodiscard]] ui::AppearanceSettings load() const override;
    [[nodiscard]] Result<void> store(const ui::AppearanceSettings& settings) override;
    [[nodiscard]] ui::ListenerToken subscribe(std::function<void(const ui::AppearanceSettings&)> listener) override;
    // Notifies the listeners when load() differs from what they last saw.
    void storeChanged();

private:
    settings::SettingsStore& m_store;
    ui::ListenerList<const ui::AppearanceSettings&> m_listeners;
    ui::AppearanceSettings m_last;
    bool m_notifying = false;
};

// The most recently opened projects (solutions and folders), newest first, from `snapshot`.
[[nodiscard]] std::vector<std::string> recentProjects(const settings::SettingsSnapshot& snapshot);
// How many entries addRecentProject keeps.
inline constexpr std::size_t kMaxRecentProjects = 20;
// Moves `path` (absolute) to the front of general.recentProjects (an equal path by document key,
// ASCII case ignored on Windows, is replaced), keeping kMaxRecentProjects. Errors: the store's.
[[nodiscard]] Result<void> addRecentProject(settings::SettingsStore& store, std::string_view path);
// Removes `path` from the list (no-op when absent). Errors: the store's.
[[nodiscard]] Result<void> removeRecentProject(settings::SettingsStore& store, std::string_view path);

} // namespace frost::app
