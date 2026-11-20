// SPDX-License-Identifier: Apache-2.0
// Services projects actual build/run/debug owners into one view; it never creates a process or invented row.
#include "frost/app/workbench.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "cmake_integration.hpp"
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace frost::app {
void Workbench::attachServices() {
    if (!m_window) return;
    const QPointer<Workbench> owner(this);
    ui::ToolWindowSpec spec;
    spec.id = "services";
    spec.title = QStringLiteral("Services");
    spec.side = ui::ToolWindowSide::Right;
    spec.createContent = [owner]() -> QWidget* {
        auto* pane = new QWidget;
        pane->setObjectName(QStringLiteral("servicesToolWindow"));
        auto* layout = new QVBoxLayout(pane);
        auto* tree = new QTreeWidget(pane);
        tree->setObjectName(QStringLiteral("servicesTree"));
        tree->setHeaderLabels({QStringLiteral("Service"), QStringLiteral("State")});
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        auto* empty = new QLabel(QStringLiteral("No active build, run or debug services."), pane);
        auto* stop = new QPushButton(QStringLiteral("Stop Selected Run or Debug Session"), pane);
        stop->setEnabled(false);
        layout->addWidget(tree); layout->addWidget(empty); layout->addWidget(stop);
        const auto refresh = [owner, pane, tree, empty, stop] {
            if (!owner || !pane->isVisible()) return;
            QString selected;
            if (auto* current = tree->currentItem()) selected = current->data(0, Qt::UserRole).toString();
            struct Row { QString key, title, state; std::uint64_t id = 0, generation = 0; };
            std::vector<Row> rows;
            if (owner->m_buildService && owner->m_buildService->state() != build::BuildState::Idle)
                rows.push_back({QStringLiteral("build"), QStringLiteral("MSBuild pipeline"), QString::fromUtf8(build::toString(owner->m_buildService->state()).data())});
            if (owner->m_cmake && owner->m_cmake->service().state() != build::BuildState::Idle)
                rows.push_back({QStringLiteral("cmake"), QStringLiteral("CMake pipeline"), QString::fromUtf8(build::toString(owner->m_cmake->service().state()).data())});
            if (owner->m_runManager) for (const auto& run : owner->m_runManager->activeRuns())
                rows.push_back({QStringLiteral("run:") + QString::number(run.id), QString::fromStdString(run.configuration.name),
                    QString::fromUtf8(run::toString(run.phase).data()), run.id});
            if (owner->m_debugSessions) for (const auto& session : owner->m_debugSessions->sessions()) {
                if (!session.controller || !session.controller->debugging()) continue;
                rows.push_back({QStringLiteral("debug:") + QString::number(session.id), session.name,
                    QString::fromUtf8(debug::toString(session.controller->state()).data()), session.id, session.controller->sessionGeneration()});
            }
            QString signature;
            for (const auto& row : rows) signature += row.key + QLatin1Char('\t') + row.title + QLatin1Char('\t') + row.state + QLatin1Char('\t') + QString::number(row.generation) + QLatin1Char('\n');
            if (tree->property("servicesSnapshot").toString() == signature) return;
            tree->setProperty("servicesSnapshot", signature);
            tree->clear();
            for (const auto& row : rows) {
                auto* item = new QTreeWidgetItem(tree, {row.title, row.state});
                item->setData(0, Qt::UserRole, row.key);
                item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(row.id));
                item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qulonglong>(row.generation));
                if (row.key == selected) tree->setCurrentItem(item);
            }
            empty->setVisible(rows.empty());
            stop->setEnabled(tree->currentItem() && (selected.startsWith(QStringLiteral("run:")) || selected.startsWith(QStringLiteral("debug:"))));
        };
        QObject::connect(tree, &QTreeWidget::currentItemChanged, pane, [tree, stop](QTreeWidgetItem* item, QTreeWidgetItem*) {
            const auto key = item ? item->data(0, Qt::UserRole).toString() : QString();
            stop->setEnabled(key.startsWith(QStringLiteral("run:")) || key.startsWith(QStringLiteral("debug:")));
        });
        QObject::connect(stop, &QPushButton::clicked, pane, [owner, tree] {
            if (!owner || !tree->currentItem()) return;
            auto* row = tree->currentItem();
            const auto key = row->data(0, Qt::UserRole).toString();
            const auto id = row->data(0, Qt::UserRole + 1).toULongLong();
            if (key.startsWith(QStringLiteral("run:")) && owner->m_runManager) owner->m_runManager->stop(id);
            if (key.startsWith(QStringLiteral("debug:")) && owner->m_debugSessions) {
                auto* controller = owner->m_debugSessions->controller(id);
                if (controller && controller->sessionGeneration() == row->data(0, Qt::UserRole + 2).toULongLong()) controller->stop();
            }
        });
        QObject::connect(tree, &QTreeWidget::itemActivated, pane, [owner](QTreeWidgetItem* row, int) {
            if (!owner || !owner->m_window) return;
            const auto key = row->data(0, Qt::UserRole).toString();
            std::string window;
            if (key.startsWith(QStringLiteral("debug:")) && owner->m_debugSessions) {
                const auto id = row->data(0, Qt::UserRole + 1).toULongLong();
                if (auto* controller = owner->m_debugSessions->controller(id);
                    !controller || controller->sessionGeneration() != row->data(0, Qt::UserRole + 2).toULongLong()) return;
                (void)owner->m_debugSessions->select(id);
                window = "debug";
            } else if (key.startsWith(QStringLiteral("run:"))) window = "run";
            else window = key.toStdString();
            owner->m_window->toolWindows().show(window, true);
        });
        auto* timer = new QTimer(pane);
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, pane, refresh);
        timer->start();
        return pane;
    };
    (void)m_window->toolWindows().add(std::move(spec));
    if (!m_registry.find("window.services")) {
        input::ActionSpec action;
        action.id = "window.services"; action.name = "Services"; action.group = "View";
        action.handler = [owner](const input::ActionContext&, const input::InvokeInfo&) {
            if (owner && owner->m_window) owner->m_window->toolWindows().activate("services");
        };
        (void)m_registry.add(std::move(action));
    }
}
} // namespace frost::app
