// SPDX-License-Identifier: Apache-2.0
//
// registerRunActions (see frost/ui/run_actions.hpp).

#include "frost/ui/run_actions.hpp"

#include "frost/ui/run_controller.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace frost::ui {

namespace {

// Whether `project` can be the startup project.
bool isCandidate(const build::RunService& service, project::ProjectId project) {
    const auto candidates = service.startupCandidates();
    return std::ranges::find(candidates, project) != candidates.end();
}

} // namespace

Result<void> registerRunActions(input::ActionRegistry& registry, RunController& controller, RunActionHooks hooks) {
    auto shared = std::make_shared<RunActionHooks>(std::move(hooks));
    const auto report = [shared](const Error& error) {
        if (shared->reportError) {
            shared->reportError(error);
        }
    };
    RunController* c = &controller;
    const auto hasSolution = [c](const input::ActionContext&) {
        return c->service() != nullptr && c->service()->solution() != nullptr;
    };
    const auto runAction = [c, report](const input::ActionContext&, const input::InvokeInfo&) {
        if (auto started = c->run(); !started && started.error().code() != ErrorCode::Cancelled) {
            report(started.error());
        }
    };

    std::vector<input::ActionSpec> specs;
    specs.push_back({.id = std::string(kRunActionId),
                     .name = "Run",
                     .description = "Build and run the startup project",
                     .group = "Run",
                     .synonyms = {"Start", "Execute"},
                     .context = {},
                     .enabled = hasSolution,
                     .handler = runAction});
    specs.push_back({.id = std::string(kRunWithoutDebuggingActionId),
                     .name = "Start Without Debugging",
                     .description = "Build and run the startup project (Visual Studio's Ctrl+F5)",
                     .group = "Run",
                     .synonyms = {"Run Without Debugging"},
                     .context = {},
                     .enabled = hasSolution,
                     .handler = runAction});
    specs.push_back({.id = std::string(kStopActionId),
                     .name = "Stop",
                     .description = "Stop the running program and everything it started",
                     .group = "Run",
                     .synonyms = {"Kill", "Terminate"},
                     .context = {},
                     .enabled = [c](const input::ActionContext&) { return c->running(); },
                     .handler = [c](const input::ActionContext&, const input::InvokeInfo&) { c->stop(); }});
    specs.push_back({.id = std::string(kRerunActionId),
                     .name = "Rerun",
                     .description = "Stop the program if it runs, then build and run it again",
                     .group = "Run",
                     .synonyms = {"Restart"},
                     .context = {},
                     .enabled = hasSolution,
                     .handler = [c, report](const input::ActionContext&, const input::InvokeInfo&) {
                         if (auto again = c->rerun(); !again) {
                             report(again.error());
                         }
                     }});
    specs.push_back({.id = std::string(kSetStartupProjectActionId),
                     .name = "Set as Startup Project",
                     .description = "Make the selected project the one Run starts",
                     .group = "Run",
                     .synonyms = {"Startup Project"},
                     .context = {},
                     .enabled =
                         [c, shared](const input::ActionContext&) {
                             if (c->service() == nullptr || !shared->selectedProject) {
                                 return false;
                             }
                             const auto selected = shared->selectedProject();
                             return selected && isCandidate(*c->service(), *selected) &&
                                    c->service()->startupProject() != selected;
                         },
                     .handler =
                         [c, shared, report](const input::ActionContext&, const input::InvokeInfo&) {
                             const auto selected = shared->selectedProject ? shared->selectedProject() : std::nullopt;
                             if (!selected) {
                                 return;
                             }
                             if (auto set = c->setStartupProject(*selected); !set) {
                                 report(set.error());
                             }
                         }});
    specs.push_back({.id = std::string(kToggleExternalConsoleActionId),
                     .name = "Run in External Console",
                     .description = "Switch console programs between an external console window and the Run console",
                     .group = "Run",
                     .synonyms = {"Use External Console", "Run Console"},
                     .context = {},
                     .enabled = [c](const input::ActionContext&) { return c->service() != nullptr; },
                     .handler =
                         [c, report](const input::ActionContext&, const input::InvokeInfo&) {
                             if (c->service() == nullptr) {
                                 return;
                             }
                             const bool external = c->service()->useExternalConsole();
                             if (auto set = c->service()->setUseExternalConsole(!external); !set) {
                                 report(set.error());
                             }
                         }});
    for (input::ActionSpec& spec : specs) {
        if (auto added = registry.add(std::move(spec)); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

} // namespace frost::ui
