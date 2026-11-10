// SPDX-License-Identifier: Apache-2.0
//
// The VCS actions in frost::input's registry (M5, ADR 0048), with the IntelliJ keymap's ids: Commit
// (vcs.commit, Ctrl+K), Push (vcs.push, Ctrl+Shift+K), Update Project (vcs.update, Ctrl+T) and
// Rollback (vcs.rollback, Ctrl+Alt+Z), whose shortcuts the IntelliJ keymap already names, plus
// Show Diff (vcs.showDiff, Ctrl+D in the Commit window), Compare with HEAD, Fetch, Pull, Branches,
// and Next / Previous Change in a diff viewer (diff.nextChange F7, diff.previousChange Shift+F7).
/*
 * Seams. The actions run through GitActionTargets, a struct of callbacks the workbench fills (open
 * the Commit window, show the Branches popup, present a diff, the active diff viewer, the confirm
 * prompt); an empty callback disables its action. The controller is held through a QPointer: once
 * it is gone every action is disabled. Mutating actions are marked executesProjectCode (hooks,
 * merges and credential helpers run). Opening Commit, Branches and the trust dialog is read-only;
 * their mutating controls use the controller's live operation gate. Compare with HEAD, Show Diff
 * and Next / Previous Change also only read (git runs in restricted mode there).
 *
 * Shortcuts the keymap does not name come from kGitShortcutDefaults through
 * KeymapService::setModuleDefaults(gitShortcutBindings()); a keymap line or a user override wins.
 * Ctrl+D (the editor's Duplicate Line) applies only with CommitViewFocus, and F7 / Shift+F7 (the
 * debugger's Step Into) only with DiffViewFocus, both at priority 1.
 *
 * GUI thread only.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap.hpp"
#include "frost/vcs/git_repository.hpp"

#include <QPointer>
#include <QString>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

class DiffViewer;
class GitController;

namespace git_actions {
inline constexpr std::string_view kCommit = "vcs.commit";
inline constexpr std::string_view kTrustRepository = "vcs.trustRepository";
inline constexpr std::string_view kPush = "vcs.push";
inline constexpr std::string_view kForcePush = "vcs.forcePush";
inline constexpr std::string_view kUpdateProject = "vcs.update";
inline constexpr std::string_view kRollback = "vcs.rollback";
inline constexpr std::string_view kShowDiff = "vcs.showDiff";
inline constexpr std::string_view kCompareWithHead = "vcs.compareWithHead";
inline constexpr std::string_view kCompareWithIndex = "vcs.compareWithIndex";
inline constexpr std::string_view kFetch = "vcs.fetch";
inline constexpr std::string_view kPull = "vcs.pull";
inline constexpr std::string_view kPullRebase = "vcs.pullRebase";
inline constexpr std::string_view kBranches = "vcs.branches";
inline constexpr std::string_view kQuickPopup = "vcs.quickPopup";
inline constexpr std::string_view kLog = "vcs.log";
inline constexpr std::string_view kStash = "vcs.stash";
inline constexpr std::string_view kUnstash = "vcs.unstash";
inline constexpr std::string_view kAnnotate = "vcs.annotate";
inline constexpr std::string_view kResolveConflicts = "vcs.resolveConflicts";
inline constexpr std::string_view kNextChange = "diff.nextChange";
inline constexpr std::string_view kPreviousChange = "diff.previousChange";
// The Find Action category.
inline constexpr std::string_view kGroup = "VCS";
} // namespace git_actions

// One default shortcut the IntelliJ keymap does not name ("" leaves the action unbound).
struct GitShortcutDefault {
    std::string_view actionId;
    std::string_view shortcut; // Strict sequence syntax.
};

inline constexpr std::array<GitShortcutDefault, 14> kGitShortcutDefaults{{
    {git_actions::kShowDiff, "Ctrl+D"},
    {git_actions::kNextChange, "F7"},
    {git_actions::kPreviousChange, "Shift+F7"},
    {git_actions::kCompareWithHead, ""},
    {git_actions::kFetch, ""},
    {git_actions::kPull, ""},
    {git_actions::kPullRebase, ""},
    {git_actions::kForcePush, ""},
    {git_actions::kBranches, ""},
    {git_actions::kLog, ""},
    {git_actions::kStash, ""},
    {git_actions::kUnstash, ""},
    {git_actions::kAnnotate, ""},
    {git_actions::kResolveConflicts, ""},
}};

// kGitShortcutDefaults as bindings for KeymapService::setModuleDefaults (unbound ones left out).
// Errors: InvalidArgument naming an entry that does not parse.
[[nodiscard]] Result<std::vector<input::Binding>> gitShortcutBindings();

// What the actions do beyond the controller (header comment). Every member is optional.
struct GitActionTargets {
    std::function<void()> requestTrust;           // Explicitly request repository trust.
    std::function<void()> openCommitWindow;       // Commit: show and focus the Commit window.
    std::function<void()> showDiffInCommitWindow; // Show Diff: the Commit window's current file.
    std::function<std::string()> currentFile;     // Compare with HEAD and Rollback: the active file ("" none).
    std::function<void(const std::string& path)> compareWithHead;
    std::function<void(const std::string& path)> compareWithIndex; // Opens a diff of the file against HEAD.
    std::function<void()> showBranches;                            // Branches: the popup.
    std::function<DiffViewer*()> activeDiffViewer;                 // Next / Previous Change.
    std::function<vcs::PullStrategy()> pullStrategy;               // Update Project's (unset: merge).
    // Rollback's and the force push's confirmation; empty answers no.
    std::function<bool(const QString& title, const QString& question)> confirm;
    std::function<void()> showLog;                         // Show Git Log tool window.
    std::function<void()> stashChanges;                    // Stash Changes... dialog/action.
    std::function<void()> unstashChanges;                  // Unstash Changes... dialog/action.
    std::function<void(const std::string& path)> annotate; // Annotate with Git Blame.
    std::function<void()> resolveConflicts;                // Resolve Conflicts... dialog.
};

/*
 * Registers the actions (header comment) for `controller` with `targets`. Errors: those of
 * ActionRegistry::add (AlreadyExists when called twice for one registry).
 */
[[nodiscard]] Result<void> registerGitActions(input::ActionRegistry& registry, GitController& controller,
                                              GitActionTargets targets);
// Resolves the current project controller at invocation time; resolver must be safe after teardown.
[[nodiscard]] Result<void> registerGitActions(input::ActionRegistry& registry,
                                              std::function<GitController*()> currentController,
                                              GitActionTargets targets);

} // namespace frost::ui
