// SPDX-License-Identifier: Apache-2.0
// Workbench terminal integration: project trust, settings, launcher and shell lifetime.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/workbench/file_system_view.hpp"

#include <utility>

namespace frost::app {

void Workbench::attachTerminal() {
    if (m_options.terminalLauncher == nullptr) {
        m_terminalLauncher = std::make_unique<term::RealPtyLauncher>(m_services.launcher);
    }
    ui::TerminalEnvironment env;
    env.launcher = m_options.terminalLauncher ? m_options.terminalLauncher : m_terminalLauncher.get();
    env.appearance = m_appearance.get();
    env.available = [self = QPointer<Workbench>(this)] { return self && !self->m_shutDown && self->m_window; };
    if (m_options.terminalProbe)
        env.probe = *m_options.terminalProbe;
    env.context = [this] {
        return ui::TerminalContext{m_projectRoot, !m_projectRoot.empty() && m_services.trust.isTrusted(m_projectRoot)};
    };
    env.settings = [this] { return term::terminalSettings(*m_services.settings.snapshot()); };
    m_terminal = std::make_unique<ui::TerminalIntegration>(*m_window, std::move(env));
    if (m_explorer) {
        m_explorer->setOpenTerminalHandler([this](const QString& path) {
            const std::string selected = path.toUtf8().toStdString();
            const auto status = m_services.fs.status(selected);
            if (!status || !m_terminal)
                return;
            const std::string folder =
                status->type == io::FileType::Directory ? selected : std::string(io::parentPath(selected));
            static_cast<void>(m_terminal->openInTerminal(folder));
        });
    }
    m_terminal->setFolderProvider([this] {
        std::string selected;
        const auto focused = m_window->toolWindows().focusedWindow();
        if (focused == ui::tool_windows::kProject && m_explorer) {
            selected = m_explorer->currentPath().toUtf8().toStdString();
        } else if (focused == ui::kFileSystemToolWindow && m_nav && m_nav->fileSystemView()) {
            selected = m_nav->fileSystemView()->selectedPath();
        }
        if (selected.empty())
            return m_projectRoot;
        const auto status = m_services.fs.status(selected);
        if (!status)
            return m_projectRoot;
        if (status->type == io::FileType::Directory)
            return selected;
        return std::string(io::parentPath(selected));
    });
}

void Workbench::stopTerminal() {
    if (m_explorer)
        m_explorer->setOpenTerminalHandler({});
    if (m_terminal) {
        if (auto* window = m_terminal->toolWindow())
            window->shutdown();
    }
}

} // namespace frost::app
