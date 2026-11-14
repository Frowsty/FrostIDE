// SPDX-License-Identifier: Apache-2.0
// Actual Lua pause -> actual native all-stop -> ordered native/Lua continuation.
#include "frost/ui/debug_session_registry.hpp"

#include "engine_mixed_debug.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>
namespace frost::app {
struct EngineMixedDebugCoordinator::Impl {
    enum class Phase { Idle, Capture, Combined, NativeContinue, LuaContinue };
    struct Pair {
        EngineNativeDebugBinding native;
        EngineLuaDebugBinding lua;
        bool same(const Pair& other) const {
            return native == other.native && lua.registrySessionId == other.lua.registrySessionId &&
                   lua.controllerGeneration == other.lua.controllerGeneration && lua.identity == other.lua.identity &&
                   lua.epoch == other.lua.epoch && lua.sessionEpoch == other.lua.sessionEpoch &&
                   lua.trustRevision == other.lua.trustRevision && lua.root == other.lua.root;
        }
    };
    struct Capture {
        Pair pair;
        debug::dap::ScriptPauseIdentity pause;
        std::int64_t luaThread = 0;
        debug::FrameList luaFrames;
        std::optional<std::uint64_t> nativeStop{};
        bool requestedFrames = false;
    };
    EngineMixedDebugCoordinator* q;
    ui::DebugSessionRegistry& registry;
    LuaSupplier lua;
    NativeSupplier native;
    NativeMapping mapping;
    QTimer deadline;
    Phase phase = Phase::Idle;
    std::optional<Capture> capture;
    ui::MixedDebugSnapshot snapshot;
    std::map<std::uint64_t, QPointer<ui::DebugController>> observed;
    QString failure;
    bool refreshing = false;

