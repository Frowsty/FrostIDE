// SPDX-License-Identifier: Apache-2.0
// Settings keymap page: staged shortcuts and conflict feedback.
#pragma once
#include "frost/input/keymap_service.hpp"
#include "frost/settings/store.hpp"

#include <QWidget>

#include <functional>
#include <optional>

class QLineEdit;
class QLabel;
class QTreeWidget;
namespace frost::app {
class SettingsKeymapPage final : public QWidget {
public:
    SettingsKeymapPage(const input::ActionRegistry& registry, input::KeymapService& keymap,
                       std::function<void()> changed, QWidget* parent = nullptr);
    void load(const settings::SettingsDocument& document, const settings::Registry& registry);
    void setKeymapId(std::string_view keymapId);
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool matches(const QString& query) const;
    void filter(const QString& query);
    [[nodiscard]] std::optional<settings::ConditionalSettingEdit> edit() const;

private:
    void rebuild();
    void selected();
    void setShortcut();
    void removeShortcuts();
    void resetShortcut();
    void change(std::optional<std::vector<input::Sequence>> sequences);
    input::BindingIndex index() const;
    const input::ActionRegistry& m_registry;
    input::KeymapService& m_keymap;
    std::function<void()> m_changed;
    input::KeymapPreferences m_preferences;
    settings::RecordList m_records;
    settings::RecordList m_original;
    std::optional<std::string> m_expected;
    bool m_valid = false;
    QTreeWidget* m_actions = nullptr;
    QLineEdit* m_shortcut = nullptr;
    QLabel* m_message = nullptr;
    QString m_filter;
};
} // namespace frost::app
