// SPDX-License-Identifier: Apache-2.0
// Application-scoped Settings dialog over the typed registry and SettingsStore.
#pragma once

#include "frost/input/keymap_service.hpp"
#include "frost/settings/store.hpp"

#include <QDialog>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace frost::app {
class SettingsKeymapPage;

// GUI thread only. The store outlives this dialog. Apply writes only modified keys;
// Cancel discards edits made since the last successful Apply.
class SettingsDialog final : public QDialog {
public:
    SettingsDialog(settings::SettingsStore& store, const input::ActionRegistry& registry, input::KeymapService& keymap,
                   QWidget* parent = nullptr);
    ~SettingsDialog() override;

private:
    struct Row;
    void build();
    void reload();
    void refreshSearch();
    void changed(std::size_t index);
    void resetRow(std::size_t index);
    bool apply();
    settings::SettingsStore& m_store;
    const input::ActionRegistry& m_registry;
    input::KeymapService& m_keymap;
    SettingsKeymapPage* m_keymapPage = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_apply = nullptr;
    std::vector<std::unique_ptr<Row>> m_rows;
    bool m_loading = false;
    bool m_loaded = false;
};
} // namespace frost::app
