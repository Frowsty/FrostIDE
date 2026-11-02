// SPDX-License-Identifier: Apache-2.0
//
// AppServices: the composition root's services for the main window (ADR 0038): the action
// registry, the keymap service over the active keyboard layout, the settings seams with their
// in-memory stores until m1/settings binds them to the registry, the placeholder editor factory and,
// in the GUI, the appearance manager. MainWindow owns one when it is default-constructed; main()
// creates one before the window.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/qt_os_appearance.hpp"
#include "frost/ui/shell/placeholder_editor.hpp"
#include "frost/ui/shell/shell_settings.hpp"
#include "frost/ui/shell/shell_window.hpp"

#include <memory>
#include <optional>
#include <string>

class QApplication;

namespace frost::app {

/*
 * Members are declared in dependency order, so destruction is safe; the window built on them must
 * be destroyed (or torn down) first. GUI thread only; needs a QGuiApplication (the double-tap
 * interval comes from QStyleHints).
 */
class AppServices {
public:
    // Creates the registry and the keymap service: the IntelliJ keymap on the active keyboard
    // layout (en-US when it cannot be read).
    AppServices();
    AppServices(const AppServices&) = delete ("the window holds references into the services");
    AppServices& operator=(const AppServices&) = delete ("the window holds references into the services");
    ~AppServices();

    // Installs the appearance from `resourceDir` (absolute, UTF-8) on `app` (ADR 0015). Errors:
    // those of AppearanceManager::install(); the application keeps Qt's style then.
    [[nodiscard]] Result<ui::InstallReport> installAppearance(QApplication& app, std::string resourceDir);

    // The environment for a ShellWindow; `followActiveLayout` re-reads the OS layout on changes.
    [[nodiscard]] ui::ShellEnvironment environment(bool followActiveLayout = true);

    [[nodiscard]] input::ActionRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] input::KeymapService& keymap() noexcept { return *m_keymap; }
    [[nodiscard]] ui::AppearanceManager* appearance() noexcept { return m_appearance.get(); }

private:
    input::ActionRegistry m_registry;
    input::MemoryKeymapSettings m_keymapSettings;
    std::optional<input::KeymapService> m_keymap;
    ui::InMemoryShellSettings m_shellSettings;
    ui::PlaceholderEditorFactory m_editors;
    ui::InMemoryAppearanceSettings m_appearanceSettings;
    std::unique_ptr<ui::QtOsAppearanceSource> m_os;
    std::unique_ptr<ui::AppearanceManager> m_appearance;
};

} // namespace frost::app
