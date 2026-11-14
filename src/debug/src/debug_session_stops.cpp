// SPDX-License-Identifier: Apache-2.0
//
// DebugSession (see frost/debug/debug_session.hpp): stop handling, thunk stepping, MSVC thrown-type
// RTTI inspection, lazy thread/frame loading, and resuming.

#include "frost/core/log.hpp"
#include "frost/debug/msvc_throw.hpp"

#include "debug_session_internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::debug {

// --- Stops ---

void DebugSession::State::stopped(const rpc::Json& body) {
    const dap::StoppedEvent event = dap::parseStoppedEvent(body);
    StopInfo info{.reason = event.reason,
                  .description = event.description,
                  .text = event.text,
                  .threadId = event.threadId,
                  .allThreadsStopped = event.allThreadsStopped,
                  .hitBreakpointIds = event.hitBreakpointIds,
                  .scriptPause = event.scriptPause};
    bool throwHit = false;
    bool stepInStop = false;
    {
        LockGuard lock(mutex);
        if (state == DebugState::Ended) {
            return;
        }
        stepInStop = stepInPhase != StepInPhase::None && info.reason == "step" && thunkHops < kThunkHops &&
                     caps.supportsSteppingGranularity && caps.supportsDisassembleRequest;
        const auto hit = [&](const std::optional<std::int64_t>& id) {
            return id && std::ranges::find(info.hitBreakpointIds, *id) != info.hitBreakpointIds.end();
        };
        if (hit(abortBreakpointId)) {
            info.reason = "abort";
            info.description = "Program called abort()";
        } else if (hit(throwBreakpointId)) {
            throwHit = true;
            info.reason = "exception";
            info.description = "C++ exception thrown";
            info.exceptionFilter = "cpp_throw";
            info.text = "C++ Throw";
            for (const dap::ExceptionFilter& filter : caps.exceptionBreakpointFilters) {
                if (filter.filter == "cpp_throw") {
                    info.text = filter.label;
                }
            }
        } else if (info.reason == "exception" && !info.text.empty()) {
            // lldb-dap names the filter that stopped by its label in `text` ("C++ Throw").
            for (const dap::ExceptionFilter& filter : caps.exceptionBreakpointFilters) {
                if (filter.label == info.text) {
                    info.exceptionFilter = filter.filter;
                }
            }
        }
        if (pauseRequested && isPauseException(info)) {
            info.reason = "pause";
        }
    }
    if (throwHit && info.threadId) {
        checkThrowThunk(std::move(info));
        return;
    }
    if (stepInStop && info.threadId) {
        checkStepThunk(std::move(info));
        return;
    }
    announceStop(std::move(info));
}

/*
 * The top frame of `thread` and whether its instruction is a jmp (asked only for a frame without
 * a source); `then` gets nullopt when the stack trace failed. A failed disassemble counts as "not
 * a jmp".
 */
void DebugSession::State::topFrame(std::int64_t thread,
                                   std::function<void(std::optional<dap::StackFrame> top, bool jump)> then) {
    send("stackTrace", rpc::Json::object().set("threadId", thread).set("startFrame", 0).set("levels", 1),
         [weak = weak(), then = std::move(then)](Result<rpc::Json> body) mutable {
             const auto self = weak.lock();
             if (!self) {
                 return;
             }
             std::optional<dap::StackFrame> top;
             if (body) {
                 if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr && stack->size() > 0) {
                     top = dap::parseStackFrame(stack->items().front());
                 }
             }
             if (!top || hasSource(*top) || top->instructionPointerReference.empty()) {
                 then(std::move(top), false);
                 return;
             }
             const std::string address = top->instructionPointerReference;
             self->send("disassemble", dap::disassembleArguments(address, 1, 0),
                        [then = std::move(then), top = std::move(top)](Result<rpc::Json> answer) mutable {
                            const auto instructions = answer ? dap::parseDisassembleResponse(*answer)
                                                             : std::vector<dap::DisassembledInstruction>{};
                            then(std::move(top), !instructions.empty() && isJump(instructions.front().instruction));
                        });
         });
}

