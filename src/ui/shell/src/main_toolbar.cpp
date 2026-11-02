// SPDX-License-Identifier: Apache-2.0
//
// MainToolbar (frost/ui/shell/main_toolbar.hpp).

#include "frost/ui/shell/main_toolbar.hpp"

#include "frost/input/keymap.hpp"
#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QAbstractItemModel>
#include <QComboBox>
#include <QSizePolicy>

#include <utility>

namespace frost::ui {

using shell_detail::text;

MainToolbar::MainToolbar(Invoker invoke, QWidget* parent)
    : QToolBar(parent), m_invoke(std::move(invoke)), m_project(new QToolButton(this)),
      m_configurations(new QComboBox(this)) {
    setObjectName(QStringLiteral("mainToolbar"));
    setWindowTitle(text("Main Toolbar"));
    setMovable(false);
    setFloatable(false);
    setContextMenuPolicy(Qt::PreventContextMenu);

    m_project->setObjectName(QStringLiteral("projectWidget"));
    m_project->setAutoRaise(true);
    m_project->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_project->setIcon(shell_detail::themedIcon(icons::kNodesFolder));
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

    addSlot(kBuild, icons::kBuildBuild, "Build");
    addSlot(kRun, icons::kRunRun, "Run");
    addSlot(kDebug, icons::kRunDebug, "Debug");
    addSlot(kStop, icons::kRunStop, "Stop");
    addSeparator();
    addSlot(kSearch, icons::kActionsSearch, "Search");
    addSlot(kSettings, icons::kActionsSettings, "Settings");
}

void MainToolbar::setProjectName(const QString& name) {
    m_project->setText(name.isEmpty() ? text("No project") : name);
}

QToolButton* MainToolbar::button(std::string_view actionId) const {
    for (const Slot& slot : m_slots) {
        if (slot.actionId == actionId) {
            return slot.button;
        }
    }
    return nullptr;
}

std::vector<QWidget*> MainToolbar::focusOrder() const {
    std::vector<QWidget*> order;
    for (QWidget* candidate : {static_cast<QWidget*>(m_project), static_cast<QWidget*>(m_configurations)}) {
        if (candidate->isVisible() && candidate->isEnabled()) {
            order.push_back(candidate);
        }
    }
    for (const Slot& slot : m_slots) {
        if (slot.button->isEnabled()) {
            order.push_back(slot.button);
        }
    }
    return order;
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
    connect(button, &QToolButton::clicked, this, [this, id] {
        if (m_invoke) {
            m_invoke(id);
        }
    });
    addWidget(button);
    m_slots.push_back(Slot{id, button});
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
