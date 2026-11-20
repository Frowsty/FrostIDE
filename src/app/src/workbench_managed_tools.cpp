// SPDX-License-Identifier: Apache-2.0
#include "frost/app/workbench.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/dialog_chrome.hpp"

#include "managed_tools_window.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>

namespace frost::app {
ManagedToolsContext Workbench::managedToolsContext() {
    QPointer<Workbench> owner(this);
    ManagedToolsContext context;
    context.launcher = &m_services.launcher;
    context.fs = &m_services.fs;
    context.trust = &m_services.trust;
    context.root = [owner] { return owner && !owner->m_shutDown ? owner->m_projectRoot : std::string(); };
    context.generation = [owner] { return owner ? owner->m_projectEditAdmissionGeneration : 0; };
    context.dotnet = [owner] {
        return owner && !owner->m_shutDown
                   ? owner->m_services.settings.snapshot()->get(settings::key::toolsManagedDotnetPath)
                   : std::string();
    };
    context.projects = [owner] {
        std::vector<std::string> projects;
        if (owner && owner->m_solution)
            for (const auto& entry : owner->m_solution->projects())
                if (entry.kind == project::ProjectKind::CSharp && !entry.path.empty())
                    projects.push_back(entry.path);
        return projects;
    };
    context.prepare = [owner]() -> Result<void> {
        if (!owner || owner->m_shutDown)
            return fail(ErrorCode::Cancelled, "Workspace closed");
        const auto saved = owner->saveAllDocuments();
        if (!saved.failed.empty() || !saved.skipped.empty())
            return fail(ErrorCode::Busy, "Save every affected document before running managed tools");
        return {};
    };
    context.apply = [owner](const document::RefactoringPlan& plan) -> Result<document::RefactoringOutcome> {
        if (!owner || owner->m_shutDown)
            return fail(ErrorCode::Cancelled, "Workspace closed");
        auto applied = owner->applyRefactoring(plan);
        if (applied && applied->status != document::RefactoringStatus::PendingRecovery)
            owner->refreshSolutionExplorerForFsChange();
        return applied;
    };
    context.openLocation = [owner](const std::string& path, int line) {
        if (owner && !owner->m_shutDown)
            owner->openLocation(QString::fromStdString(path), line, 1);
    };
    return context;
}
void Workbench::showManagedPackages(const std::string& projectPath) {
    if (!m_window || m_shutDown)
        return;
    auto context = managedToolsContext();
    auto projects = context.projects;
    context.projects = [projects, projectPath] {
        auto paths = projects ? projects() : std::vector<std::string>();
        if (!projectPath.empty()) {
            std::erase(paths, projectPath);
            paths.insert(paths.begin(), projectPath);
        }
        return paths;
    };
    QDialog dialog(m_window);
    dialog.setWindowTitle(tr("NuGet Packages"));
    dialog.resize(1100, 750);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(createManagedPackagesWidget(&dialog, std::move(context)));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    ui::installDialogChrome(&dialog, true);
    dialog.exec();
}
} // namespace frost::app
