// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): registerActions, the workbench's actions in the action
// registry and their handlers (split from workbench.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_properties_dialog.hpp"
#include "unity_control.hpp"
#include <QFileDialog>

#include <array>
#include <cstring>
#include <utility>

namespace frost::app {

namespace wa = workbench_actions;

namespace {

constexpr std::string_view kLog = "workbench";

} // namespace

void Workbench::registerActions() {
    registerRefactoringActions();
    // The shell owns Go to Line. Refactoring owns Undo/Redo routing at global document edges.
    static constexpr std::array<std::string_view, 3> kShellOwned{ui::actions::kGoToLine, "editor.undo", "editor.redo"};
    if (auto editor = ui::registerEditorActions(m_registry, m_actionHost, kShellOwned); !editor) {
        log::warn(kLog, "editor actions: {}", editor.error().describe());
    }
    const auto add = [this](std::string_view id, std::string name, std::function<void()> run,
                            std::function<bool()> enabled = {}) {
        input::ActionSpec spec;
        spec.id = std::string(id);
        spec.name = std::move(name);
        spec.group = "File";
        spec.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        spec.handler = [run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) { run(); };
        if (enabled) {
            spec.enabled = [enabled = std::move(enabled)](const input::ActionContext&) { return enabled(); };
        }
        if (auto added = m_registry.add(std::move(spec)); !added) {
            log::warn(kLog, "action {}: {}", id, added.error().describe());
        }
    };
    input::ActionSpec checkUpdate;
    checkUpdate.id = "help.checkForUpdates";
    checkUpdate.name = "Check for Updates...";
    checkUpdate.group = "Help";
    checkUpdate.requiredLevel = input::AvailabilityLevel::L1;
    checkUpdate.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    checkUpdate.handler = [this](const input::ActionContext&, const input::InvokeInfo&) { showUpdateCheck(); };
    if (auto added = m_registry.add(std::move(checkUpdate)); !added)
        log::warn(kLog, "update action: {}", added.error().describe());
    // Save All, Save, Reload All from Disk and Close Project are the navigation workbench's.
    add(wa::kNewProject, "New Project...", [this] { showNewProjectDialog(); });
    add(wa::kOpenFolder, "Open Folder...", [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::Folder); });
    add("file.settings", "Settings...", [this] { showSettings(); }, [this] { return m_window != nullptr; });
    add("window.structure", "Structure", [this] {
        if (m_window)
            m_window->toolWindows().activate("structure");
    });
    add(
        "code.inspectLua", "Inspect Lua Code...", [this] { (void)inspectLuaCode(); },
        [this] { return !m_projectRoot.empty(); });
    input::ActionSpec inspectCpp;
    inspectCpp.id = "code.inspectCpp";
    inspectCpp.name = "Inspect C++ Module...";
    inspectCpp.group = "Code";
    inspectCpp.description =
        "Runs the project's clang-tidy profile over the active source module; asks for trust first.";
    inspectCpp.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    inspectCpp.enabled = [this](const input::ActionContext&) {
        DocumentEditorView* view = currentView();
        return m_window != nullptr && view != nullptr && isCFamilySource(view->path());
    };
    inspectCpp.handler = [this](const input::ActionContext&, const input::InvokeInfo&) {
        DocumentEditorView* view = currentView();
        if (!view || !isCFamilySource(view->path()))
            return;
        const std::string directory(io::parentPath(view->path()));
        inspectCppCodeAsync(directory, [](Result<lang::InspectionReport>) {});
    };
    if (auto added = m_registry.add(std::move(inspectCpp)); !added)
        log::warn(kLog, "C++ inspect action: {}", added.error().describe());
    add(
        "code.exportCppInspectionSarif", "Export C++ Inspection as SARIF...",
        [this] {
            if (!m_window || !m_lastCppInspectionReport)
                return;
            const QString path = QFileDialog::getSaveFileName(m_window, QStringLiteral("Export C++ Inspection"),
                                                              QStringLiteral("inspection.sarif"),
                                                              QStringLiteral("SARIF 2.1.0 (*.sarif);;JSON (*.json)"));
            if (path.isEmpty())
                return;
            const auto exported = exportCppInspectionSarif(path.toUtf8().toStdString());
            if (!exported)
                notify(QStringLiteral("SARIF export failed"),
                       QString::fromUtf8(exported.error().message().data(),
                                         static_cast<qsizetype>(exported.error().message().size())),
                       ui::NotificationKind::Warning);
            else
                notify(QStringLiteral("SARIF exported"), path, ui::NotificationKind::Info);
        },
        [this] { return m_window != nullptr && m_lastCppInspectionReport.has_value(); });
    {
        input::ActionSpec cppBinding;
        cppBinding.id = std::string(ui::actions::kGoToCppBinding);
        cppBinding.name = "Go to C++ Binding";
        cppBinding.group = "Navigate";
        cppBinding.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        cppBinding.enabled = [this](const input::ActionContext&) {
            return currentView() != nullptr && isLuaSource(currentView()->path());
        };
        cppBinding.handler = [this](const input::ActionContext&, const input::InvokeInfo&) { (void)gotoCppBinding(); };
        if (auto added = m_registry.add(std::move(cppBinding)); !added) {
            log::warn(kLog, "action {}: {}", ui::actions::kGoToCppBinding, added.error().describe());
        }
    }
    add(
        wa::kOpenRecent, "Open Recent...", [this] { showOpenRecent(); },
        [this] { return !recentProjects(*m_services.settings.snapshot()).empty(); });
    add(
        "file.projectStructure", "Project Properties...",
        [this] {
            if (!m_window || !m_solution)
                return;
            std::string projPath;
            if (m_explorer) {
                if (const auto projId = m_explorer->currentProject())
                    projPath = m_solution->project(*projId).path;
            }
            if (projPath.empty()) {
                for (const auto& p : m_solution->projects()) {
                    if (p.nodeKind == project::NodeKind::Project && !p.path.empty()) {
                        projPath = p.path;
                        break;
                    }
                }
            }
            if (projPath.empty()) {
                showSolutionPropertiesDialog(
                    m_window, m_services.fs, *m_solution, m_runService ? m_runService->startupProject() : std::nullopt,
                    [this](project::ProjectId id) {
                        if (m_runController)
                            (void)m_runController->setStartupProject(id);
                    },
                    [this] { refreshSolutionExplorerForFsChange(); });
                return;
            }
            std::string activeCfg = "Debug|x64";
            if (const auto idx = m_build->activeConfigurationIndex(); idx && *idx < m_build->configurations().size())
                activeCfg = m_build->configurations()[*idx].name();
            showProjectPropertiesDialog(m_window, m_services.fs, projPath, activeCfg, [this] {
                refreshSolutionExplorerForFsChange();
                startProjectEvaluation(m_projectRoot);
                if (m_clangd)
                    restartClangd();
            });
        },
        [this] { return m_window != nullptr && m_solution != nullptr; });
    add(
        "file.solutionProperties", "Solution Properties...",
        [this] {
            if (!m_window || !m_solution)
                return;
            showSolutionPropertiesDialog(
                m_window, m_services.fs, *m_solution, m_runService ? m_runService->startupProject() : std::nullopt,
                [this](project::ProjectId id) {
                    if (m_runController)
                        (void)m_runController->setStartupProject(id);
                },
                [this] { refreshSolutionExplorerForFsChange(); });
        },
        [this] { return m_window != nullptr && m_solution != nullptr; });
    add(
        "build.configurationManager", "Configuration Manager...",
        [this] {
            if (!m_window || !m_solution)
                return;
            showConfigurationManagerDialog(
                m_window, m_services.fs, *m_solution, static_cast<int>(m_build->activeConfigurationIndex().value_or(0)),
                [this](int idx) {
                    if (idx >= 0)
                        (void)m_build->setActiveConfiguration(static_cast<std::size_t>(idx));
                },
                [this] {
                    refreshSolutionExplorerForFsChange();
                    startProjectEvaluation(m_projectRoot);
                    if (m_clangd)
                        restartClangd();
                });
        },
        [this] { return m_window != nullptr && m_solution != nullptr; });
    // RUN-3: the Run/Debug Configurations dialog over the workspace's run configurations.
    add(
        "run.editConfigurations", "Edit Run/Debug Configurations...", [this] { editRunConfigurations(); },
        [this] { return m_window != nullptr && hasProject() && m_runController != nullptr; });
    add(
        "run.runCompound", "Run Compound Configuration...", [this] { editRunConfigurations(); },
        // Disabled until compound run configurations exist (RUN-5); see launchCompoundConfiguration.
        [] { return false; });
    add(
        "run.selectConfiguration", "Select Run Configuration...", [this] { editRunConfigurations(); },
        [this] { return m_window != nullptr && hasProject() && m_runController != nullptr; });
    add(
        "run.selectDebugConfiguration", "Select Debug Configuration...", [this] { editRunConfigurations(); },
        [this] { return m_window != nullptr && hasProject() && m_runController != nullptr; });
    add(
        "project.manageNuGetPackages", "Manage NuGet Packages...",
        [this] {
            if (!m_window || !m_solution)
                return;
            std::string projPath;
            if (m_explorer) {
                if (const auto projId = m_explorer->currentProject())
                    projPath = m_solution->project(*projId).path;
            }
            if (projPath.empty()) {
                for (const auto& p : m_solution->projects()) {
                    if (p.kind == project::ProjectKind::CSharp && !p.path.empty()) {
                        projPath = p.path;
                        break;
                    }
                }
            }
            if (!projPath.empty())
                showNuGetPackageManagerDialog(m_window, m_services.fs, projPath,
                                              [this] { refreshSolutionExplorerForFsChange(); });
        },
        [this] { return m_window != nullptr && m_solution != nullptr; });
    add(
        "project.addProjectReference", "Add Project Reference...",
        [this] {
            if (!m_window || !m_solution)
                return;
            std::string projPath;
            if (m_explorer) {
                if (const auto projId = m_explorer->currentProject())
                    projPath = m_solution->project(*projId).path;
            }
            if (!projPath.empty())
                showAddProjectReferenceDialog(m_window, m_services.fs, *m_solution, projPath,
                                              [this] { refreshSolutionExplorerForFsChange(); });
        },
        [this] { return m_window != nullptr && m_solution != nullptr; });
    add(
        "vcs.showLocalHistory", "Show Local History...",
        [this] {
            DocumentEditorView* view = currentView();
            if (!m_window || !m_history || !view || view->path().empty())
                return;
            const std::string currentText = view->editor().editor().text().text();
            showLocalHistoryDialog(
                m_window, *m_history, view->path(), currentText, [this, view](const std::string& restored) {
                    const text::Snapshot snap = view->editor().editor().text();
                    const lang::ByteEdit edit{text::ByteRange{ByteOffset{0}, snap.size()}, restored};
                    (void)m_bridge->applyEdits(std::span(&edit, 1), "Restore Local History", ByteOffset{0});
                });
        },
        [this] { return m_window != nullptr && m_history.has_value() && currentView() != nullptr; });
    add(
        "window.nuget", "NuGet Packages",
        [this] {
            if (m_window)
                m_window->toolWindows().toggle(kNuGetToolWindow);
        },
        [this] { return m_window != nullptr; });
    add(
        "window.asmViewer", "IL / Assembly View (unavailable)",
        [this] {
            if (m_window)
                m_window->toolWindows().toggle(kAsmViewerToolWindow);
        },
        [this] { return m_window != nullptr; });
    add(
        "window.unitTests", "Unit Tests",
        [this] {
            if (m_window)
                m_window->toolWindows().toggle(kUnitTestsToolWindow);
        },
        [this] { return m_window != nullptr; });
    add(
        "window.git", "Git",
        [this] {
            if (m_window)
                m_window->toolWindows().activate("gitLog");
        },
        [this] { return m_window != nullptr && m_window->toolWindows().contains("gitLog"); });
    add(
        "tools.runAnything", "Run Anything...",
        [this] {
            if (m_nav)
                static_cast<void>(m_nav->showFindAction());
        },
        [this] { return m_nav != nullptr; });
    {
        input::ActionSpec atMention;
        atMention.id = "agent.atMention";
        atMention.name = "Mention Selection in AI Agent";
        atMention.description = "Requires the AI agent panel (UX-17)";
        atMention.group = "Agent";
        atMention.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        atMention.enabled = [](const input::ActionContext&) { return false; };
        atMention.handler = [](const input::ActionContext&, const input::InvokeInfo&) {};
        if (auto added = m_registry.add(std::move(atMention)); !added) {
            log::warn(kLog, "action agent.atMention: {}", added.error().describe());
        }
    }
    // No "window.services" alias for the Unit Tests window: the Services window is UX-9's, and
    // Alt+8 is bound to window.unitTests in the module defaults.
    add(
        "application.switchVersion", "Switch IDE Version...", [this] { showVersionSwitcher(); },
        [this] { return activeTopWindow() != nullptr; });
}

} // namespace frost::app
