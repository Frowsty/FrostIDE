// SPDX-License-Identifier: Apache-2.0
//
// readWindowsAppearance (frost/ui/model/os_appearance.hpp): the apps colour mode from the
// Personalize registry key and the high-contrast flag, through wide Win32 APIs only.

#include "frost/ui/model/os_appearance.hpp"

#include <windows.h>

namespace frost::ui {

OsAppearance readWindowsAppearance() noexcept {
    OsAppearance result;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (status == ERROR_SUCCESS) {
        result.scheme = value == 0 ? ColorScheme::Dark : ColorScheme::Light;
    } else if (status == ERROR_FILE_NOT_FOUND) {
        // No value (or no key): Windows uses the light apps mode.
        result.scheme = ColorScheme::Light;
    }
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE) {
        result.highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return result;
}

} // namespace frost::ui