// A step-in's stop: follows a thunk, or reports the stop (header comment, "Step into a thunk").
void DebugSession::State::checkStepThunk(StopInfo info) {
    const std::int64_t thread = *info.threadId;
    topFrame(
        thread, [weak = weak(), info = std::move(info), thread](std::optional<dap::StackFrame> top, bool jump) mutable {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            std::string command;
            rpc::Json arguments = rpc::Json::object().set("threadId", thread);
            {
                LockGuard lock(self->mutex);
                if (self->state == DebugState::Ended || self->stopRequested) {
                    return;
                }
                const StepInPhase phase = self->stepInPhase;
                if (top && jump) {
                    command = "stepIn"; // Through the thunk's jmp.
                    arguments.set("granularity", "instruction");
                    self->stepInPhase = StepInPhase::FollowedThunk;
                } else if (top && ((phase == StepInPhase::FollowedThunk && hasSource(*top) &&
                                    top->name.starts_with(kStepAvoidPrefix)) ||
                                   (!hasSource(*top) && self->stepOutOfNoDebug &&
                                    (phase == StepInPhase::Stepping || phase == StepInPhase::FollowedThunk)))) {
                    // Step out of an avoided function or a frame whose debug information is missing.
                    command = "stepOut";
                    self->stepInPhase = StepInPhase::LeftAvoided;
                } else if (top && phase == StepInPhase::LeftAvoided && self->stepInOrigin &&
                           top->name == self->stepInOrigin->first && top->line == self->stepInOrigin->second) {
                    command = "stepIn"; // Back on the line the step began on: on to its next call.
                    self->stepInPhase = StepInPhase::Stepping;
                }
                if (!command.empty()) {
                    ++self->thunkHops;
                }
            }
            if (command.empty()) {
                self->announceStop(std::move(info));
                return;
            }
            self->send(command, std::move(arguments), [weak = self->weak(), info](Result<rpc::Json> answer) mutable {
                const auto state = weak.lock();
                if (state && !answer) {
                    state->announceStop(std::move(info)); // The stop as it came.
                }
            });
        });
}

// A _CxxThrowException stop: resumes it when it is the import thunk, else reads the thrown type
// (header comment, "Exception filters").
void DebugSession::State::checkThrowThunk(StopInfo info) {
    const std::int64_t thread = *info.threadId;
    send("stackTrace", rpc::Json::object().set("threadId", thread).set("startFrame", 0).set("levels", 1),
         [weak = weak(), info, thread](Result<rpc::Json> body) mutable {
             const auto self = weak.lock();
             if (!self) {
                 return;
             }
             std::optional<dap::StackFrame> top;
             if (body) {
                 if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr && stack->size() > 0) {
                     top = dap::parseStackFrame(stack->items().front());
                 }
             }
             if (!top || top->instructionPointerReference.empty()) {
                 self->announceStop(std::move(info));
                 return;
             }
             const std::string address = top->instructionPointerReference;
             self->send("disassemble", dap::disassembleArguments(address, 1, 0),
                        [weak = self->weak(), info = std::move(info), thread,
                         top = std::move(*top)](Result<rpc::Json> answer) mutable {
                            const auto state = weak.lock();
                            if (!state) {
                                return;
                            }
                            const auto instructions = answer ? dap::parseDisassembleResponse(*answer)
                                                             : std::vector<dap::DisassembledInstruction>{};
                            if (instructions.empty() || !isJump(instructions.front().instruction)) {
                                state->readThrownType(std::move(info), thread, top);
                                return;
                            }
                            // The import thunk: the runtime's location of the same breakpoint stops next.
                            state->resumeThrow(thread);
                        });
         });
}

// Continues a throw stop that is not reported (a thunk, a type the filter leaves out).
void DebugSession::State::resumeThrow(std::int64_t thread) {
    {
        LockGuard lock(mutex);
        if (state == DebugState::Ended || stopRequested) {
            return;
        }
    }
    send("continue", rpc::Json::object().set("threadId", thread), [](Result<rpc::Json>) {});
}

// --- MSVC thrown types (DBG-2b; msvc_throw.hpp for the structures) ---

