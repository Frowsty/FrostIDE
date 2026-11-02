// SPDX-License-Identifier: Apache-2.0
//
// The VCS actions (frost/ui/vcs/git_actions.hpp): the defaults table and the registrations.

#include "frost/ui/vcs/git_actions.hpp"

#include "frost/input/key.hpp"
#include "frost/ui/vcs/diff_viewer.hpp"
#include "frost/ui/vcs/git_controller.hpp"

#include <QCoreApplication>

#include <format>
#include <memory>

namespace frost::ui {
namespace {

QString trText(const char* text) { return QCoreApplication::translate("GitActions", text); }

// One registration: its texts, context, trust need, and what it does.
struct GitActionSpec {
    std::string_view id;
    std::string_view name;
    std::string_view description;
    std::vector<std::string> synonyms;
    input::ContextRule context;
    bool needsTrust = true;
    std::function<bool()> enabled; // Beyond "the controller exists".
    std::function<void()> run;
};

constexpr input::ContextRule kAnywhere{{}, {input::ContextKey::DialogFocus}};
constexpr input::ContextRule kCommitView{{input::ContextKey::CommitViewFocus}, {input::ContextKey::DialogFocus}};
constexpr input::ContextRule kDiffView{{input::ContextKey::DiffViewFocus}, {input::ContextKey::DialogFocus}};

} // namespace

Result<std::vector<input::Binding>> gitShortcutBindings() {
    std::vector<input::Binding> bindings;
    for (const GitShortcutDefault& entry : kGitShortcutDefaults) {
        if (entry.shortcut.empty()) {
            continue;
        }
        auto sequence = input::parseSequence(entry.shortcut, input::SyntaxMode::Strict);
        if (!sequence) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the default shortcut '{}' of {} does not parse: {}", entry.shortcut,
                                    entry.actionId, sequence.error().message()));
        }
        bindings.push_back(input::Binding{std::string(entry.actionId), {*sequence}});
    }
    return bindings;
}

