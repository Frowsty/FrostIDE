// SPDX-License-Identifier: Apache-2.0
//
// DebugController (see frost/ui/debug_controller.hpp): stepping, evaluation, disassembly/memory
// queries, breakpoint commands, and editor gutter/inline-value/hover synchronization.

#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/run_console.hpp"

#include "debug_controller_internal.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace frost::ui {

// --- Commands ---

bool DebugController::report(Result<void> result, const QString& what) {
    if (result) {
        return true;
    }
    log::warn("debug", "{}: {}", what.toStdString(), result.error().message());
    if (m_hooks.notify) {
        m_hooks.notify(what, QString::fromStdString(result.error().message()), true);
    }
    return false;
}

bool DebugController::resume() {
    const bool resumed = m_session && report(m_session->continueRun(), QStringLiteral("Resume"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::pause() { return m_session && report(m_session->pause(), QStringLiteral("Pause")); }

bool DebugController::stepOver() {
    const bool resumed = m_session && report(m_session->next(), QStringLiteral("Step Over"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::stepInto() {
    const bool resumed = m_session && report(m_session->stepIn(), QStringLiteral("Step Into"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::stepOut() {
    const bool resumed = m_session && report(m_session->stepOut(), QStringLiteral("Step Out"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::validateCoreClrPath(std::string path, std::function<void(DebugController&)> approved) {
    if (!m_coreClrBreakpointValidator ||
        std::ranges::find(m_coreClrAllowedSources, path) == m_coreClrAllowedSources.end())
        return false;
    const auto sequence = ++m_coreClrPathRequests[path];
    const auto epoch = m_coreClrValidationEpoch;
    const auto generation = m_generation;
    const auto relay = m_relay;
    std::string requestedPath = path;
    m_coreClrBreakpointValidator(std::move(requestedPath), [relay, generation, epoch, sequence, path = std::move(path),
                                                            approved = std::move(approved)](bool ok) {
        relay->post(generation, [ok, epoch, sequence, path, approved](DebugController* self) {
            const auto it = self->m_coreClrPathRequests.find(path);
            if (ok && self->m_session && self->m_coreClrSession && self->m_coreClrValidationEpoch == epoch &&
                it != self->m_coreClrPathRequests.end() && it->second == sequence)
                approved(*self);
        });
    });
    return true;
}

bool DebugController::runToCursor() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (!m_session || view == nullptr) {
        return false;
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    auto path = io::normalizePath(view->path());
    if (!path) {
        return false;
    }
    if (m_coreClrSession) {
        const auto targetLine = static_cast<std::int64_t>(line.value()) + 1;
        return validateCoreClrPath(*path, [path = *path, targetLine](DebugController& self) {
            static_cast<void>(
                self.report(self.m_session->runToLine(path, targetLine), QStringLiteral("Run to Cursor")));
        });
    }
    const bool resumed = report(m_session->runToLine(*path, static_cast<std::int64_t>(line.value()) + 1),
                                QStringLiteral("Run to Cursor"));
    if (resumed)
        clearLoadedSource();
    return resumed;
}

bool DebugController::setNextStatement() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (!m_session || !paused() || view == nullptr) {
        return false;
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    auto path = io::normalizePath(view->path());
    if (!path) {
        return false;
    }
    const std::int64_t threadId = m_thread.value_or(m_threads.empty() ? 1 : m_threads.front().id);
    const std::int64_t targetLine = static_cast<std::int64_t>(line.value()) + 1;
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->gotoLine(threadId, *path, targetLine, [relay, generation](Result<void> res) {
        relay->post(generation, [res = std::move(res)](DebugController* self) mutable {
            static_cast<void>(self->report(std::move(res), QStringLiteral("Set Next Statement")));
        });
    });
    return true;
}

void DebugController::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_rerunPending = false;
    m_rerunTimer.stop();
    ++m_generation;
    if (m_pendingRun && m_service)
        m_service->stop(*m_pendingRun);
    m_pendingRun.reset();
    if (m_alternatePending && m_hooks.stopAlternate)
        m_hooks.stopAlternate();
    m_alternatePending = false;
    m_killTimer.stop();
    m_session.reset();
    m_state = debug::DebugState::Ended;
    m_stop.reset();
    m_frame.reset();
    m_currentFrameVariables.clear();
    m_frameScopeReferences.clear();
    m_hoverCache.clear();
    clearLoadedSource();
    showExecutionPoint();
    emit stateChanged();
}

void DebugController::cancelRerun() {
    m_rerunPending = false;
    m_rerunTimer.stop();
}

void DebugController::rerun() {
    if (m_coreClrSession || m_lastCoreClrSession) {
        if (m_hooks.notify)
            m_hooks.notify(QStringLiteral("Debug .NET"), QStringLiteral("Start the .NET project again to rebuild it."),
                           false);
        return;
    }
    if (!m_rerunAllowed) {
        if (m_hooks.notify)
            m_hooks.notify(QStringLiteral("Debug"), QStringLiteral("Rerun is not available for an attached session"),
                           false);
        return;
    }
    stop();
    m_rerunPending = true;
    m_rerunTimer.start();
}

Result<void> DebugController::terminateTarget(std::int64_t expectedPid, std::uint64_t expectedGeneration,
                                              debug::DebugSession::TerminateDone done) {
    if (!nativeSession() || m_generation != expectedGeneration || m_session->processId() != expectedPid)
        return fail(ErrorCode::PermissionDenied, "The selected engine is not this native debugger session");
    m_rerunPending = false;
    m_rerunTimer.stop();
    const auto relay = m_relay;
    return m_session->terminateDebuggee(
        expectedPid, [relay, expectedGeneration, done = std::move(done)](Result<void> result) mutable {
            relay->post(expectedGeneration, [done = std::move(done),
                                             result = std::move(result)](DebugController* self) mutable {
                if (!result)
                    self->report(Result<void>(std::unexpected(result.error())), QStringLiteral("Stop FrostEngine"));
                if (done)
                    done(std::move(result));
                emit self->stateChanged();
            });
        });
}

void DebugController::stop() {
    ++m_coreClrValidationEpoch;
    m_rerunPending = false;
    m_rerunTimer.stop();
    if (m_alternatePending) {
        ++m_generation;
        m_alternatePending = false;
        if (m_hooks.stopAlternate)
            m_hooks.stopAlternate();
        emit stateChanged();
    }
    if (m_pendingRun) {
        const auto pending = *m_pendingRun;
        m_pendingRun.reset();
        ++m_generation;
        if (m_service)
            m_service->stop(pending);
        emit stateChanged();
    }
    if (!m_session) {
        return;
    }
    m_session->stop();
    if (!m_killTimer.isActive()) {
        m_killTimer.start();
    }
}

void DebugController::selectThread(std::int64_t threadId) {
    if (!m_session) {
        return;
    }
    m_thread = threadId;
    m_frames = m_session->frames(threadId);
    if (m_coreClrFrameAllowed)
        std::erase_if(m_frames.frames, [this](const auto& frame) { return !m_coreClrFrameAllowed(frame); });
    m_frame.reset();
    emit framesChanged();
    static_cast<void>(report(m_session->selectThread(threadId), QStringLiteral("Select Thread")));
}

void DebugController::selectFrame(std::int64_t frameId) {
    if (!m_session || !m_thread) {
        return;
    }
    static_cast<void>(report(m_session->selectFrame(*m_thread, frameId), QStringLiteral("Select Frame")));
}

void DebugController::loadMoreFrames() {
    if (m_session && m_thread) {
        m_session->loadMoreFrames(*m_thread);
    }
}

void DebugController::scopes(std::int64_t frameId, ScopesDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->scopes(frameId, [relay, generation, frameId, done = std::move(done)](Result<debug::ScopeList> list) {
        relay->post(generation, [done, frameId, list = std::move(list)](DebugController* self) {
            if (list && self->m_frame && self->m_frame->id == frameId) {
                // Inline values and hovers use these scopes' own variables only (refreshInlineDebuggerValues).
                self->m_frameScopeReferences.clear();
                for (const debug::dap::Scope& scope : *list) {
                    if (!scope.expensive && scope.variablesReference > 0) {
                        self->m_frameScopeReferences.push_back(scope.variablesReference);
                    }
                }
            }
            done(list);
        });
    });
}

void DebugController::variables(std::int64_t reference, std::int64_t start, std::int64_t count, VariablesDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->variables(
        reference, start, count,
        [relay, generation, reference, done = std::move(done)](Result<debug::VariableList> list) {
            relay->post(generation, [done, reference, list = std::move(list)](DebugController* self) {
                if (list && self->paused() && std::ranges::contains(self->m_frameScopeReferences, reference)) {
                    for (const debug::dap::Variable& var : *list) {
                        if (var.name.empty() || var.value.empty()) {
                            continue;
                        }
                        const auto existing =
                            std::ranges::find(self->m_currentFrameVariables, var.name, &debug::dap::Variable::name);
                        if (existing == self->m_currentFrameVariables.end()) {
                            self->m_currentFrameVariables.push_back(var);
                        } else {
                            *existing = var;
                        }
                    }
                    self->refreshInlineDebuggerValues();
                }
                done(list);
            });
        });
}

void DebugController::evaluate(const QString& expression, const QString& context, EvaluateDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    const std::optional<std::int64_t> frame = m_frame ? std::optional<std::int64_t>(m_frame->id) : std::nullopt;
    m_session->evaluate(expression.toStdString(), frame, context.toStdString(),
                        [relay, generation, done = std::move(done)](Result<debug::dap::Variable> result) {
                            relay->post(generation,
                                        [done, result = std::move(result)](DebugController*) { done(result); });
                        });
}

void DebugController::submitConsoleInput(const QString& line) {
    if (!m_session) {
        return;
    }
    const QString trimmed = line.trimmed();
    if (m_console) {
        m_console->appendOutput(build::RunOutputKind::System, std::format("> {}\n", trimmed.toStdString()));
    }
    if (trimmed.isEmpty()) {
        return;
    }
    evaluate(trimmed, QStringLiteral("repl"), [this](Result<debug::dap::Variable> res) {
        if (!m_console) {
            return;
        }
        if (res) {
            if (!res->value.empty()) {
                m_console->appendOutput(build::RunOutputKind::Stdout, res->value + "\n");
            }
        } else {
            m_console->appendOutput(build::RunOutputKind::Stderr, res.error().message() + "\n");
        }
    });
}

void DebugController::setVariableValue(std::int64_t variablesReference, const QString& name, const QString& value,
                                       EvaluateDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->setVariable(
        variablesReference, name.toStdString(), value.toStdString(),
        [relay, generation, done = std::move(done)](Result<debug::dap::Variable> res) {
            relay->post(generation, [done, res = std::move(res)](DebugController* self) mutable {
                if (!res) {
                    static_cast<void>(self->report(std::unexpected(res.error()), QStringLiteral("Set Variable Value")));
                }
                if (done) {
                    done(std::move(res));
                }
            });
        });
}

bool DebugController::setExceptionFilterEnabled(std::string_view filter, bool enabled) {
    return m_breakpoints.setExceptionFilterEnabled(filter, enabled);
}

Result<debug::BreakpointId> DebugController::addFunctionBreakpoint(const QString& name) {
    return m_breakpoints.addFunction(name.trimmed().toStdString());
}

void DebugController::disassemble(const QString& memoryReference, int instructionCount, DisassembleDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->disassemble(
        memoryReference.trimmed().toStdString(), instructionCount,
        [relay, generation, done = std::move(done)](Result<std::vector<debug::dap::DisassembledInstruction>> res) {
            relay->post(generation, [done, res = std::move(res)](DebugController*) mutable {
                if (done) {
                    done(std::move(res));
                }
            });
        });
}

void DebugController::readMemory(const QString& memoryReference, int count, ReadMemoryDone done) {
    if (!m_session) {
        return;
    }
    const std::shared_ptr<Relay> relay = m_relay;
    const std::uint64_t generation = m_generation;
    m_session->readMemory(memoryReference.trimmed().toStdString(), count,
                          [relay, generation, done = std::move(done)](Result<debug::dap::ReadMemoryResult> res) {
                              relay->post(generation, [done, res = std::move(res)](DebugController*) mutable {
                                  if (done) {
                                      done(std::move(res));
                                  }
                              });
                          });
}

// --- Breakpoints ---

void DebugController::toggleBreakpoint(const std::string& path, int line) {
    auto normal = io::normalizePath(path);
    if (!normal) {
        return;
    }
    if (auto toggled = m_breakpoints.toggle(*normal, static_cast<std::int64_t>(line) + 1); !toggled) {
        log::warn("debug", "Toggle Breakpoint: {}", toggled.error().message());
    }
}

bool DebugController::toggleBreakpointAtCaret() {
    EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr;
    if (view == nullptr) {
        return false;
    }
    attachEditor(view);
    if (const auto it = m_editors.find(view); it != m_editors.end() && it->second->tracker) {
        it->second->tracker->sync();
    }
    const LineIndex line = view->editor().text().lineAt(view->editor().state().primary().head);
    toggleBreakpoint(view->path(), static_cast<int>(line.value()));
    return true;
}

void DebugController::setMutedBreakpoints(bool muted) { m_breakpoints.setMuted(muted); }

void DebugController::toggleMuteBreakpoints() { m_breakpoints.setMuted(!m_breakpoints.muted()); }

void DebugController::requestEditBreakpoint() {
    debug::BreakpointId targetId = 0;
    if (EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr) {
        attachEditor(view);
        if (const auto it = m_editors.find(view); it != m_editors.end() && it->second->tracker) {
            it->second->tracker->sync();
        }
        const std::int64_t line =
            static_cast<std::int64_t>(view->editor().text().lineAt(view->editor().state().primary().head).value()) + 1;
        if (auto norm = io::normalizePath(view->path())) {
            if (auto existing = m_breakpoints.at(*norm, line)) {
                targetId = existing->id;
            } else if (auto added = m_breakpoints.add(*norm, line)) {
                targetId = *added;
            }
        }
    }
    if (m_hooks.showToolWindow) {
        m_hooks.showToolWindow();
    }
    emit editBreakpointRequested(targetId);
}

void DebugController::requestViewBreakpoints() {
    if (m_hooks.showToolWindow) {
        m_hooks.showToolWindow();
    }
    emit viewBreakpointsRequested();
}

void DebugController::requestEvaluate() {
    QString expression;
    if (EditorView* view = m_hooks.currentEditor ? m_hooks.currentEditor() : nullptr) {
        const text::Selection primary = view->editor().state().primary();
        if (!primary.empty() && primary.range().size() <= 4096) {
            expression = QString::fromStdString(view->editor().text().text(primary.range())).trimmed();
        }
    }
    emit evaluateRequested(expression);
}

void DebugController::attachEditor(EditorView* view) {
    if (view == nullptr || m_editors.contains(view)) {
        return;
    }
    auto path = io::normalizePath(view->path());
    if (!path) {
        return; // An untitled buffer has no breakpoints.
    }
    auto attached = std::make_unique<Attached>();
    attached->view = view;
    attached->path = *path;
    attached->tracker = std::make_unique<debug::DocumentBreakpointTracker>(m_breakpoints, view->document(), *path);
    Attached* raw = attached.get();
    attached->connections.push_back(connect(view, &EditorView::breakpointToggleRequested, this, [this, raw](int line) {
        if (raw->tracker) {
            raw->tracker->sync();
        }
        toggleBreakpoint(raw->path, line);
    }));
    attached->connections.push_back(connect(view, &EditorView::textChanged, this, [raw] {
        if (raw->tracker) {
            raw->tracker->sync();
        }
    }));
    attached->connections.push_back(connect(view, &QObject::destroyed, this, [this, view] { m_editors.erase(view); }));
    view->setBreakpointGutterEnabled(true);
    view->setDebugHoverProvider([this](std::string_view expr, LineIndex line) { return debugHoverFor(expr, line); });
    m_editors.emplace(view, std::move(attached));
    refreshMarkers(*view);
    showExecutionPoint();
}

void DebugController::detachEditor(EditorView* view) {
    const auto it = m_editors.find(view);
    if (it == m_editors.end()) {
        return;
    }
    for (const QMetaObject::Connection& connection : it->second->connections) {
        disconnect(connection);
    }
    if (it->second->view) {
        it->second->view->setBreakpointMarkers({});
        it->second->view->setExecutionPoint(std::nullopt);
        it->second->view->setInlineDebuggerValues({});
        it->second->view->setDebugHoverProvider({});
        it->second->view->setBreakpointGutterEnabled(false);
    }
    m_editors.erase(it);
}

void DebugController::refreshMarkers(EditorView& view) {
    const auto it = m_editors.find(&view);
    if (it == m_editors.end()) {
        return;
    }
    std::vector<BreakpointMarker> markers;
    for (const debug::LineBreakpoint& breakpoint : m_breakpoints.inFile(it->second->path)) {
        const bool unverified = breakpoint.status == debug::BreakpointStatus::Unverified;
        markers.push_back(BreakpointMarker{
            .line = LineIndex(static_cast<std::uint32_t>(breakpoint.line - 1)),
            .kind = unverified ? BreakpointKind::Unverified : BreakpointKind::Verified,
            .message = unverified ? (breakpoint.message.empty() ? std::string("The breakpoint is not bound to code")
                                                                : breakpoint.message)
                                  : std::string()});
    }
    view.setBreakpointMarkers(std::move(markers));
}

void DebugController::onBreakpointsChanged(std::string_view path, debug::BreakpointManager::Change change) {
    for (auto& [view, attached] : m_editors) {
        if (attached->view && m_breakpoints.samePath(attached->path, path)) {
            refreshMarkers(*attached->view);
        }
    }
    if (change == debug::BreakpointManager::Change::Lines) {
        if (m_session && m_coreClrSession) {
            const std::string file(path);
            static_cast<void>(validateCoreClrPath(file, [file](DebugController& self) {
                self.m_session->setBreakpoints(file, self.m_breakpoints.sessionLines(file));
            }));
        } else if (m_session) {
            m_session->setBreakpoints(std::string(path), m_breakpoints.sessionLines(path));
        }
        save();
    } else if (change == debug::BreakpointManager::Change::Functions) {
        if (m_session && m_nativeSession) {
            m_session->setFunctionBreakpoints(m_breakpoints.sessionFunctions());
        }
        save();
    } else if (change == debug::BreakpointManager::Change::Exceptions) {
        if (m_session && m_nativeSession) {
            m_session->setExceptionFilters(m_breakpoints.sessionExceptionFilters());
            m_session->setThrowTypes(m_breakpoints.sessionThrowTypes());
        }
        save();
    }
    emit breakpointsChanged();
}

void DebugController::clearLoadedSource() {
    if (m_sourceEpoch == std::numeric_limits<std::uint64_t>::max())
        m_sourceEpochExhausted = true;
    else
        ++m_sourceEpoch;
    emit loadedSourceChanged({}, {}, 0);
}

void DebugController::showExecutionPoint() {
    for (auto& [view, attached] : m_editors) {
        if (!attached->view) {
            continue;
        }
        if (!m_coreClrFrameAllowed && m_frame && (m_coreClrSession || m_frame->sourceReference == 0) &&
            m_frame->hasSource() && m_breakpoints.samePath(attached->path, m_frame->path)) {
            attached->view->setExecutionPoint(LineIndex(static_cast<std::uint32_t>(m_frame->line - 1)));
        } else {
            attached->view->setExecutionPoint(std::nullopt);
        }
    }
    refreshInlineDebuggerValues();
}

void DebugController::refreshInlineDebuggerValues() {
    const bool active = paused() && m_frame && m_frame->hasSource() && !m_currentFrameVariables.empty() &&
                        (m_coreClrSession || m_frame->sourceReference == 0);
    for (auto& [view, attached] : m_editors) {
        if (!attached->view) {
            continue;
        }
        if (!active || !m_breakpoints.samePath(attached->path, m_frame->path)) {
            attached->view->setInlineDebuggerValues({});
            continue;
        }
        const text::Snapshot& snapshot = attached->view->editor().text();
        const std::uint32_t lineCount = snapshot.lineCount();
        if (lineCount == 0) {
            attached->view->setInlineDebuggerValues({});
            continue;
        }
        const std::uint32_t stoppedLine = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(std::max<std::int64_t>(1, m_frame->line) - 1), lineCount - 1);
        const std::uint32_t firstLine = stoppedLine > 40 ? stoppedLine - 40 : 0;
        const std::uint32_t lastLine = std::min<std::uint32_t>(stoppedLine + 2, lineCount - 1);
        std::vector<EditorInlineValue> inlineValues;
        for (std::uint32_t l = firstLine; l <= lastLine; ++l) {
            const auto info = snapshot.lineInfo(LineIndex{l});
            if (!info || info->content.empty()) {
                continue;
            }
            const std::string lineText = snapshot.text(info->content);
            std::string combined;
            std::size_t countOnLine = 0;
            for (const debug::dap::Variable& var : m_currentFrameVariables) {
                if (lineContainsVariable(lineText, var.name)) {
                    if (!combined.empty()) {
                        combined += "  ";
                    }
                    std::string val = var.value;
                    if (val.size() > 48) {
                        val = val.substr(0, 45) + "...";
                    }
                    combined += var.name + " = " + val;
                    if (++countOnLine >= 4) {
                        break;
                    }
                }
            }
            if (!combined.empty()) {
                inlineValues.push_back(EditorInlineValue{
                    .line = LineIndex{l},
                    .text = std::move(combined),
                    .tooltip = "Inline debugger variable values",
                });
            }
        }
        attached->view->setInlineDebuggerValues(std::move(inlineValues));
    }
}

QString DebugController::debugHoverFor(std::string_view expression, LineIndex) {
    if (!paused() || !m_session) {
        return {};
    }
    while (!expression.empty() && (expression.front() == ' ' || expression.front() == '\t')) {
        expression.remove_prefix(1);
    }
    while (!expression.empty() && (expression.back() == ' ' || expression.back() == '\t')) {
        expression.remove_suffix(1);
    }
    if (expression.empty()) {
        return {};
    }
    for (const debug::dap::Variable& var : m_currentFrameVariables) {
        if (var.name == expression) {
            if (!var.type.empty()) {
                return QString::fromStdString(std::format("{} ({}) = {}", var.name, var.type, var.value));
            }
            return QString::fromStdString(std::format("{} = {}", var.name, var.value));
        }
    }
    if (const auto it = m_hoverCache.find(expression); it != m_hoverCache.end()) {
        return QString::fromStdString(it->second);
    }
    const std::string exprStr(expression);
    evaluate(QString::fromStdString(exprStr), QStringLiteral("hover"),
             [this, exprStr](Result<debug::dap::Variable> res) {
                 if (!res || res->value.empty()) {
                     return;
                 }
                 const std::string formatted = !res->type.empty()
                                                   ? std::format("{} ({}) = {}", exprStr, res->type, res->value)
                                                   : std::format("{} = {}", exprStr, res->value);
                 m_hoverCache[exprStr] = formatted;
             });
    return {};
}

} // namespace frost::ui