/*
 * Reads the thrown type of a _CxxThrowException stop and reports or resumes it (header comment,
 * "Exception filters"): the ThrowInfo pointer is the runtime frame's pThrowInfo argument when the
 * runtime has symbols, else rdx (the stop is then at the function's first instruction, so the
 * argument register still holds it). Cached per ThrowInfo address for the session.
 */
void DebugSession::State::readThrownType(StopInfo info, std::int64_t thread, const dap::StackFrame& top) {
    auto read = std::make_shared<ThrowRead>(ThrowRead{.info = std::move(info), .thread = thread, .frameId = top.id});
    evaluateThrowInfo(read, hasSource(top) ? "pThrowInfo" : "$rdx");
}

void DebugSession::State::evaluateThrowInfo(const ThrowReadPtr& read, std::string expression) {
    rpc::Json arguments =
        rpc::Json::object().set("expression", expression).set("frameId", read->frameId).set("context", "watch");
    send("evaluate", std::move(arguments),
         [weak = weak(), read, expression = std::move(expression)](Result<rpc::Json> body) {
             const auto self = weak.lock();
             if (!self) {
                 return;
             }
             std::optional<std::uint64_t> address;
             if (body) {
                 address = msvc::parseAddress(body->stringAt("result"));
             }
             if (!address || *address == 0) {
                 if (expression != "$rdx") {
                     self->evaluateThrowInfo(read, "$rdx"); // The argument has no location here.
                     return;
                 }
                 self->throwTypeRead(read, fail(ErrorCode::NotFound, "the debugger did not show the ThrowInfo "
                                                                     "argument of _CxxThrowException"));
                 return;
             }
             read->throwInfo = *address;
             std::optional<std::pair<std::string, std::string>> known;
             {
                 LockGuard lock(self->mutex);
                 if (const auto found = self->thrownTypes.find(*address); found != self->thrownTypes.end()) {
                     known = found->second;
                 }
                 for (const auto& [base, size] : self->imageSizes) {
                     if (*address >= base && *address - base < size) {
                         read->base = base;
                         read->imageSize = size;
                     }
                 }
             }
             if (known) {
                 self->throwTypeRead(read, std::move(*known));
             } else if (read->base != 0) {
                 self->readThrowInfo(read);
             } else {
                 self->findThrowModule(read);
             }
         });
}

// The module holding the ThrowInfo: the highest module base below it, confirmed by its PE header.
void DebugSession::State::findThrowModule(const ThrowReadPtr& read) {
    send("modules", rpc::Json::object(), [weak = weak(), read](Result<rpc::Json> body) {
        const auto self = weak.lock();
        if (!self) {
            return;
        }
        if (!body) {
            self->throwTypeRead(read, std::unexpected(std::move(body.error())));
            return;
        }
        std::uint64_t best = 0;
        if (const rpc::Json* modules = body->find("modules"); modules != nullptr && modules->isArray()) {
            for (const rpc::Json& module : modules->items()) {
                const auto base = msvc::parseAddress(module.stringAt("addressRange"));
                if (base && *base <= read->throwInfo && *base > best) {
                    best = *base;
                }
            }
        }
        if (best == 0) {
            self->throwTypeRead(read, fail(ErrorCode::NotFound, "no loaded module holds the ThrowInfo"));
            return;
        }
        read->base = best;
        self->readBytes(best, msvc::kImageHeaderBytes, [weak = self->weak(), read](Result<std::string> bytes) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            if (!bytes) {
                state->throwTypeRead(read, std::unexpected(std::move(bytes.error())));
                return;
            }
            auto header = msvc::parseImageHeader(*bytes);
            if (!header) {
                state->throwTypeRead(read, std::unexpected(std::move(header.error())));
                return;
            }
            if (header->machine != msvc::kMachineAmd64) {
                state->throwTypeRead(read, fail(ErrorCode::Unsupported, "exception type filters need an x64 program"));
                return;
            }
            if (read->throwInfo - read->base >= header->sizeOfImage) {
                state->throwTypeRead(read, fail(ErrorCode::NotFound, "no loaded module holds the ThrowInfo"));
                return;
            }
            read->imageSize = header->sizeOfImage;
            {
                LockGuard lock(state->mutex);
                state->imageSizes[read->base] = header->sizeOfImage;
            }
            state->readThrowInfo(read);
        });
    });
}

