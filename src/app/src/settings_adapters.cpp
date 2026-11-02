// SPDX-License-Identifier: Apache-2.0
//
// The workbench's settings adapters and the recent-projects list (frost/app/settings_adapters.hpp).

#include "frost/app/settings_adapters.hpp"

#include "frost/input/key.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace frost::app {

namespace key = settings::key;
using settings::FieldValue;
using settings::Record;
using settings::RecordField;
using settings::RecordList;
using settings::Scope;
using settings::StringList;

namespace {

// The field `name` of `record` as T, or nullopt.
template<class T>
std::optional<T> field(const Record& record, std::string_view name) {
    for (const RecordField& f : record) {
        if (f.name == name) {
            if (const T* value = std::get_if<T>(&f.value)) {
                return *value;
            }
        }
    }
    return std::nullopt;
}

} // namespace

// --- StoreShellSettings ---

Result<ui::ShellLayout> StoreShellSettings::load() const {
    const auto snapshot = m_store.snapshot();
    ui::ShellLayout layout;
    layout.toolbarVisible = snapshot->get(key::uiToolbarVisible);
    layout.statusBarVisible = snapshot->get(key::uiStatusBarVisible);
    for (const Record& record : snapshot->get(key::uiToolWindows)) {
        const auto id = field<std::string>(record, "id");
        const auto visible = field<bool>(record, "visible");
        if (!id || !visible) {
            continue;
        }
        ui::ToolWindowLayout& entry = layout.entry(*id);
        entry.visible = *visible;
        entry.size =
            static_cast<int>(std::clamp<std::int64_t>(field<std::int64_t>(record, "size").value_or(0), 0, 100000));
    }
    return layout;
}

Result<void> StoreShellSettings::store(const ui::ShellLayout& layout) {
    const auto snapshot = m_store.snapshot();
    if (snapshot->get(key::uiToolbarVisible) != layout.toolbarVisible) {
        if (auto ok = m_store.set(Scope::Application, key::uiToolbarVisible, layout.toolbarVisible); !ok) {
            return ok;
        }
    }
    if (snapshot->get(key::uiStatusBarVisible) != layout.statusBarVisible) {
        if (auto ok = m_store.set(Scope::Application, key::uiStatusBarVisible, layout.statusBarVisible); !ok) {
            return ok;
        }
    }
    RecordList windows;
    for (const ui::ToolWindowLayout& window : layout.toolWindows) {
        windows.push_back(settings::makeRecord({RecordField{"id", FieldValue(window.id)},
                                                RecordField{"visible", FieldValue(window.visible)},
                                                RecordField{"size", FieldValue(std::int64_t{window.size})}}));
    }
    if (snapshot->get(key::uiToolWindows) == windows) {
        return {};
    }
    return m_store.set(Scope::Application, key::uiToolWindows, std::move(windows));
}

// --- StoreKeymapSettings ---

Result<input::KeymapPreferences> StoreKeymapSettings::load() const {
    const auto snapshot = m_store.snapshot();
    input::KeymapPreferences preferences;
    preferences.keymapId = snapshot->get(key::keymapId);
    preferences.doubleTapInterval = std::chrono::milliseconds(snapshot->get(key::keymapDoubleTapIntervalMs));
    for (const Record& record : snapshot->get(key::keymapOverrides)) {
        const auto action = field<std::string>(record, "action");
        const auto shortcuts = field<StringList>(record, "shortcuts");
        if (!action || !shortcuts) {
            continue;
        }
        input::ActionOverride override{*action, {}};
        bool valid = true;
        for (const std::string& text : *shortcuts) {
            auto sequence = input::parseSequence(text, input::SyntaxMode::Strict);
            if (!sequence) {
                valid = false; // A hand-edited entry that does not parse is skipped whole.
                break;
            }
            override.sequences.push_back(std::move(*sequence));
        }
        if (valid) {
            preferences.overrides.push_back(std::move(override));
        }
    }
    return preferences;
}

Result<void> StoreKeymapSettings::store(const input::KeymapPreferences& preferences) {
    if (auto ok = m_store.set(Scope::Application, key::keymapId, preferences.keymapId); !ok) {
        return ok;
    }
    const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(preferences.doubleTapInterval);
    if (auto ok = m_store.set(Scope::Application, key::keymapDoubleTapIntervalMs, std::int64_t{interval.count()});
        !ok) {
        return ok;
    }
    RecordList overrides;
    for (const input::ActionOverride& override : preferences.overrides) {
        StringList shortcuts;
        for (const input::Sequence& sequence : override.sequences) {
            shortcuts.push_back(input::formatSequence(sequence));
        }
        overrides.push_back(settings::makeRecord(
            {RecordField{"action", FieldValue(override.actionId)}, RecordField{"shortcuts", FieldValue(shortcuts)}}));
    }
    return m_store.set(Scope::Application, key::keymapOverrides, std::move(overrides));
}

