// SPDX-License-Identifier: Apache-2.0
//
// The build pieces of the shell's main toolbar and menus (W1, ADR 0039): the Build, Rebuild, Clean
// and Cancel Build actions in frost::input's registry and as QActions, the table of their default
// shortcuts, and the solution configuration combo.
//
/*
 * Default shortcuts. kBuildShortcutDefaults decides them, one entry per action, in frost::input's
 * Strict sequence syntax; changing a default is a one-line edit there. The table reaches the
 * shortcut matcher through KeymapService::setModuleDefaults(buildShortcutBindings()): a keymap line
 * for the same action wins over it, and the user's overrides win over both. Build Solution is
 * Ctrl+Shift+B (Visual Studio's) rather than IntelliJ's Ctrl+F9 until the user decides (one entry).
 * IntelliJ binds Ctrl+Shift+B to Go to Type Declaration, so the build actions register with
 * priority 1 and win that shared shortcut wherever both apply. Cancel Build is Ctrl+Break, which
 * frost::input names Ctrl+Pause.
 *
 * Enablement. Build, Rebuild and Clean need an open solution and no build in progress; Cancel
 * needs a build in progress (a running build may be cancelled from any of its states). The
 * actions are not marked executesProjectCode: the build service's own trust prompt (NeedsTrust)
 * asks before a build runs project code, instead of the commands being greyed out in an untrusted
 * folder. The registry's enabled callbacks and the QActions read the same BuildController state.
 *
 * GUI thread only. The controller is held through a QPointer: once it is gone the actions are
 * disabled and their handlers do nothing.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"

#include <QComboBox>
#include <QObject>
#include <QPointer>

#include <array>
#include <string_view>
#include <vector>

class QAction;

namespace frost::ui {

class BuildController;

namespace build_actions {
inline constexpr std::string_view kBuildSolution = "build.buildSolution";
inline constexpr std::string_view kRebuildSolution = "build.rebuildSolution";
inline constexpr std::string_view kCleanSolution = "build.cleanSolution";
inline constexpr std::string_view kCancelBuild = "build.cancelBuild";
// The Find Action category of all four.
inline constexpr std::string_view kGroup = "Build";
} // namespace build_actions

// One default shortcut (header comment).
struct BuildShortcutDefault {
    std::string_view actionId;
    std::string_view shortcut; // Strict sequence syntax; "" leaves the action unbound by default.
};

// The defaults table (header comment). Edit here to change a default.
inline constexpr std::array<BuildShortcutDefault, 4> kBuildShortcutDefaults{{
    {build_actions::kBuildSolution, "Ctrl+Shift+B"}, // Visual Studio; IntelliJ's would be Ctrl+F9.
    {build_actions::kRebuildSolution, ""},
    {build_actions::kCleanSolution, ""},
    {build_actions::kCancelBuild, "Ctrl+Pause"}, // Ctrl+Break.
}};

// kBuildShortcutDefaults as keymap bindings, unbound entries left out, for
// KeymapService::setModuleDefaults. Errors: InvalidArgument naming an entry that does not parse.
[[nodiscard]] Result<std::vector<input::Binding>> buildShortcutBindings();

class BuildActions final : public QObject {
    Q_OBJECT

public:
    // The QActions exist from construction; registerActions adds the registry entries.
    explicit BuildActions(BuildController& controller, QObject* parent = nullptr);
    ~BuildActions() override;

    /*
     * Registers the four actions (group "Build", priority 1, global context) with handlers and
     * enabled callbacks over the controller. The callbacks hold the controller through a QPointer,
     * not this object, so they stay safe (and inert) once either is gone. Errors: those of
     * ActionRegistry::add (AlreadyExists when called twice for one registry).
     */
    [[nodiscard]] Result<void> registerActions(input::ActionRegistry& registry);

    // The toolbar and menu actions, owned by this object.
    [[nodiscard]] QAction* buildSolution() const noexcept { return m_build; }
    [[nodiscard]] QAction* rebuildSolution() const noexcept { return m_rebuild; }
    [[nodiscard]] QAction* cleanSolution() const noexcept { return m_clean; }
    [[nodiscard]] QAction* cancelBuild() const noexcept { return m_cancel; }

    // Whether the command with this action id may run now (header comment, "Enablement").
    [[nodiscard]] bool isEnabled(std::string_view actionId) const;
    // Runs the command with this action id if it is enabled; returns whether it ran.
    bool run(std::string_view actionId);

private:
    void updateEnabled();

    QPointer<BuildController> m_controller;
    QAction* m_build = nullptr;
    QAction* m_rebuild = nullptr;
    QAction* m_clean = nullptr;
    QAction* m_cancel = nullptr;
};

/*
 * The solution configuration combo ("Debug | x64"): lists BuildController::configurations(), shows
 * the active one, and switches (and so persists) the active configuration when the user picks
 * another. Disabled without a solution and during a build; a refused switch snaps back. GUI thread
 * only; the controller must outlive the combo.
 */
class ConfigurationCombo final : public QComboBox {
    Q_OBJECT

public:
    explicit ConfigurationCombo(BuildController& controller, QWidget* parent = nullptr);
    ~ConfigurationCombo() override;

private:
    void reload();
    void showActive();
    void updateEnabled();

    BuildController& m_controller;
    bool m_updating = false;
};

} // namespace frost::ui
