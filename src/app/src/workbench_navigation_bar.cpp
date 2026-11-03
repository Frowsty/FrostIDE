// SPDX-License-Identifier: Apache-2.0
// Workbench adapters for the navigation bar's live project models.
#include "frost/app/workbench.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include <QTreeView>

namespace frost::app {
void Workbench::attachNavigationBar() {
    const auto update = [this] {
        if (!m_window || !m_nav)
            return;
        auto& bar = m_window->navigationBar();
        if (!hasProject()) {
            bar.clearModel();
            return;
        }
        const auto open = [this](const std::string& path) {
            if (m_nav)
                (void)m_nav->openFile(path);
        };
        if (m_solution && m_explorer) {
            QPointer<ui::SolutionTreeModel> model = m_explorer->model();
            bar.setModel(
                model, ui::SolutionTreeModel::PathRole,
                [model](const std::string& path) { return model ? model->indexOfPath(path) : QModelIndex(); },
                [](const QModelIndex& index) {
                    return index.data(ui::SolutionTreeModel::KindRole).toInt() !=
                           static_cast<int>(project::NodeKind::File);
                },
                open);
        } else {
            QPointer<ui::FileSystemView> view = m_nav->fileSystemView();
            if (!view) {
                bar.clearModel();
                return;
            }
            bar.setModel(
                view->tree()->model(), ui::FileSystemView::PathRole,
                [view](const std::string& path) { return view ? view->indexOfPath(path) : QModelIndex(); },
                [](const QModelIndex& index) { return index.data(ui::FileSystemView::DirectoryRole).toBool(); }, open);
        }
    };
    connect(this, &Workbench::projectChanged, &m_window->navigationBar(), [update](const QString&) { update(); });
    update();
}
} // namespace frost::app
