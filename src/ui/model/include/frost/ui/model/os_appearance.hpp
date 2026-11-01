// SPDX-License-Identifier: Apache-2.0
//
// The operating system's appearance (light or dark colour scheme, high contrast) and how
// "Sync with OS" turns it into a theme id (ADR 0015 decision 5). The Qt-based source lives in
// frost/ui/qt_os_appearance.hpp; this header holds the Qt-free parts: the source interface,
// the resolution truth table, and the Windows and Linux readers. Qt-free.
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/model/listener.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

enum class ColorScheme : std::uint8_t { Unknown, Light, Dark };

struct OsAppearance {
    ColorScheme scheme = ColorScheme::Unknown;
    bool highContrast = false;

    friend constexpr bool operator==(OsAppearance, OsAppearance) noexcept = default;
};

// A source of the OS appearance with change notification. GUI thread only; passed by
// reference, never deleted through this interface.
class IOsAppearanceSource {
public:
    [[nodiscard]] virtual OsAppearance current() const = 0;
    // Called on the GUI thread whenever current() changes; destroying the token unsubscribes.
    [[nodiscard]] virtual ListenerToken subscribe(std::function<void(OsAppearance)> listener) = 0;

protected:
    IOsAppearanceSource() = default;
    IOsAppearanceSource(const IOsAppearanceSource&) = default;
    IOsAppearanceSource& operator=(const IOsAppearanceSource&) = default;
    ~IOsAppearanceSource() = default;
};

/*
 * The theme id the settings select under `os`:
 *   settings.theme is not kSyncWithOs -> settings.theme;
 *   os.highContrast                   -> syncHighContrastTheme;
 *   os.scheme == Dark                 -> syncDarkTheme;
 *   os.scheme == Light                -> syncLightTheme;
 *   os.scheme == Unknown              -> syncDarkTheme.
 * Returns a copy, so a temporary `settings` is safe. Pure, O(1).
 */
[[nodiscard]] std::string resolveThemeId(const AppearanceSettings& settings, OsAppearance os);

// The scheme of `primary` when it is definite, else that of `secondary`; high contrast when
// either reports it. Pure.
[[nodiscard]] constexpr OsAppearance combineAppearance(OsAppearance primary, OsAppearance secondary) noexcept {
    return {primary.scheme != ColorScheme::Unknown ? primary.scheme : secondary.scheme,
            primary.highContrast || secondary.highContrast};
}

#ifdef _WIN32
/*
 * Reads the Windows apps colour mode and high-contrast state through the wide APIs:
 * RegGetValueW(HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize,
 * AppsUseLightTheme, REG_DWORD): 0 is Dark, non-zero Light, an absent value Light (the Windows
 * default), any other failure Unknown; SystemParametersInfoW(SPI_GETHIGHCONTRAST) for
 * HCF_HIGHCONTRASTON. Thread-safe; about 20 us.
 */
[[nodiscard]] OsAppearance readWindowsAppearance() noexcept;
#endif

/*
 * A fallback below Qt's own Linux detection: a case-insensitive look at GTK_THEME (read through
 * `getenv`). A value ending in ":dark" or containing "-dark" is Dark; one containing
 * "highcontrast" sets highContrast; everything else (and an unset variable) is Unknown. Pure
 * apart from `getenv`.
 */
[[nodiscard]] OsAppearance
appearanceFromLinuxEnvironment(frost::function_ref<std::optional<std::string>(std::string_view)> getenv);

} // namespace frost::ui
