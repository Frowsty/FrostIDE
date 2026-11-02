// SPDX-License-Identifier: Apache-2.0
//
// AppServices (frost/app/app_services.hpp).

#include "frost/app/app_services.hpp"

#include "frost/core/assert.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/file_system.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>

#include <chrono>
#include <utility>

namespace frost::app {

AppServices::AppServices() : m_editors(io::realFileSystem()) {
    // The thread's active layout; en-US when it cannot be read (ADR 0016 decision 12).
    Result<input::LayoutTable> active = input::readActiveLayout();
    const input::LayoutTable layout =
        active ? std::move(*active) : input::simulatedLayout(input::SimulatedLayoutId::EnUs);
    const auto interval = std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    Result<input::KeymapService> keymap =
        input::KeymapService::create(m_registry, m_keymapSettings, input::currentKeymapPlatform(), layout, interval);
    // create() fails only when the embedded keymap does not parse, which its tests rule out.
    FROST_ASSERT(keymap.has_value());
    m_keymap.emplace(std::move(*keymap));
}

AppServices::~AppServices() = default;

Result<ui::InstallReport> AppServices::installAppearance(QApplication& app, std::string resourceDir) {
    if (m_appearance != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the appearance is already installed");
    }
    m_os = std::make_unique<ui::QtOsAppearanceSource>();
    auto manager = std::make_unique<ui::AppearanceManager>(
        ui::AppearanceEnvironment{io::realFileSystem(), std::move(resourceDir), m_appearanceSettings, m_os.get()});
    Result<ui::InstallReport> report = manager->install(app);
    if (report) {
        m_appearance = std::move(manager);
    }
    return report;
}

ui::ShellEnvironment AppServices::environment(bool followActiveLayout) {
    ui::ShellEnvironment environment{m_registry, *m_keymap, m_shellSettings, m_editors};
    environment.appearance = m_appearance.get();
    environment.appearanceSettings = m_appearance != nullptr ? &m_appearanceSettings : nullptr;
    environment.followActiveLayout = followActiveLayout;
    return environment;
}

} // namespace frost::app
