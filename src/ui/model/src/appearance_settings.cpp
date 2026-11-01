// SPDX-License-Identifier: Apache-2.0
//
// Appearance settings validation and the in-memory store (frost/ui/model/appearance_settings.hpp).

#include "frost/ui/model/appearance_settings.hpp"

#include "frost/core/assert.hpp"
#include "frost/ui/model/theme.hpp"

#include <format>

namespace frost::ui {

Result<void> validate(const AppearanceSettings& settings) {
    if (settings.theme != kSyncWithOs && !isValidThemeId(settings.theme)) {
        return fail(ErrorCode::InvalidArgument, std::format("ui.theme: invalid theme id '{}'", settings.theme));
    }
    const std::pair<std::string_view, const std::string*> syncFields[] = {
        {"ui.theme.syncLight", &settings.syncLightTheme},
        {"ui.theme.syncDark", &settings.syncDarkTheme},
        {"ui.theme.syncHighContrast", &settings.syncHighContrastTheme},
    };
    for (const auto& [key, value] : syncFields) {
        if (!isValidThemeId(*value) || *value == kSyncWithOs) {
            return fail(ErrorCode::InvalidArgument, std::format("{}: invalid theme id '{}'", key, *value));
        }
    }
    if (settings.density != Density::Normal && settings.density != Density::Compact) {
        return fail(ErrorCode::InvalidArgument, "ui.compactMode: unknown density");
    }
    if (settings.zoomPercent < kMinZoomPercent || settings.zoomPercent > kMaxZoomPercent) {
        return fail(ErrorCode::InvalidArgument, std::format("ui.zoom: {}% is outside [{}, {}]", settings.zoomPercent,
                                                            kMinZoomPercent, kMaxZoomPercent));
    }
    return {};
}

InMemoryAppearanceSettings::InMemoryAppearanceSettings(AppearanceSettings initial) : m_settings(std::move(initial)) {
    FROST_PRE(validate(m_settings).has_value());
}

Result<void> InMemoryAppearanceSettings::store(const AppearanceSettings& settings) {
    if (m_notifying) {
        return fail(ErrorCode::Busy, "appearance settings stored from inside a change listener");
    }
    if (auto valid = validate(settings); !valid) {
        return valid;
    }
    m_settings = settings;
    m_notifying = true;
    // Listeners see the committed value; a copy, so a listener cannot observe a later change mid-loop.
    const AppearanceSettings committed = m_settings;
    m_listeners.notify(committed);
    m_notifying = false;
    return {};
}

ListenerToken InMemoryAppearanceSettings::subscribe(std::function<void(const AppearanceSettings&)> listener) {
    return m_listeners.add(std::move(listener));
}

} // namespace frost::ui
