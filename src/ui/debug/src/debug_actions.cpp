// SPDX-License-Identifier: Apache-2.0
//
// registerDebugActions and registerDebugToolWindow (see frost/ui/debug_actions.hpp).

#include "frost/ui/debug_actions.hpp"

#include "frost/core/log.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/debug_sessions_window.hpp"
#include "frost/ui/debug_tool_window.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include <QPointer>

#include <functional>
#include <string>
#include <utility>

namespace frost::ui {

namespace {
Result<void> registerActions(input::ActionRegistry& registry, std::function<DebugController*()> current,
                             std::function<Result<void>()> launch = {}, std::function<bool()> mayLaunch = {}) {
    using Enabled = std::function<bool(const DebugController&)>;
    using Run = std::function<void(DebugController&)>;
    const auto behaviour = [](std::string_view id) -> std::pair<Enabled, Run> {
        const auto paused = [](const DebugController& d) { return d.paused() && !d.readOnlySession(); };
        if (id == "run.debug" || id == "run.debugContext") {
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
            return {[](const DebugController& d) {
                        return d.debugging() && !d.readOnlySession() && d.state() == debug::DebugState::Running;
                    },
                    [](DebugController& d) { d.pause(); }};
        }
        if (id == "debug.stepOver") {
            return {paused, [](DebugController& d) { d.stepOver(); }};
        }
        if (id == "debug.stepInto") {
            return {paused, [](DebugController& d) { d.stepInto(); }};
        }
        if (id == "debug.smartStepInto") {
            return {[](const DebugController&) { return false; }, [](DebugController&) {}};
        }
        if (id == "debug.stepOut") {
            return {paused, [](DebugController& d) { d.stepOut(); }};
        }
        if (id == "debug.runToCursor") {
            return {paused, [](DebugController& d) { d.runToCursor(); }};
        }
        if (id == "debug.setNextStatement") {
            return {paused, [](DebugController& d) { d.setNextStatement(); }};
        }
        if (id == "debug.muteBreakpoints") {
            return {{}, [](DebugController& d) { d.toggleMuteBreakpoints(); }};
        }
        if (id == "debug.editBreakpoint") {
            return {{}, [](DebugController& d) { d.requestEditBreakpoint(); }};
        }
        if (id == "debug.viewBreakpoints") {
            return {{}, [](DebugController& d) { d.requestViewBreakpoints(); }};
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
        if (launch && (info.id == "run.debug" || info.id == "run.debugContext")) {
            spec.enabled = [mayLaunch](const input::ActionContext&) { return !mayLaunch || mayLaunch(); };
            spec.handler = [launch](const input::ActionContext&, const input::InvokeInfo&) {
                if (auto result = launch(); !result)
                    log::warn("debug", "Debug: {}", result.error().message());
            };
        } else {
            spec.enabled = [current, enabled = std::move(enabled)](const input::ActionContext&) {
                const auto* controller = current();
                return controller && (!enabled || enabled(*controller));
            };
            spec.handler = [current, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
                if (auto* controller = current())
                    run(*controller);
            };
        }
        if (auto added = registry.add(std::move(spec)); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return {};
}

} // namespace

Result<void> registerDebugActions(input::ActionRegistry& registry, DebugController& controller) {
    const QPointer<DebugController> guarded(&controller);
    return registerActions(registry, [guarded] { return guarded.data(); });
}
Result<void> registerDebugActions(input::ActionRegistry& registry, DebugSessionRegistry& sessions,
                                  std::function<Result<void>()> launch, std::function<bool()> mayLaunch) {
    const QPointer<DebugSessionRegistry> guarded(&sessions);
    return registerActions(
        registry, [guarded] { return guarded ? guarded->selected() : nullptr; }, std::move(launch),
        std::move(mayLaunch));
}

namespace {
Result<void> registerWindow(ToolWindowManager& windows, input::ActionRegistry& registry,
                            std::function<QWidget*()> content) {
    ToolWindowSpec spec;
    spec.id = std::string(kDebugToolWindowId);
    spec.title = QStringLiteral("Debug");
    spec.side = ToolWindowSide::Bottom;
    spec.iconId = std::string(icons::kRunDebug);
    spec.createContent = std::move(content);
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

} // namespace
Result<void> registerDebugToolWindow(ToolWindowManager& windows, input::ActionRegistry& registry,
                                     DebugController& controller) {
    const QPointer<DebugController> guarded(&controller);
    return registerWindow(windows, registry,
                          [guarded]() -> QWidget* { return guarded ? new DebugToolWindow(*guarded) : nullptr; });
}
Result<void> registerDebugToolWindow(ToolWindowManager& windows, input::ActionRegistry& registry,
                                     DebugSessionRegistry& sessions, std::function<QWidget*()> mixedFactory) {
    const QPointer<DebugSessionRegistry> guarded(&sessions);
    return registerWindow(windows, registry, [guarded, mixedFactory = std::move(mixedFactory)]() -> QWidget* {
        if (!guarded)
            return nullptr;
        auto* window = new DebugSessionsWindow(*guarded);
        if (mixedFactory)
            window->setMixedDebugWidget(mixedFactory());
        return window;
    });
}
} // namespace frost::ui