// ThrowInfo -> CatchableTypeArray[0] -> CatchableType -> TypeDescriptor name.
void DebugSession::State::readThrowInfo(const ThrowReadPtr& read) {
    // An image-relative offset inside the module, or an error.
    const auto at = [read](std::optional<std::uint32_t> rva) -> Result<std::uint64_t> {
        if (!rva || *rva == 0 || *rva >= read->imageSize) {
            return fail(ErrorCode::Corrupt, "the ThrowInfo points outside its module");
        }
        return read->base + *rva;
    };
    readBytes(read->throwInfo, msvc::kThrowInfoSize, [weak = weak(), read, at](Result<std::string> info) {
        const auto self = weak.lock();
        if (!self) {
            return;
        }
        auto array = info ? at(msvc::readU32(*info, msvc::kThrowInfoCatchableTypeArray))
                          : Result<std::uint64_t>(std::unexpected(std::move(info.error())));
        if (!array) {
            self->throwTypeRead(read, std::unexpected(std::move(array.error())));
            return;
        }
        self->readBytes(*array, 8, [weak = self->weak(), read, at](Result<std::string> types) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            const auto count = types ? msvc::readU32(*types, 0) : std::nullopt;
            auto first = types && count && *count > 0
                             ? at(msvc::readU32(*types, 4))
                             : Result<std::uint64_t>(fail(ErrorCode::Corrupt, "the ThrowInfo lists no type"));
            if (!first) {
                state->throwTypeRead(read, std::unexpected(std::move(first.error())));
                return;
            }
            state->readBytes(*first, 8, [weak = state->weak(), read, at](Result<std::string> catchable) {
                const auto again = weak.lock();
                if (!again) {
                    return;
                }
                auto descriptor = catchable ? at(msvc::readU32(*catchable, msvc::kCatchableTypeTypeDescriptor))
                                            : Result<std::uint64_t>(std::unexpected(std::move(catchable.error())));
                if (!descriptor) {
                    again->throwTypeRead(read, std::unexpected(std::move(descriptor.error())));
                    return;
                }
                again->readBytes(*descriptor + msvc::kTypeDescriptorName64, msvc::kMaxTypeNameBytes,
                                 [weak = again->weak(), read](Result<std::string> name) {
                                     const auto last = weak.lock();
                                     if (!last) {
                                         return;
                                     }
                                     if (!name) {
                                         last->throwTypeRead(read, std::unexpected(std::move(name.error())));
                                         return;
                                     }
                                     std::string decorated(msvc::cString(*name));
                                     if (!decorated.starts_with('.')) {
                                         last->throwTypeRead(
                                             read, fail(ErrorCode::Corrupt, "the thrown type has no RTTI name"));
                                         return;
                                     }
                                     std::string undecorated = msvc::undecorateTypeName(decorated).value_or(decorated);
                                     std::pair names{std::move(decorated), std::move(undecorated)};
                                     {
                                         LockGuard lock(last->mutex);
                                         last->thrownTypes[read->throwInfo] = names;
                                     }
                                     last->throwTypeRead(read, std::move(names));
                                 });
            });
        });
    });
}

// readMemory of `count` bytes at `address`; an error when the adapter read none.
void DebugSession::State::readBytes(std::uint64_t address, std::uint64_t count,
                                    std::function<void(Result<std::string>)> then) {
    send("readMemory", dap::readMemoryArguments(std::format("0x{:X}", address), static_cast<std::int64_t>(count), 0),
         [then = std::move(then), address](Result<rpc::Json> body) {
             if (!body) {
                 then(std::unexpected(std::move(body.error())));
                 return;
             }
             dap::ReadMemoryResult result = dap::parseReadMemoryResponse(*body);
             if (result.decodedBytes.empty()) {
                 then(fail(ErrorCode::IoError, std::format("the debugger could not read memory at 0x{:X}", address)));
                 return;
             }
             then(std::move(result.decodedBytes));
         });
}

