// SPDX-License-Identifier: Apache-2.0
// Per-window hosted review composition; installed CLI requests are explicit and remain bound to project/trust.
#include "frost/app/workbench.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "hosting_panel.hpp"
namespace frost::app {
void Workbench::attachHosting() {
    if (!m_window)
        return;
    const QPointer<Workbench> owner(this);
    ui::ToolWindowSpec spec;
    spec.id = "pullRequests";
    spec.title = QStringLiteral("Pull Requests");
    spec.side = ui::ToolWindowSide::Right;
    spec.createContent = [owner]() -> QWidget* {
        if (!owner)
            return new QWidget;
        HostingEnvironment environment;
        environment.launcher = &owner->m_services.launcher;
        environment.root = [owner] { return owner ? owner->projectRoot() : std::string{}; };
        auto* trust = &owner->m_services.trust;
        environment.trusted = [trust](const std::string& root) { return !root.empty() && trust->isTrusted(root); };
        environment.trustRevision = [trust](const std::string& root) { return trust->snapshot(root).revision; };
        auto* panel = new HostingPanel(std::move(environment));
        QObject::connect(owner, &Workbench::projectChanged, panel, [panel] { panel->invalidate(); });
        return panel;
    };
    (void)m_window->toolWindows().add(std::move(spec));
    if (!m_registry.find("vcs.pullRequests")) {
        input::ActionSpec action;
        action.id = "vcs.pullRequests";
        action.name = "Pull Requests";
        action.group = "VCS";
        action.handler = [owner](const input::ActionContext&, const input::InvokeInfo&) {
            if (owner && owner->m_window)
                owner->m_window->toolWindows().activate("pullRequests");
        };
        (void)m_registry.add(std::move(action));
    }
}
} // namespace frost::app