Result<void> registerGitActions(input::ActionRegistry& registry, GitController& controller, GitActionTargets targets) {
    const QPointer<GitController> git(&controller);
    const auto shared = std::make_shared<const GitActionTargets>(std::move(targets));
    const auto confirm = [shared](const QString& title, const QString& question) {
        return shared->confirm && shared->confirm(title, question);
    };
    const auto file = [shared]() { return shared->currentFile ? shared->currentFile() : std::string(); };
    const auto viewer = [shared]() -> DiffViewer* {
        return shared->activeDiffViewer ? shared->activeDiffViewer() : nullptr;
    };

    std::vector<GitActionSpec> specs;
    specs.push_back({git_actions::kCommit,
                     "Commit...",
                     "Open the Commit window to commit the changes",
                     {"checkin", "commit changes"},
                     kAnywhere,
                     true,
                     [shared] { return static_cast<bool>(shared->openCommitWindow); },
                     [shared] { shared->openCommitWindow(); }});
    specs.push_back({git_actions::kPush,
                     "Push",
                     "Push the current branch to its upstream",
                     {"git push"},
                     kAnywhere,
                     true,
                     {},
                     [git] { git->push(); }});
    specs.push_back({git_actions::kForcePush,
                     "Force Push",
                     "Force-push the current branch with lease, after asking",
                     {"push force", "force with lease"},
                     kAnywhere,
                     true,
                     {},
                     [git, confirm] {
                         if (confirm(trText("Force Push"),
                                     trText("Force-push the current branch? Commits on the remote that are not here "
                                            "will be lost (the push is refused if the remote moved since the last "
                                            "fetch)."))) {
                             git->push(true, true);
                         }
                     }});
    specs.push_back(
        {git_actions::kUpdateProject,
         "Update Project",
         "Pull the current branch's upstream",
         {"update", "git pull"},
         kAnywhere,
         true,
         {},
         [git, shared] { git->pull(shared->pullStrategy ? shared->pullStrategy() : vcs::PullStrategy::Merge); }});
    specs.push_back({git_actions::kPull,
                     "Pull",
                     "Fetch and merge the upstream into the current branch",
                     {"merge"},
                     kAnywhere,
                     true,
                     {},
                     [git] { git->pull(vcs::PullStrategy::Merge); }});
    specs.push_back({git_actions::kPullRebase,
                     "Pull with Rebase",
                     "Fetch and rebase the current branch onto its upstream",
                     {"rebase"},
                     kAnywhere,
                     true,
                     {},
                     [git] { git->pull(vcs::PullStrategy::Rebase); }});
    specs.push_back({git_actions::kFetch, "Fetch", "Fetch every remote", {"git fetch"}, kAnywhere, true, {}, [git] {
                         git->fetch();
                     }});
    specs.push_back(
        {git_actions::kRollback,
         "Rollback...",
         "Roll back the changes to the current file",
         {"revert", "discard changes"},
         kAnywhere,
         true,
         [file] { return !file().empty(); },
         [git, file, confirm] {
             const std::string path = file();
             if (!path.empty() &&
                 confirm(trText("Rollback Changes"),
                         trText("Roll back the changes to %1? They will be lost.").arg(QString::fromStdString(path)))) {
                 git->rollback({path});
             }
         }});
    specs.push_back({git_actions::kShowDiff,
                     "Show Diff",
                     "Show the diff of the selected change",
                     {"diff"},
                     kCommitView,
                     false,
                     [shared] { return static_cast<bool>(shared->showDiffInCommitWindow); },
                     [shared] { shared->showDiffInCommitWindow(); }});
    specs.push_back({git_actions::kCompareWithHead,
                     "Compare with HEAD",
                     "Show the differences between the current file and its HEAD version",
                     {"compare with the same repository version", "diff with head"},
                     kAnywhere,
                     false,
                     [shared, file] { return static_cast<bool>(shared->compareWithHead) && !file().empty(); },
                     [shared, file] { shared->compareWithHead(file()); }});
    specs.push_back({git_actions::kBranches,
                     "Branches...",
                     "Show the local and remote branches",
                     {"checkout", "git branches", "new branch"},
                     kAnywhere,
                     true,
                     [shared] { return static_cast<bool>(shared->showBranches); },
                     [shared] { shared->showBranches(); }});
    specs.push_back({git_actions::kNextChange,
                     "Next Change",
                     "Go to the next change in the diff",
                     {"next difference"},
                     kDiffView,
                     false,
                     [viewer] { return viewer() != nullptr; },
                     [viewer] { viewer()->nextChange(); }});
    specs.push_back({git_actions::kPreviousChange,
                     "Previous Change",
                     "Go to the previous change in the diff",
                     {"previous difference"},
                     kDiffView,
                     false,
                     [viewer] { return viewer() != nullptr; },
                     [viewer] { viewer()->previousChange(); }});

    for (GitActionSpec& entry : specs) {
        input::ActionSpec spec;
        spec.id = std::string(entry.id);
        spec.name = std::string(entry.name);
        spec.description = std::string(entry.description);
        spec.group = std::string(git_actions::kGroup);
        spec.synonyms = std::move(entry.synonyms);
        spec.context = entry.context;
        // Show Diff and Next / Previous Change share keys with the editor and the debugger (header comment).
        spec.priority = entry.context == kAnywhere ? 0 : 1;
        // Hooks, merges and credential helpers run project code: the registry's trust gate (Untrusted).
        spec.executesProjectCode = entry.needsTrust;
        spec.enabled = [git, enabled = entry.enabled](const input::ActionContext&) {
            return git && (!enabled || enabled());
        };
        spec.handler = [git, run = entry.run](const input::ActionContext&, const input::InvokeInfo&) {
            if (git) {
                run();
            }
        };
        if (auto added = registry.add(std::move(spec)); !added) {
            return std::unexpected(std::move(added).error());
        }
    }
    return {};
}

} // namespace frost::ui