/*
 * The read's end: the type ({decorated, undecorated}) or why it could not be read. A type the
 * filter leaves out resumes the program; anything else is reported, an unread type with a
 * filter set with a note saying why the filter could not decide.
 */
void DebugSession::State::throwTypeRead(const ThrowReadPtr& read, Result<std::pair<std::string, std::string>> names) {
    std::vector<std::string> filter;
    {
        LockGuard lock(mutex);
        filter = throwTypes;
    }
    StopInfo info = std::move(read->info);
    if (names) {
        info.exceptionType = names->second;
        info.description = "C++ exception thrown: " + names->second;
        if (!msvc::throwTypeMatches(filter, names->second, names->first)) {
            resumeThrow(read->thread);
            return;
        }
    } else if (!filter.empty()) {
        const std::string note =
            std::format("The thrown type could not be read ({}), so the debugger stopped at this throw "
                        "although the exception type filter may not include it.\n",
                        names.error().message());
        emit([&] {
            if (callbacks.onOutput)
                callbacks.onOutput("important", note);
        });
    }
    announceStop(std::move(info));
}

void DebugSession::State::announceStop(StopInfo info) {
    std::uint64_t gen = 0;
    std::optional<std::string> restore;
    {
        LockGuard lock(mutex);
        if (state == DebugState::Ended) {
            return;
        }
        pauseRequested = false;
        stepInPhase = StepInPhase::None;
        thunkHops = 0;
        stepInOrigin.reset();
        pauseCandidates.clear();
        pauseOrigin = info.reason == "pause" ? info.threadId : std::nullopt;
        state = DebugState::Stopped;
        stop = info;
        gen = ++generation;
        frames.clear();
        loading.clear();
        currentFrame.reset();
        if (info.threadId) {
            currentThread = info.threadId;
        }
        if (runTo) {
            restore = runTo->first;
            runTo.reset();
        }
    }
    if (restore) {
        sendBreakpoints(*restore);
    }
    emit([&] {
        if (callbacks.onAdapterStopped)
            callbacks.onAdapterStopped();
        if (callbacks.onStateChanged) {
            callbacks.onStateChanged(DebugState::Stopped);
        }
        if (callbacks.onStopped) {
            callbacks.onStopped(info);
        }
    });
    requestThreads(gen, std::nullopt);
}

// Asks for the threads; then loads the selected thread's frames (and, with `then`, pauses it).
/*
 * A thread request (pause) before any thread is known: lldb-dap answers threads with an empty or
 * stale list while the program runs, and pauses the whole process whatever threadId says, so
 * the request goes out with thread 0 rather than not at all.
 */
void DebugSession::State::sendWithoutThread(const std::string& command) {
    send(command, rpc::Json::object().set("threadId", 0), [](Result<rpc::Json>) {});
}

void DebugSession::State::requestThreads(std::uint64_t gen, std::optional<std::string> then) {
    send("threads", rpc::Json(), [weak = weak(), gen, then = std::move(then)](Result<rpc::Json> body) {
        const auto self = weak.lock();
        if (!self) {
            return;
        }
        if (!body) {
            log::warn("debug", "threads failed: {}", body.error().message());
            if (then) {
                self->sendWithoutThread(*then);
            }
            return;
        }
        std::vector<DebugThread> list;
        if (const rpc::Json* threads = body->find("threads"); threads != nullptr) {
            for (const rpc::Json& thread : threads->items()) {
                const dap::Thread parsed = dap::parseThread(thread);
                list.push_back(DebugThread{parsed.id, parsed.name});
            }
        }
        std::optional<std::int64_t> selected;
        {
            LockGuard lock(self->mutex);
            if (gen != self->generation) {
                return;
            }
            self->threads = list;
            const std::optional<std::int64_t> current = self->currentThread;
            if (!current || std::ranges::none_of(list, [&](const DebugThread& t) { return t.id == *current; })) {
                self->currentThread = list.empty() ? std::nullopt : std::optional<std::int64_t>(list.front().id);
            }
            selected = self->currentThread;
            self->pauseCandidates.clear();
            if (self->pauseOrigin) {
                for (const DebugThread& thread : list) {
                    if (thread.id != *self->pauseOrigin && self->pauseCandidates.size() < kPauseThreadCandidates) {
                        self->pauseCandidates.push_back(thread.id);
                    }
                }
            }
        }
        self->emit([&] {
            if (self->callbacks.onThreads) {
                self->callbacks.onThreads(list);
            }
        });
        if (then && selected) {
            self->send(*then, rpc::Json::object().set("threadId", *selected), [](Result<rpc::Json>) {});
            return;
        }
        if (then) {
            self->sendWithoutThread(*then);
            return;
        }
        if (selected) {
            self->loadFrames(*selected, gen, true);
        }
    });
}

