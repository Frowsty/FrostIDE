// SPDX-License-Identifier: Apache-2.0
//
// CPP-3 (resolve_context.hpp): the status-bar resolve-context selector.

#include "frost/ui/lang/resolve_context.hpp"

#include "frost/ui/tr.hpp"

#include <QAction>
#include <QActionGroup>
#include <QMenu>

namespace frost::ui::lang {

namespace {

QString uiText(const char* source) { return qtr("frost::ui::lang::ResolveContextButton", source); }

} // namespace

ResolveContextButton::ResolveContextButton(QWidget* parent) : QToolButton(parent), m_menu(new QMenu(this)) {
    setObjectName(QStringLiteral("resolveContext"));
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setMenu(m_menu);
    setVisible(false);
}

void ResolveContextButton::showContexts(const QString& file, const QStringList& targets, int current) {
    m_file = file;
    m_targets = targets;
    m_current = current >= 0 && current < targets.size() ? current : -1;
    rebuild();
    setVisible(true);
}

void ResolveContextButton::hideContexts() {
    m_file.clear();
    m_targets.clear();
    m_current = -1;
    qDeleteAll(m_menu->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly));
    m_menu->clear();
    setVisible(false);
}

void ResolveContextButton::choose(int index) {
    if (index < -1 || index >= m_targets.size() || m_file.isEmpty())
        return;
    if (index == m_current)
        return;
    m_current = index;
    rebuild();
    emit contextChosen(m_file, index);
}

void ResolveContextButton::rebuild() {
    setText(m_current < 0 ? uiText("Context: automatic") : uiText("Context: %1").arg(m_targets[m_current]));
    setToolTip(uiText("%1 is part of %2 CMake targets. clangd parses it with the chosen target's compile "
                      "flags; Automatic lets clangd infer them from a nearby source.")
                   .arg(m_file)
                   .arg(m_targets.size()));
    qDeleteAll(m_menu->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly));
    m_menu->clear();
    auto* group = new QActionGroup(m_menu);
    const auto add = [&](const QString& label, int index) {
        QAction* action = m_menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(index == m_current);
        action->setData(index);
        group->addAction(action);
        // Queued: choose() rebuilds the menu, which deletes the action still emitting.
        connect(action, &QAction::triggered, this, [this, index] { choose(index); }, Qt::QueuedConnection);
    };
    add(uiText("Automatic"), -1);
    m_menu->addSeparator();
    for (int i = 0; i < m_targets.size(); ++i)
        add(m_targets[i], i);
}

} // namespace frost::ui::lang
