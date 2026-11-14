// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/core/error.hpp"
#include <functional>
#include <string>
#include <string_view>
class QWidget;
namespace frost::io { class IFileSystem; }
namespace frost::settings { class SettingsStore; }
namespace frost::build { class CMakeService; }
namespace frost::app {
// Appends one preset while keeping all pre-existing JSON values and bytes intact.
[[nodiscard]] Result<std::string> appendCMakeKitPreset(std::string original, std::string_view presetJson);
void showCMakeKitDialog(io::IFileSystem&, settings::SettingsStore&, build::CMakeService&, QWidget&, std::function<bool()> stillCurrent = {});
}