void DebugSession::State::loadFrames(std::int64_t threadId, std::uint64_t gen, bool select) {
    std::int64_t start = 0;
    {
        LockGuard lock(mutex);
        if (gen != generation || state != DebugState::Stopped || loading.contains(threadId)) {
            return;
        }
        const FrameList& list = frames[threadId];
        if (list.complete) {
            return;
        }
        start = static_cast<std::int64_t>(list.frames.size());
        loading.insert(threadId);
    }
    send("stackTrace", rpc::Json::object().set("threadId", threadId).set("startFrame", start).set("levels", kFramePage),
         [weak = weak(), threadId, gen, select](Result<rpc::Json> body) {
             if (const auto self = weak.lock()) {
                 self->framesAnswered(threadId, gen, select, std::move(body));
             }
         });
}

void DebugSession::State::framesAnswered(std::int64_t threadId, std::uint64_t gen, bool select,
                                         Result<rpc::Json> body) {
    FrameList snapshot;
    std::optional<DebugFrame> chosen;
    std::int64_t chosenThread = threadId;
    std::optional<std::int64_t> tryNext; // Header comment, "Pause".
    {
        LockGuard lock(mutex);
        loading.erase(threadId);
        if (gen != generation) {
            return;
        }
        FrameList& list = frames[threadId];
        if (!body) {
            list.complete = true;
            log::warn("debug", "stackTrace failed: {}", body.error().message());
        } else {
            std::size_t received = 0;
            if (const rpc::Json* stack = body->find("stackFrames"); stack != nullptr) {
                for (const rpc::Json& frame : stack->items()) {
                    list.frames.push_back(toFrame(dap::parseStackFrame(frame)));
                    ++received;
                }
            }
            list.totalFrames = body->integerAt("totalFrames");
            list.complete = received < static_cast<std::size_t>(kFramePage) ||
                            (list.totalFrames && static_cast<std::int64_t>(list.frames.size()) >= *list.totalFrames);
        }
        snapshot = list;
        if (select && !currentFrame && currentThread == threadId) {
            const bool pausedWithoutSource = stop && stop->reason == "pause" && pauseOrigin &&
                                             std::ranges::none_of(list.frames, [](const DebugFrame& frame) {
                                                 return frame.hasSource() && !frame.subtle;
                                             });
            if (pausedWithoutSource && !pauseCandidates.empty()) {
                tryNext = pauseCandidates.front();
                pauseCandidates.erase(pauseCandidates.begin());
                currentThread = tryNext;
            } else if (pausedWithoutSource && threadId != *pauseOrigin) {
                // No other thread has a source frame either: back to the thread that stopped.
                chosenThread = *pauseOrigin;
                currentThread = chosenThread;
                currentFrame = pickFrame(frames[chosenThread].frames, false);
                chosen = currentFrame;
            } else {
                // A pause lands in system code (a sleep, a wait) as often as not: show the program's frame.
                currentFrame = pickFrame(list.frames, stop && (stop->isCrash() || !stop->exceptionFilter.empty() ||
                                                               stop->reason == "pause"));
                chosen = currentFrame;
            }
        }
    }
    emit([&] {
        if (callbacks.onFrames) {
            callbacks.onFrames(threadId, snapshot);
        }
        if (chosen && callbacks.onFrameSelected) {
            callbacks.onFrameSelected(chosenThread, *chosen);
        }
    });
    if (tryNext) {
        loadFrames(*tryNext, gen, true);
    }
}

