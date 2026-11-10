// SPDX-License-Identifier: Apache-2.0
// Application-scoped Settings dialog over the typed registry and SettingsStore.
#pragma once

#include "frost/input/keymap_service.hpp"
#include "frost/io/file_system.hpp"
#include "frost/settings/store.hpp"

#include <QDialog>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace frost::app {
class SettingsKeymapPage;
class SettingsLuaPage;

// The Lua page's project (LUA-4): its .luarc.json is edited in place. Without `fs` the dialog has
// no Lua page; an empty root shows the page without a project.
struct SettingsLuaContext {
    io::IFileSystem* fs = nullptr;
    std::string projectRoot;
    std::function<void()> applied; // After the page wrote .luarc.json (to tell the running LuaLS).
};

// GUI thread only. The store outlives this dialog. Apply writes only modified keys;
// Cancel discards edits made since the last successful Apply.
class SettingsDialog final : public QDialog {
public:
    SettingsDialog(settings::SettingsStore& store, const input::ActionRegistry& registry, input::KeymapService& keymap,
                   QWidget* parent = nullptr, SettingsLuaContext lua = {});
    ~SettingsDialog() override;

private:
    struct Row;
    void build();
    void reload();
    void refreshSearch();
    void changed(std::size_t index);
    void resetRow(std::size_t index);
    bool apply();
    void refreshApply();
    settings::SettingsStore& m_store;
    const input::ActionRegistry& m_registry;
    input::KeymapService& m_keymap;
    SettingsKeymapPage* m_keymapPage = nullptr;
    SettingsLuaContext m_luaContext;
    SettingsLuaPage* m_luaPage = nullptr; // Category row and stack page kPages.size(), when present.
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
