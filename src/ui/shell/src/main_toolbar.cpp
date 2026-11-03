// SPDX-License-Identifier: Apache-2.0
//
// MainToolbar (frost/ui/shell/main_toolbar.hpp).

#include "frost/ui/shell/main_toolbar.hpp"

#include "frost/input/keymap.hpp"
#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QAbstractItemModel>
#include <QActionEvent>
#include <QComboBox>
#include <QMenu>
#include <QSizePolicy>
#include <QTimer>

#include <utility>

namespace frost::ui {

using shell_detail::text;

MainToolbar::MainToolbar(Invoker invoke, QWidget* parent)
    : QToolBar(parent), m_invoke(std::move(invoke)), m_project(new QToolButton(this)),
      m_projectMenu(new QMenu(m_project)), m_configurations(new QComboBox(this)) {
    setObjectName(QStringLiteral("mainToolbar"));
    setWindowTitle(text("Main Toolbar"));
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);

    m_project->setObjectName(QStringLiteral("projectWidget"));
    m_project->setAutoRaise(true);
    // Project identity is informational until project switching is implemented.
    m_project->setFocusPolicy(Qt::NoFocus);
    m_project->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_project->setIcon(shell_detail::themedIcon(icons::kNodesFolder));
    m_project->setToolTip(text("Current project"));
    m_projectMenu->setObjectName(QStringLiteral("projectSwitcherMenu"));
    connect(m_projectMenu, &QMenu::aboutToShow, this, [this] {
        m_projectMenu->clear();
        if (m_projectMenuBuilder) {
            m_projectMenuBuilder(*m_projectMenu);
        }
    });
    QFont projectFont = m_project->font();
    projectFont.setBold(true);
    m_project->setFont(projectFont);
    setProjectName({});
    addWidget(m_project);

    auto* spacer = new QWidget(this);
    spacer->setObjectName(QStringLiteral("toolbarSpacer"));
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(spacer);

    m_configurations->setObjectName(QStringLiteral("runConfigurations"));
    m_configurations->setMinimumContentsLength(18);
    m_configurations->setPlaceholderText(text("No configurations"));
    m_configurations->setToolTip(text("Run/Build Configuration"));
    m_configurations->setEnabled(false);
    addWidget(m_configurations);
    // The slot is usable as soon as a later slice adds configurations.
    const auto updateCombo = [this] { m_configurations->setEnabled(m_configurations->count() > 0); };
    connect(m_configurations->model(), &QAbstractItemModel::rowsInserted, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::rowsRemoved, this, updateCombo);
    connect(m_configurations->model(), &QAbstractItemModel::modelReset, this, updateCombo);

    addSeparator();
    addSlot(kBuild, icons::kBuildBuild, "Build");
    addSeparator();
    addSlot(kRun, icons::kRunRun, "Run");
    addSlot(kDebug, icons::kRunDebug, "Debug");
    addSlot(kStop, icons::kRunStop, "Stop");
    auto* utilitySpacer = new QWidget(this);
    utilitySpacer->setObjectName(QStringLiteral("toolbarUtilitySpacer"));
    utilitySpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    addWidget(utilitySpacer);
    addSeparator();
    addSlot(kSearch, icons::kActionsSearch, "Search");
    addSlot(kSettings, icons::kActionsSettings, "Settings");
}

void MainToolbar::setProjectName(const QString& name) {
    m_project->setText(name.isEmpty() ? text("No project") : name);
    m_project->setAccessibleName(text("Project: %1").arg(m_project->text()));
}

void MainToolbar::setProjectMenuBuilder(std::function<void(QMenu&)> builder) {
    m_projectMenu->close();
    m_projectMenuBuilder = std::move(builder);
    const bool available = static_cast<bool>(m_projectMenuBuilder);
    m_project->setMenu(available ? m_projectMenu : nullptr);
    m_project->setPopupMode(available ? QToolButton::InstantPopup : QToolButton::DelayedPopup);
    m_project->setFocusPolicy(available ? Qt::TabFocus : Qt::NoFocus);
    m_project->setToolTip(available ? text("Open or switch project") : text("Current project"));
    updateTabOrder();
}