// --- Resuming ---

// Sends `command` for the current thread after switching to Running.
Result<void> DebugSession::State::resume(const std::string& command, rpc::Json extra,
                                         std::optional<std::uint64_t> expectedGeneration,
                                         std::optional<std::int64_t> expectedFrame) {
    if (readOnly)
        return fail(ErrorCode::Unsupported, "A dump session cannot execute a process");
    std::int64_t thread = 0;
    std::uint64_t commandGeneration = 0;
    std::optional<StopInfo> oldStop;
    std::map<std::int64_t, FrameList> oldFrames;
    std::optional<DebugFrame> oldFrame;
    {
        LockGuard lock(mutex);
        if (state != DebugState::Stopped) {
            return fail(ErrorCode::Busy, std::format("{} needs a paused program", command));
        }
        if (expectedGeneration && (generation != *expectedGeneration || !currentFrame ||
                                   currentFrame->id != expectedFrame))
            return fail(ErrorCode::Cancelled, "Step target expired");
        thread = currentThread.value_or(threads.empty() ? 1 : threads.front().id);
        oldStop = stop;
        oldFrames = frames;
        oldFrame = currentFrame;
        state = DebugState::Running;
        stepInPhase = command == "stepIn" ? StepInPhase::Stepping : StepInPhase::None;
        thunkHops = 0;
        stepInOrigin.reset();
        if (const auto top = frames.find(thread); top != frames.end() && !top->second.frames.empty()) {
            stepInOrigin.emplace(top->second.frames.front().name, top->second.frames.front().line);
        }
        commandGeneration = ++generation;
        stop.reset();
        frames.clear();
        loading.clear();
        currentFrame.reset();
    }
    emit([&] {
        if (callbacks.onStateChanged)
            callbacks.onStateChanged(DebugState::Running);
    });
    send(command, std::move(extra.set("threadId", thread)),
         [weak = weak(), command, commandGeneration, thread, oldStop = std::move(oldStop),
          oldFrames = std::move(oldFrames), oldFrame = std::move(oldFrame)](Result<rpc::Json> answer) mutable {
             const auto self = weak.lock();
             if (!self || answer)
                 return;
             bool restored = false;
             {
                 LockGuard lock(self->mutex);
                 if (!self->stopRequested && self->state == DebugState::Running &&
                     self->generation == commandGeneration) {
                     self->state = DebugState::Stopped;
                     ++self->generation;
                     self->stop = oldStop;
                     self->frames = oldFrames;
                     self->currentFrame = oldFrame;
                     restored = true;
                 }
             }
             if (!restored)
                 return;
             const std::string message = std::format("{} failed: {}\n", command, answer.error().message());
             self->emit([&] {
                 if (self->callbacks.onOutput)
                     self->callbacks.onOutput("important", message);
                 if (self->callbacks.onStateChanged)
                     self->callbacks.onStateChanged(DebugState::Stopped);
                 if (oldStop && self->callbacks.onStopped)
                     self->callbacks.onStopped(*oldStop);
                 if (const auto it = oldFrames.find(thread); it != oldFrames.end() && self->callbacks.onFrames)
                     self->callbacks.onFrames(thread, it->second);
                 if (oldFrame && self->callbacks.onFrameSelected)
                     self->callbacks.onFrameSelected(thread, *oldFrame);
             });
         });
    return {};
}

void DebugSession::State::continued() {
    bool changed = false;
    {
        LockGuard lock(mutex);
        if (state == DebugState::Running) {
            ++generation; // A continued event supersedes any outstanding resume reply.
        } else if (state != DebugState::Stopped) {
            return;
        } else {
            state = DebugState::Running;
            ++generation;
            stop.reset();
            frames.clear();
            loading.clear();
            currentFrame.reset();
            changed = true;
        }
    }
    emit([&] {
        if (changed && callbacks.onStateChanged) {
            callbacks.onStateChanged(DebugState::Running);
        }
        if (callbacks.onContinued)
            callbacks.onContinued();
    });
}

} // namespace frost::debug