    Impl(EngineMixedDebugCoordinator* owner, ui::DebugSessionRegistry& sessions, LuaSupplier luaSupplier,
         NativeSupplier nativeSupplier, NativeMapping verifier)
        : q(owner), registry(sessions), lua(std::move(luaSupplier)), native(std::move(nativeSupplier)),
          mapping(std::move(verifier)) {
        deadline.setSingleShot(true);
        deadline.setInterval(15000);
        QObject::connect(&deadline, &QTimer::timeout, q, [this] {
            abort(phase == Phase::NativeContinue
                      ? QStringLiteral("Native continuation was not confirmed; Lua continuation was not sent")
                  : phase == Phase::Capture
                      ? QStringLiteral("The native adapter did not confirm the Lua execution OS-thread stack")
                      : QStringLiteral("Lua continuation was not confirmed"));
        });
    }
    void clear(QString reason) {
        deadline.stop();
        phase = Phase::Idle;
        capture.reset();
        failure = std::move(reason);
    }
    void abort(QString reason) {
        clear(std::move(reason));
        emit q->failed(failure);
        q->refresh();
    }
    void watch() {
        std::erase_if(observed, [this](const auto& entry) {
            return !entry.second || registry.controller(entry.first) != entry.second;
        });
        for (const auto& entry : registry.sessions()) {
            if (observed[entry.id] == entry.controller)
                continue;
            observed[entry.id] = entry.controller;
            for (auto signal : {&ui::DebugController::stateChanged, &ui::DebugController::framesChanged,
                                &ui::DebugController::threadsChanged})
                QObject::connect(entry.controller, signal, q, [this] { q->refresh(); });
            QObject::connect(entry.controller, &ui::DebugController::adapterContinued, q,
                             [this, id = entry.id](std::uint64_t generation) { continued(id, generation); });
        }
    }
    std::optional<Pair> pair(QString& reason) const {
        const auto n = native ? native() : std::nullopt;
        if (!n || !n->controller || registry.controller(n->registrySessionId) != n->controller ||
            n->controller->sessionGeneration() != n->debugGeneration || !n->controller->session() ||
            n->controller->session()->processId() != n->target.pid || n->target.pid <= 0 ||
            n->controller->session()->state() == debug::DebugState::Ended) {
            reason = QStringLiteral("No native debugger is bound to the authenticated engine");
            return {};
        }
        if (n->controller->session()->adapterId() != "lldb-dap" || !mapping || !mapping(*n)) {
            reason = QStringLiteral("Same-pause capture requires the verified pinned LLDB thread mapping");
            return {};
        }
        std::optional<EngineLuaDebugBinding> match;
        for (const auto& entry : registry.sessions()) {
            const auto candidate = lua ? lua(entry.id) : std::nullopt;
            if (!candidate || candidate->registrySessionId != entry.id || !candidate->current ||
                !candidate->current() || !entry.controller->session() ||
                entry.controller->session()->adapterId() != "frost-lua" ||
                entry.controller->session()->state() == debug::DebugState::Ended ||
                entry.controller->sessionGeneration() != candidate->controllerGeneration)
                continue;
            const auto& i = candidate->identity;
            const auto& t = n->target;
            if (i.processId != t.pid || i.instanceId != t.instanceId || i.generation != t.serverGeneration ||
                i.connectionGeneration != t.connectionGeneration || candidate->root != t.projectRoot ||
                candidate->sessionEpoch != t.sessionEpoch || candidate->trustRevision != t.trustRevision ||
                i.debugSessionId.empty())
                continue;
            if (match) {
                reason = QStringLiteral("Multiple Lua sessions match this engine; same-pause binding is ambiguous");
                return {};
            }
            match = candidate;
        }
        if (!match) {
            reason = QStringLiteral("No current Lua debugger matches the authenticated native engine");
            return {};
        }
        return Pair{*n, *match};
    }
    ui::DebugController* luaController(const Pair& p) const { return registry.controller(p.lua.registrySessionId); }
    bool samePause(const Capture& saved) const {
        const auto* child = luaController(saved.pair);
        if (!child || !child->session() || child->session()->state() != debug::DebugState::Stopped)
            return false;
        const auto stop = child->session()->stopInfo();
        return stop && stop->scriptPause == saved.pause && stop->threadId == saved.luaThread;
    }
    bool ready(const Pair& p, QString& reason) const {
        const auto* child = luaController(p);
        if (!child || !child->session() || child->session()->state() != debug::DebugState::Stopped) {
            reason = QStringLiteral("Pause Lua at a real script breakpoint before capturing C++");
            return false;
        }
        const auto stop = child->session()->stopInfo();
        if (!stop || !stop->threadId || !stop->scriptPause ||
            stop->scriptPause->osThreadId > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            reason = QStringLiteral("The Lua stop has no usable OS-thread, VM and pause identity");
            return false;
        }
        if (child->session()->frames(*stop->threadId).frames.empty()) {
            reason = QStringLiteral("Waiting for the actual Lua pause stack");
            return false;
        }
        return true;
    }
    void continued(std::uint64_t id, std::uint64_t generation) {
        if (!capture)
            return;
        if (phase == Phase::NativeContinue && id == capture->pair.native.registrySessionId &&
            generation == capture->pair.native.debugGeneration) {
            // Native evidence/heartbeat observers finish before any Lua command is admitted.
            QMetaObject::invokeMethod(q, [this] { continueLua(); }, Qt::QueuedConnection);
        } else if (phase == Phase::LuaContinue && id == capture->pair.lua.registrySessionId &&
                   generation == capture->pair.lua.controllerGeneration) {
            clear({});
            q->refresh();
        } else if (id == capture->pair.native.registrySessionId || id == capture->pair.lua.registrySessionId) {
            abort(QStringLiteral("A debugger continued; the combined pause was cleared"));
        }
    }
    void continueLua() {
        if (phase != Phase::NativeContinue || !capture)
            return;
        QString reason;
        const auto current = pair(reason);
        auto* n = capture->pair.native.controller.data();
        if (!current || !current->same(capture->pair) || !samePause(*capture) || !n || !n->session() ||
            n->adapterStoppedEvidence() || n->session()->state() != debug::DebugState::Running) {
            abort(QStringLiteral("The exact native continuation or Lua pause is no longer current"));
            return;
        }
        phase = Phase::LuaContinue;
        if (!luaController(capture->pair)->resume())
            abort(QStringLiteral("Lua continuation was refused after native continued"));
        else
            q->refresh();
    }
};

EngineMixedDebugCoordinator::EngineMixedDebugCoordinator(ui::DebugSessionRegistry& registry,
                                                         EngineLuaDebugCoordinator& lua, NativeSupplier native,
                                                         NativeMapping mapping, QObject* parent)
    : EngineMixedDebugCoordinator(
          registry, [&lua](std::uint64_t id) { return lua.binding(id); }, std::move(native), std::move(mapping),
          parent) {
    connect(&lua, &EngineLuaDebugCoordinator::bindingChanged, this, [this] { refresh(); });
}
EngineMixedDebugCoordinator::EngineMixedDebugCoordinator(ui::DebugSessionRegistry& registry, LuaSupplier lua,
                                                         NativeSupplier native, NativeMapping mapping, QObject* parent)
    : QObject(parent),
      m_impl(std::make_unique<Impl>(this, registry, std::move(lua), std::move(native), std::move(mapping))) {
    connect(&registry, &ui::DebugSessionRegistry::sessionsChanged, this, &EngineMixedDebugCoordinator::refresh);
    refresh();
}
EngineMixedDebugCoordinator::~EngineMixedDebugCoordinator() = default;
const ui::MixedDebugSnapshot& EngineMixedDebugCoordinator::snapshot() const { return m_impl->snapshot; }
void EngineMixedDebugCoordinator::refresh() {
    auto& s = *m_impl;
    if (s.refreshing)
        return;
    s.refreshing = true;
    s.watch();
    ui::MixedDebugSnapshot next;
    QString reason;
    const auto p = s.pair(reason);
    if (s.capture &&
        (!p || !p->same(s.capture->pair) || (s.phase != Impl::Phase::LuaContinue && !s.samePause(*s.capture))))
        s.clear(QStringLiteral("The engine identity or Lua pause changed; combined stacks were cleared"));
    if (!p) {
        next.status = reason;
    } else {
        auto* n = p->native.controller.data();
        next.processId = p->native.target.pid;
        if (s.phase == Impl::Phase::NativeContinue || s.phase == Impl::Phase::LuaContinue) {
            next.status = s.phase == Impl::Phase::NativeContinue
                              ? QStringLiteral("Waiting for actual native continuation; Lua continuation was not sent")
                              : QStringLiteral("Native continued; waiting for actual Lua continuation");
        } else if (s.capture) {
            auto& saved = *s.capture;
            next.status = QStringLiteral("Waiting for the actual native OS-thread stack");
            if (n->session()->state() == debug::DebugState::Stopped && n->adapterStoppedEvidence()) {
                const auto serial = n->adapterEvidenceSequence();
                if (!saved.nativeStop)
                    saved.nativeStop = serial;
                if (*saved.nativeStop != serial) {
                    s.clear(QStringLiteral("A newer native pause replaced the combined pause"));
                } else {
                    const auto tid = static_cast<std::int64_t>(saved.pause.osThreadId);
                    const auto threads = n->session()->threads();
                    if (std::ranges::any_of(threads, [tid](const auto& thread) { return thread.id == tid; })) {
                        const auto frames = n->session()->frames(tid);
                        if (!frames.frames.empty()) {
                            s.phase = Impl::Phase::Combined;
                            s.deadline.stop();
                            next.status = QStringLiteral("Same Lua pause — process %1, OS thread %2, VM %3, pause %4")
                                              .arg(next.processId)
                                              .arg(tid)
                                              .arg(saved.pause.vmGeneration)
                                              .arg(saved.pause.stopGeneration);
                            next.pause = saved.pause;
                            next.native = ui::MixedDebugStack{p->native.registrySessionId, tid, "C++", frames};
                            next.lua =
                                ui::MixedDebugStack{p->lua.registrySessionId, saved.luaThread, "Lua", saved.luaFrames};
                            next.canResume = true;
                        } else if (!saved.requestedFrames) {
                            saved.requestedFrames = true;
                            if (auto selected = n->session()->selectThread(tid); !selected)
                                s.failure = QString::fromStdString(selected.error().message());
                        }
                    }
                }
            } else if (s.phase == Impl::Phase::Combined) {
                s.clear(QStringLiteral("The native pause changed; combined stacks were cleared"));
            }
        } else if (n->adapterStoppedEvidence() || n->session()->state() != debug::DebugState::Running) {
            next.status =
                QStringLiteral("Native debugger is stopped; Lua requests are unavailable until native continues");
        } else {
            next.canCapture = s.ready(*p, next.status);
            if (next.canCapture)
                next.status = QStringLiteral("Lua is paused; capture C++ to inspect the same OS-thread pause");
        }
    }
    if (!s.failure.isEmpty())
        next.status = s.failure;
    s.snapshot = std::move(next);
    s.refreshing = false;
    emit changed();
}
Result<void> EngineMixedDebugCoordinator::captureNativeStack() {
    refresh();
    auto& s = *m_impl;
    QString reason;
    const auto p = s.pair(reason);
    if (!p || !s.snapshot.canCapture)
        return fail(ErrorCode::Busy, s.snapshot.status.toStdString());
    auto* child = s.luaController(*p);
    const auto stop = child->session()->stopInfo();
    if (!stop || !stop->scriptPause || !stop->threadId ||
        p->native.controller->session()->state() != debug::DebugState::Running ||
        p->native.controller->adapterStoppedEvidence())
        return fail(ErrorCode::Busy, "The actual pause changed before capture");
    const auto frames = child->session()->frames(*stop->threadId);
    if (frames.frames.empty())
        return fail(ErrorCode::Busy, "The actual Lua stack is no longer available");
    s.failure.clear();
    s.capture = Impl::Capture{*p, *stop->scriptPause, *stop->threadId, frames};
    if (!s.samePause(*s.capture)) {
        s.capture.reset();
        return fail(ErrorCode::Busy, "The actual Lua pause changed during capture");
    }
    s.phase = Impl::Phase::Capture;
    s.deadline.start();
    if (!p->native.controller->pause()) {
        s.abort(QStringLiteral("The native debugger refused the pause request"));
        return fail(ErrorCode::Busy, "The native debugger refused the pause request");
    }
    refresh();
    return {};
}
Result<void> EngineMixedDebugCoordinator::resumeBoth() {
    refresh();
    auto& s = *m_impl;
    if (!s.snapshot.canResume || !s.capture)
        return fail(ErrorCode::Busy, "No current combined pause is available");
    s.failure.clear();
    s.phase = Impl::Phase::NativeContinue;
    s.deadline.start();
    if (!s.capture->pair.native.controller->resume()) {
        s.abort(QStringLiteral("Native continuation was refused; Lua continuation was not sent"));
        return fail(ErrorCode::Busy, "Native continuation was refused");
    }
    refresh();
    return {};
}
Result<void> EngineMixedDebugCoordinator::selectFrame(std::uint64_t id, std::int64_t thread, std::int64_t frame) {
    refresh();
    if (!m_impl->snapshot.canResume || !m_impl->capture || id != m_impl->capture->pair.native.registrySessionId)
        return fail(ErrorCode::Busy, "Only the current native stack can be selected while native all-stop holds Lua");
    if (thread != static_cast<std::int64_t>(m_impl->capture->pause.osThreadId))
        return fail(ErrorCode::InvalidArgument, "The frame belongs to another OS thread");
    auto* child = m_impl->capture->pair.native.controller.data();
    auto selected = child->session()->selectFrame(thread, frame);
    if (selected)
        static_cast<void>(m_impl->registry.select(id));
    return selected;
}
} // namespace frost::app