QToolButton* MainToolbar::button(std::string_view actionId) const {
    for (const Slot& slot : m_slots) {
        if (slot.slotId == actionId || slot.actionId == actionId) {
            return slot.button;
        }
    }
    return nullptr;
}

void MainToolbar::setSlotAction(std::string_view slotId, std::string_view actionId) {
    for (Slot& slot : m_slots) {
        if (slot.slotId == slotId) {
            slot.actionId = std::string(actionId);
        }
    }
}

std::vector<QWidget*> MainToolbar::focusOrder() const {
    std::vector<QWidget*> order;
    // Workbench integrations insert their profile and target combos dynamically. Walk the
    // toolbar's actual action order so keyboard focus follows the controls currently displayed.
    for (QAction* action : actions()) {
        QWidget* candidate = widgetForAction(action);
        if (candidate != nullptr && candidate->isVisible() && candidate->isEnabled() &&
            (candidate->focusPolicy() & Qt::TabFocus) != 0) {
            order.push_back(candidate);
        }
    }
    return order;
}

void MainToolbar::actionEvent(QActionEvent* event) {
    QToolBar::actionEvent(event);
    if (event->type() != QEvent::ActionAdded && event->type() != QEvent::ActionRemoved) {
        return;
    }
    // insertWidget() can add controls after construction. Defer until Qt has associated the
    // widget with its action; QObject ownership cancels this callback on destruction.
    QTimer::singleShot(0, this, [this] { updateTabOrder(); });
}

void MainToolbar::updateTabOrder() {
    QWidget* previous = nullptr;
    for (QAction* action : actions()) {
        QWidget* candidate = widgetForAction(action);
        if (candidate == nullptr || (candidate->focusPolicy() & Qt::TabFocus) == 0) {
            continue;
        }
        if (previous != nullptr) {
            QWidget::setTabOrder(previous, candidate);
        }
        previous = candidate;
    }
}

void MainToolbar::addSlot(std::string_view actionId, std::string_view iconId, std::string_view fallbackText) {
    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("toolbar.") + shell_detail::fromUtf8(actionId));
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    const QIcon icon = shell_detail::themedIcon(iconId);
    if (icon.isNull()) {
        button->setText(text(fallbackText));
    } else {
        button->setIcon(icon);
    }
    button->setToolTip(text(fallbackText));
    button->setEnabled(false);
    const std::string id(actionId);
    // The slot's current action is looked up at click time (setSlotAction may rebind it).
    connect(button, &QToolButton::clicked, this, [this, id] {
        for (const Slot& slot : m_slots) {
            if (slot.slotId == id && m_invoke) {
                m_invoke(slot.actionId);
            }
        }
    });
    addWidget(button);
    m_slots.push_back(Slot{id, id, button});
}

void MainToolbar::refresh(input::ActionRegistry& registry, input::KeymapService& keymap,
                          const input::ActionContext& context) {
    for (Slot& slot : m_slots) {
        const std::optional<input::ActionId> id = registry.find(slot.actionId);
        if (!id) {
            slot.button->setEnabled(false);
            continue;
        }
        QString tooltip = qtr(shell_detail::kActionsContext, registry.info(*id).name);
        if (const std::optional<input::Sequence> shortcut = keymap.primaryShortcut(*id); shortcut) {
            tooltip += QStringLiteral(" (") +
                       shell_detail::fromUtf8(input::toDisplayText(*shortcut, keymap.layout(), keymap.platform())) +
                       QStringLiteral(")");
        }
        slot.button->setToolTip(tooltip);
        slot.button->setEnabled(registry.isEnabled(*id, context));
    }
}

} // namespace frost::ui