// --- StoreAppearanceSettings ---

StoreAppearanceSettings::StoreAppearanceSettings(settings::SettingsStore& store) : m_store(store) { m_last = load(); }

ui::AppearanceSettings StoreAppearanceSettings::load() const {
    const auto snapshot = m_store.snapshot();
    ui::AppearanceSettings defaults;
    ui::AppearanceSettings out = defaults;
    // Field by field, so one bad value keeps the others (the interface promises a valid result).
    const auto accept = [&](auto apply) {
        ui::AppearanceSettings candidate = out;
        apply(candidate);
        if (ui::validate(candidate)) {
            out = std::move(candidate);
        }
    };
    accept([&](ui::AppearanceSettings& s) { s.theme = snapshot->get(key::uiTheme); });
    accept([&](ui::AppearanceSettings& s) { s.syncLightTheme = snapshot->get(key::uiThemeSyncLight); });
    accept([&](ui::AppearanceSettings& s) { s.syncDarkTheme = snapshot->get(key::uiThemeSyncDark); });
    accept([&](ui::AppearanceSettings& s) { s.syncHighContrastTheme = snapshot->get(key::uiThemeSyncHighContrast); });
    accept([&](ui::AppearanceSettings& s) {
        s.density = snapshot->get(key::uiCompactMode) ? ui::Density::Compact : ui::Density::Normal;
    });
    accept([&](ui::AppearanceSettings& s) {
        s.zoomPercent = static_cast<std::uint16_t>(std::clamp<std::int64_t>(snapshot->get(key::uiZoom), 0, 1000));
    });
    return out;
}

Result<void> StoreAppearanceSettings::store(const ui::AppearanceSettings& settings) {
    if (m_notifying) {
        return fail(ErrorCode::Busy, "the appearance settings cannot change while their listeners run");
    }
    if (auto valid = ui::validate(settings); !valid) {
        return valid;
    }
    const auto write = [&](auto k, auto value) -> Result<void> {
        return m_store.set(Scope::Application, k, std::move(value));
    };
    if (auto ok = write(key::uiTheme, settings.theme); !ok) {
        return ok;
    }
    if (auto ok = write(key::uiThemeSyncLight, settings.syncLightTheme); !ok) {
        return ok;
    }
    if (auto ok = write(key::uiThemeSyncDark, settings.syncDarkTheme); !ok) {
        return ok;
    }
    if (auto ok = write(key::uiThemeSyncHighContrast, settings.syncHighContrastTheme); !ok) {
        return ok;
    }
    if (auto ok = write(key::uiCompactMode, settings.density == ui::Density::Compact); !ok) {
        return ok;
    }
    if (auto ok = write(key::uiZoom, std::int64_t{settings.zoomPercent}); !ok) {
        return ok;
    }
    storeChanged();
    return {};
}

ui::ListenerToken StoreAppearanceSettings::subscribe(std::function<void(const ui::AppearanceSettings&)> listener) {
    return m_listeners.add(std::move(listener));
}

void StoreAppearanceSettings::storeChanged() {
    ui::AppearanceSettings now = load();
    if (now == m_last || m_notifying) {
        return;
    }
    m_last = now;
    m_notifying = true;
    m_listeners.notify(now);
    m_notifying = false;
}

// --- Recent projects ---

std::vector<std::string> recentProjects(const settings::SettingsSnapshot& snapshot) {
    return snapshot.get(key::generalRecentProjects);
}

Result<void> addRecentProject(settings::SettingsStore& store, std::string_view path) {
    StringList list = recentProjects(*store.snapshot());
    const std::string wanted = lang::documentKey(path);
    std::erase_if(list, [&](const std::string& entry) { return lang::documentKey(entry) == wanted; });
    list.insert(list.begin(), std::string(path));
    if (list.size() > kMaxRecentProjects) {
        list.resize(kMaxRecentProjects);
    }
    return store.set(Scope::Application, key::generalRecentProjects, std::move(list));
}

Result<void> removeRecentProject(settings::SettingsStore& store, std::string_view path) {
    StringList list = recentProjects(*store.snapshot());
    const std::string wanted = lang::documentKey(path);
    const auto removed =
        std::erase_if(list, [&](const std::string& entry) { return lang::documentKey(entry) == wanted; });
    if (removed == 0) {
        return {};
    }
    return store.set(Scope::Application, key::generalRecentProjects, std::move(list));
}

} // namespace frost::app
