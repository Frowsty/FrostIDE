// SPDX-License-Identifier: Apache-2.0
//
// Sync-with-OS resolution and the Linux GTK_THEME fallback (frost/ui/model/os_appearance.hpp).

#include "frost/ui/model/os_appearance.hpp"

#include <algorithm>
#include <cctype>

namespace frost::ui {

std::string resolveThemeId(const AppearanceSettings& settings, OsAppearance os) {
    if (settings.theme != kSyncWithOs) {
        return settings.theme;
    }
    if (os.highContrast) {
        return settings.syncHighContrastTheme;
    }
    switch (os.scheme) {
    case ColorScheme::Light: return settings.syncLightTheme;
    case ColorScheme::Dark:
    case ColorScheme::Unknown: break;
    }
    return settings.syncDarkTheme;
}

OsAppearance appearanceFromLinuxEnvironment(frost::function_ref<std::optional<std::string>(std::string_view)> getenv) {
    const std::optional<std::string> value = getenv("GTK_THEME");
    if (!value) {
        return {};
    }
    std::string lower = *value;
    std::ranges::transform(lower, lower.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    OsAppearance result;
    if (lower.ends_with(":dark") || lower.find("-dark") != std::string::npos) {
        result.scheme = ColorScheme::Dark;
    }
    result.highContrast = lower.find("highcontrast") != std::string::npos;
    return result;
}

} // namespace frost::ui
