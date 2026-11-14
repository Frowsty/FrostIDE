// SPDX-License-Identifier: Apache-2.0
// REF-8 ordinary source-file Move reuses the actual membership/sidecar transaction producer.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QFileDialog>
#include <QMessageBox>
namespace frost::app {
void Workbench::moveCppProjectSource() {
    auto* view = m_actionHost.active();
    if (!view || view->path().empty() || m_projectRoot.empty())
        return;
    const auto oldPath = view->path(), root = m_projectRoot;
    const auto target = QFileDialog::getSaveFileName(m_window, QStringLiteral("Move source file"),
                                                     QString::fromStdString(oldPath), QStringLiteral("All files (*)"));
    if (target.isEmpty())
        return;
    if (m_projectRoot != root || m_shutDown) {
        QMessageBox::warning(m_window, QStringLiteral("Move source"),
                             QStringLiteral("The project changed; request Move again."));
        return;
    }
    const auto destination = m_documentRegistry->canonicalize(target.toStdString());
    if (destination == oldPath)
        return;
    checkFrostEngineAssetReferences(oldPath, "Moving");
    auto plan = prepareProjectSourceRename(oldPath, destination);
    if (!plan) {
        QMessageBox::warning(m_window, QStringLiteral("Move source refused"),
                             QString::fromStdString(plan.error().message()));
        return;
    }
    // The producer includes source/.fmeta moves, project/filter/CMake edits and captured admission;
    // there is no post-move membership write and no separate filesystem move here.
    auto result = applyRefactoring(*plan);
    if (!result)
        QMessageBox::warning(m_window, QStringLiteral("Move source refused"),
                             QString::fromStdString(result.error().message()));
}
} // namespace frost::app
