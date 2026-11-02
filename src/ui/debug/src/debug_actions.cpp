// SPDX-License-Identifier: Apache-2.0
//
// registerDebugActions and registerDebugToolWindow (see frost/ui/debug_actions.hpp).

#include "frost/ui/debug_actions.hpp"

#include "frost/core/log.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include <QPointer>

#include <functional>
#include <string>
#include <utility>

namespace frost::ui {

Result<void> registerDebugActions(input::ActionRegistry& registry, DebugController& controller) {
    const QPointer<DebugController> c(&controller);
    using Enabled = std::function<bool(const DebugController&)>;
    using Run = std::function<void(DebugController&)>;
    const auto behaviour = [](std::string_view id) -> std::pair<Enabled, Run> {
        const auto paused = [](const DebugController& d) { return d.paused(); };
        if (id == "run.debug") {
            return {[](const DebugController& d) { return !d.debugging() && d.canDebug(); },
                    [](DebugController& d) {
                        if (auto started = d.debug(); !started) {
                            log::warn("debug", "Debug: {}", started.error().message());
                        }
                    }};
        }
        if (id == "debug.toggleBreakpoint") {
            return {{}, [](DebugController& d) { d.toggleBreakpointAtCaret(); }};
        }
        if (id == "debug.resume") {
            return {paused, [](DebugController& d) { d.resume(); }};
        }
        if (id == "debug.pause") {
            return {[](const DebugController& d) { return d.debugging() && d.state() == debug::DebugState::Running; },
                    [](DebugController& d) { d.pause(); }};
        }
        if (id == "debug.stepOver") {
            return {paused, [](DebugController& d) { d.stepOver(); }};
        }
        if (id == "debug.stepInto") {
            return {paused, [](DebugController& d) { d.stepInto(); }};
        }
        if (id == "debug.stepOut") {
            return {paused, [](DebugController& d) { d.stepOut(); }};
        }
        if (id == "debug.runToCursor") {
            return {paused, [](DebugController& d) { d.runToCursor(); }};
        }
        return {paused, [](DebugController& d) { d.requestEvaluate(); }};
    };
    for (const DebugActionInfo& info : kDebugActions) {
        auto [enabled, run] = behaviour(info.id);
        input::ActionSpec spec;
        spec.id = std::string(info.id);
        spec.name = std::string(info.name);
        spec.description = std::string(info.description);
        spec.group = "Debug";
        if (enabled) {
            spec.enabled = [c, enabled = std::move(enabled)](const input::ActionContext&) {
                return c != nullptr && enabled(*c);
            };
        }
        spec.handler = [c, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
            if (c != nullptr) {
                run(*c);
            }
        };
        if (auto added = registry.add(std::move(spec)); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

Result<void> registerDebugToolWindow(ToolWindowManager& windows, input::ActionRegistry& registry,
                                     DebugController& controller) {
    const QPointer<DebugController> c(&controller);
    ToolWindowSpec spec;
    spec.id = std::string(kDebugToolWindowId);
    spec.title = QStringLiteral("Debug");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kRunDebug);
    spec.createContent = [c]() -> QWidget* { return c != nullptr ? new DebugToolWindow(*c) : nullptr; };
    if (auto added = windows.add(std::move(spec)); !added) {
        return added;
    }
    const QPointer<ToolWindowManager> w(&windows);
    input::ActionSpec action;
    action.id = std::string(kDebugWindowActionId);
    action.name = "Debug";
    action.description = "Show the Debug tool window";
    action.group = "Tool Windows";
    action.handler = [w](const input::ActionContext&, const input::InvokeInfo&) {
        if (w != nullptr) {
            w->activate(kDebugToolWindowId);
        }
    };
    if (auto added = registry.add(std::move(action)); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return {};
}

} // namespace frost::ui
