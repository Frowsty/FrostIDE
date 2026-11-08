// SPDX-License-Identifier: Apache-2.0
//
// CPP-3: the resolve-context selector. A file that several CMake targets list (a header shared by
// two libraries) is parsed by clangd with one target's flags; this status-bar button names that
// target ("Context: alpha") and its menu lets the user choose another, or "Automatic", where clangd
// infers the command from a nearby source (no target is named then, because the IDE cannot know
// which one clangd picked: the truth rule, ADR 0078).
/*
 * The button is hidden until showContexts(); the workbench calls it for the current editor's file
 * when two or more targets list it, and hideContexts() otherwise. choose() is what a menu action
 * does and emits contextChosen; showContexts() emits nothing. GUI thread only.
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QToolButton>

class QMenu;

namespace frost::ui::lang {

class ResolveContextButton final : public QToolButton {
    Q_OBJECT

public:
    explicit ResolveContextButton(QWidget* parent = nullptr);

    // Shows the selector for `file` with `targets` (in model order); `current` is the chosen index,
    // or -1 for automatic.
    void showContexts(const QString& file, const QStringList& targets, int current);
    void hideContexts();

    [[nodiscard]] QString file() const { return m_file; }
    [[nodiscard]] QStringList targets() const { return m_targets; }
    // The chosen index, -1 for automatic.
    [[nodiscard]] int current() const noexcept { return m_current; }
    // Chooses `index` (-1: automatic) as the menu does: updates the text and emits contextChosen
    // when the choice changed. Out-of-range indices are ignored.
    void choose(int index);

signals:
    void contextChosen(const QString& file, int index);

private:
    void rebuild();

    QMenu* m_menu = nullptr;
    QString m_file;
    QStringList m_targets;
    int m_current = -1;
};

} // namespace frost::ui::lang
