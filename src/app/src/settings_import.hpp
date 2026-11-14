// SPDX-License-Identifier: Apache-2.0
// Clean-room import of the documented open .vssettings XML container; arbitrary package XML is not executed.
#pragma once
#include "frost/settings/store.hpp"
#include <string>
#include <vector>
class QWidget;
namespace frost::app {
struct ImportedFontCategory {
    std::string id, font;
    std::optional<std::int64_t> size;
    settings::RecordList colors;
};
struct ImportedVisualStudioSettings {
    std::vector<ImportedFontCategory> fonts;
    settings::RecordList shortcuts;
    std::vector<std::string> warnings;
};
// XML is capped at 1MiB/50k tokens/depth64; DTD/entities and malformed XML are refused.
// O(bytes), no file/process/network access. Each font category remains separate for explicit selection.
[[nodiscard]] Result<ImportedVisualStudioSettings> parseVisualStudioSettings(std::string_view xml);
// Captures application raw expectations for preview. Unrelated keymap overrides survive import.
// Apply the returned edits exactly once through SettingsStore::applyConditional after user review.
[[nodiscard]] Result<std::vector<settings::ConditionalSettingEdit>> prepareVisualStudioImport(
    settings::SettingsStore& store, const ImportedVisualStudioSettings& imported, std::size_t category);
// Opens a bounded file picker, explicit category selector and reviewable import preview.
// Successful Apply uses one atomic SettingsStore replacement; Cancel has no side effects.
bool showSettingsImport(settings::SettingsStore& store, QWidget* parent);
} // namespace frost::app
