// SPDX-License-Identifier: Apache-2.0
//
// FakeDebugAdapter and scriptProgram (see frost/debug/fake_debug_adapter.hpp): the in-process
// transport, the request decoder and the scripted debuggee.

#include "frost/debug/fake_debug_adapter.hpp"

#include "frost/core/mutex.hpp"
#include "frost/rpc/framing.hpp"

#include <algorithm>
#include <condition_variable>
#include <format>

namespace frost::debug {

struct FakeDebugAdapter::State {
    mutable Mutex mutex;
    mutable std::condition_variable changed;
    std::string inbound FROST_GUARDED_BY(mutex); // Bytes the client has not read yet.
    bool eof FROST_GUARDED_BY(mutex) = false;
    std::optional<int> exitCode FROST_GUARDED_BY(mutex);
    bool closedByClient FROST_GUARDED_BY(mutex) = false;
    std::int64_t seq FROST_GUARDED_BY(mutex) = 1;
    std::map<std::string, Handler, std::less<>> handlers FROST_GUARDED_BY(mutex);
    std::vector<rpc::Json> received FROST_GUARDED_BY(mutex);
    std::vector<rpc::Json> reverseResponses FROST_GUARDED_BY(mutex);
    rpc::FrameDecoder decoder; // Only the client's writer thread touches it (through write()).

    // Queues `message` with the next seq.
    void queue(rpc::Json message) {
        {
            LockGuard lock(mutex);
            if (eof) {
                return;
            }
            message.set("seq", seq++);
            inbound += rpc::encodeFrame(message.dump());
        }
        changed.notify_all();
    }

    void end(std::optional<int> code) {
        {
            LockGuard lock(mutex);
            eof = true;
            if (!exitCode) {
                exitCode = code;
            }
        }
        changed.notify_all();
    }

    void handleRequest(const rpc::Json& message) {
        const std::string command(message.stringAt("command"));
        Handler handler;
        {
            LockGuard lock(mutex);
            if (const auto it = handlers.find(command); it != handlers.end()) {
                handler = it->second;
            }
        }
        const rpc::Json* arguments = message.find("arguments");
        const rpc::Json empty;
        Reply reply;
        if (handler) {
            reply = handler(arguments != nullptr ? *arguments : empty);
        } else if (command == "disconnect") {
            reply.exitAfter = true;
        }
        if (reply.respond) {
            rpc::Json response = rpc::Json::object()
                                     .set("type", "response")
                                     .set("request_seq", message.integerAt("seq").value_or(0))
                                     .set("command", command)
                                     .set("success", reply.success);
            if (!reply.message.empty()) {
                response.set("message", reply.message);
            }
            if (!reply.body.isNull()) {
                response.set("body", std::move(reply.body));
            }
            queue(std::move(response));
        }
        for (auto& [event, body] : reply.events) {
            rpc::Json frame = rpc::Json::object().set("type", "event").set("event", event);
            if (!body.isNull()) {
                frame.set("body", std::move(body));
            }
            queue(std::move(frame));
        }
        if (reply.exitAfter) {
            end(0);
        }
    }

    // The client's writer thread: decodes what it wrote and answers each request.
    void received_(std::string_view bytes) {
        decoder.feed(bytes);
        while (auto frame = decoder.next()) {
            if (!*frame) {
                continue;
            }
            auto parsed = rpc::parseJson(**frame);
            if (!parsed || !parsed->isObject()) {
                continue;
            }
            const std::string type(parsed->stringAt("type"));
            {
                LockGuard lock(mutex);
                if (type == "request") {
                    received.push_back(*parsed);
                } else if (type == "response") {
                    reverseResponses.push_back(*parsed);
                }
            }
            changed.notify_all();
            if (type == "request") {
                handleRequest(*parsed);
            }
        }
    }
};

namespace {

class FakeTransport final : public IDapTransport {
public:
    explicit FakeTransport(std::shared_ptr<FakeDebugAdapter::State> state) : m_state(std::move(state)) {}

    Result<std::size_t> read(std::span<char> buffer) override {
        UniqueLock lock(m_state->mutex);
        while (m_state->inbound.empty() && !m_state->eof) {
            m_state->changed.wait(lock.native());
        }
        if (m_state->inbound.empty()) {
            return std::size_t{0};
        }
        const std::size_t count = std::min(buffer.size(), m_state->inbound.size());
        std::copy_n(m_state->inbound.data(), count, buffer.data());
        m_state->inbound.erase(0, count);
        return count;
    }

    Result<void> write(std::string_view bytes) override {
        {
            LockGuard lock(m_state->mutex);
            if (m_state->eof) {
                return fail(ErrorCode::IoError, "the fake debug adapter has exited");
            }
        }
        m_state->received_(bytes);
        return {};
    }

    void close() noexcept override {
        {
            LockGuard lock(m_state->mutex);
            m_state->closedByClient = true;
            m_state->eof = true;
            m_state->inbound.clear();
        }
        m_state->changed.notify_all();
    }

    std::optional<int> exitCode() noexcept override {
        LockGuard lock(m_state->mutex);
        return m_state->exitCode;
    }

private:
    std::shared_ptr<FakeDebugAdapter::State> m_state;
};

} // namespace

FakeDebugAdapter::FakeDebugAdapter() : m_state(std::make_shared<State>()) {}

FakeDebugAdapter::~FakeDebugAdapter() { m_state->end(std::nullopt); }

std::unique_ptr<IDapTransport> FakeDebugAdapter::transport() { return std::make_unique<FakeTransport>(m_state); }

void FakeDebugAdapter::on(std::string command, Handler handler) {
    LockGuard lock(m_state->mutex);
    m_state->handlers[std::move(command)] = std::move(handler);
}

void FakeDebugAdapter::sendEvent(std::string event, rpc::Json body) {
    rpc::Json message = rpc::Json::object().set("type", "event").set("event", std::move(event));
    if (!body.isNull()) {
        message.set("body", std::move(body));
    }
    m_state->queue(std::move(message));
}

std::int64_t FakeDebugAdapter::sendReverseRequest(std::string command, rpc::Json arguments) {
    std::int64_t seq = 0;
    {
        LockGuard lock(m_state->mutex);
        seq = m_state->seq;
    }
    rpc::Json message = rpc::Json::object().set("type", "request").set("command", std::move(command));
    if (!arguments.isNull()) {
        message.set("arguments", std::move(arguments));
    }
    m_state->queue(std::move(message));
    return seq;
}

void FakeDebugAdapter::crash(int exitCode) { m_state->end(exitCode); }

void FakeDebugAdapter::sendRaw(const std::string& bytes) {
    {
        LockGuard lock(m_state->mutex);
        m_state->inbound += bytes;
    }
    m_state->changed.notify_all();
}

std::vector<rpc::Json> FakeDebugAdapter::requests() const {
    LockGuard lock(m_state->mutex);
    return m_state->received;
}

std::vector<std::string> FakeDebugAdapter::commands() const {
    LockGuard lock(m_state->mutex);
    std::vector<std::string> out;
    for (const rpc::Json& request : m_state->received) {
        out.emplace_back(request.stringAt("command"));
    }
    return out;
}

std::vector<rpc::Json> FakeDebugAdapter::argumentsOf(std::string_view command) const {
    LockGuard lock(m_state->mutex);
    std::vector<rpc::Json> out;
    for (const rpc::Json& request : m_state->received) {
        if (request.stringAt("command") == command) {
            const rpc::Json* arguments = request.find("arguments");
            out.push_back(arguments != nullptr ? *arguments : rpc::Json());
        }
    }
    return out;
}

std::vector<rpc::Json> FakeDebugAdapter::reverseResponses() const {
    LockGuard lock(m_state->mutex);
    return m_state->reverseResponses;
}

bool FakeDebugAdapter::closedByClient() const {
    LockGuard lock(m_state->mutex);
    return m_state->closedByClient;
}

bool FakeDebugAdapter::waitForRequest(std::string_view command, std::size_t count, IClock::Duration timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(m_state->mutex);
    for (;;) {
        std::size_t seen = 0;
        for (const rpc::Json& request : m_state->received) {
            seen += request.stringAt("command") == command ? 1 : 0;
        }
        if (seen >= count) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        m_state->changed.wait_until(lock.native(), deadline);
    }
}

// --- scriptProgram ---

namespace {

// The debuggee's run state, shared by the handlers.
struct ProgramState {
    Mutex mutex;
    FakeProgram program;
    std::vector<std::int64_t> bound FROST_GUARDED_BY(mutex); // Verified breakpoint lines, sorted.
    std::int64_t line FROST_GUARDED_BY(mutex) = 0;           // The current stop line.
    bool crashed FROST_GUARDED_BY(mutex) = false;
    std::int64_t nextBreakpointId FROST_GUARDED_BY(mutex) = 1;
};

rpc::Json stopped(std::string_view reason, std::string_view description = {}) {
    rpc::Json body = rpc::Json::object().set("reason", reason).set("threadId", 1).set("allThreadsStopped", true);
    if (!description.empty()) {
        body.set("description", description);
    }
    return body;
}

rpc::Json frame(std::int64_t id, std::string_view name, std::string_view path, std::int64_t line) {
    rpc::Json json = rpc::Json::object().set("id", id).set("name", name).set("line", line).set("column", 1);
    if (!path.empty()) {
        json.set("source", rpc::Json::object().set("name", "main.cpp").set("path", path));
    }
    return json;
}

rpc::Json variable(std::string_view name, std::string_view value, std::string_view type, std::int64_t reference = 0,
                   std::int64_t indexed = 0) {
    rpc::Json json = rpc::Json::object()
                         .set("name", name)
                         .set("value", value)
                         .set("type", type)
                         .set("evaluateName", name)
                         .set("variablesReference", reference);
    if (indexed > 0) {
        json.set("indexedVariables", indexed);
    }
    return json;
}

// The stop after configurationDone or continue from `from` (exclusive): the next bound line.
std::optional<std::int64_t> nextStop(const std::vector<std::int64_t>& bound, std::int64_t from) {
    const auto it = std::ranges::upper_bound(bound, from);
    return it == bound.end() ? std::nullopt : std::optional<std::int64_t>(*it);
}

} // namespace

void scriptProgram(FakeDebugAdapter& adapter, FakeProgram program) {
    auto state = std::make_shared<ProgramState>();
    state->program = std::move(program);
    using Reply = FakeDebugAdapter::Reply;

    adapter.on("initialize", [](const rpc::Json&) {
        return Reply{.body = rpc::Json::object()
                                 .set("supportsConfigurationDoneRequest", true)
                                 .set("supportsCancelRequest", true)
                                 .set("supportsTerminateRequest", true)
                                 .set("supportsEvaluateForHovers", true)};
    });
    adapter.on("launch", [](const rpc::Json&) { return Reply{.events = {{"initialized", rpc::Json()}}}; });
    adapter.on("setBreakpoints", [state](const rpc::Json& arguments) {
        LockGuard lock(state->mutex);
        const rpc::Json* source = arguments.find("source");
        const bool ourFile = source != nullptr && source->stringAt("path") == state->program.sourcePath;
        rpc::Json answers = rpc::Json::array();
        if (ourFile) {
            state->bound.clear();
        }
        if (const rpc::Json* breakpoints = arguments.find("breakpoints"); breakpoints != nullptr) {
            for (const rpc::Json& breakpoint : breakpoints->items()) {
                const std::int64_t line = breakpoint.integerAt("line").value_or(0);
                const bool verified =
                    ourFile && std::ranges::find(state->program.codeLines, line) != state->program.codeLines.end();
                rpc::Json answer = rpc::Json::object().set("id", state->nextBreakpointId++).set("verified", verified);
                if (verified) {
                    answer.set("line", line);
                    state->bound.push_back(line);
                } else {
                    answer.set("message", "No code is generated for this line").set("reason", "failed");
                }
                answers.push(std::move(answer));
            }
        }
        std::ranges::sort(state->bound);
        return Reply{.body = rpc::Json::object().set("breakpoints", std::move(answers))};
    });
    adapter.on("configurationDone", [state](const rpc::Json&) {
        LockGuard lock(state->mutex);
        Reply reply;
        if (!state->program.output.empty()) {
            reply.events.emplace_back(
                "output", rpc::Json::object().set("category", "stdout").set("output", state->program.output));
        }
        if (state->program.crashReason) {
            state->line = state->program.entryLine;
            state->crashed = true;
            reply.events.emplace_back("stopped", stopped("exception", *state->program.crashReason));
        } else if (const auto first = nextStop(state->bound, 0)) {
            state->line = *first;
            reply.events.emplace_back("stopped", stopped("breakpoint"));
        } else if (!state->program.runsForever) {
            reply.events.emplace_back("exited", rpc::Json::object().set("exitCode", state->program.exitCode));
            reply.events.emplace_back("terminated", rpc::Json());
        }
        return reply;
    });
    adapter.on("threads", [](const rpc::Json&) {
        return Reply{.body = rpc::Json::object().set(
                         "threads", rpc::Json::array({rpc::Json::object().set("id", 1).set("name", "main"),
                                                      rpc::Json::object().set("id", 2).set("name", "worker")}))};
    });
    adapter.on("stackTrace", [state](const rpc::Json& arguments) {
        LockGuard lock(state->mutex);
        const std::int64_t thread = arguments.integerAt("threadId").value_or(0);
        const std::string& path = state->program.sourcePath;
        std::vector<rpc::Json> all;
        if (thread == 2) {
            all.push_back(frame(2000, "worker_loop()", path, 3));
        } else if (state->crashed) {
            rpc::Json top = frame(1000, "abort", "", 0);
            top.set("moduleId", "ucrtbase.dll").set("presentationHint", "subtle");
            all.push_back(std::move(top));
            all.push_back(frame(1001, "crash()", path, state->line));
            all.push_back(frame(1002, "main()", path, state->line + 10));
        } else {
            for (std::int64_t level = 0; level < state->program.frameCount; ++level) {
                const bool last = level + 1 == state->program.frameCount && level > 0;
                all.push_back(frame(1000 + level, level == 0 ? "main()" : std::format("caller{}()", level),
                                    last ? "" : path, level == 0 ? state->line : 100 + level));
            }
        }
        const auto start = static_cast<std::size_t>(arguments.integerAt("startFrame").value_or(0));
        const std::int64_t levels = arguments.integerAt("levels").value_or(0);
        rpc::Json frames = rpc::Json::array();
        for (std::size_t i = start; i < all.size() && (levels <= 0 || i < start + static_cast<std::size_t>(levels));
             ++i) {
            frames.push(all[i]);
        }
        return Reply{.body = rpc::Json::object()
                                 .set("stackFrames", std::move(frames))
                                 .set("totalFrames", static_cast<std::int64_t>(all.size()))};
    });
    adapter.on("scopes", [](const rpc::Json&) {
        return Reply{.body =
                         rpc::Json::object().set("scopes", rpc::Json::array({rpc::Json::object()
                                                                                 .set("name", "Locals")
                                                                                 .set("presentationHint", "locals")
                                                                                 .set("variablesReference", 100)
                                                                                 .set("expensive", false),
                                                                             rpc::Json::object()
                                                                                 .set("name", "Registers")
                                                                                 .set("presentationHint", "registers")
                                                                                 .set("variablesReference", 900)
                                                                                 .set("expensive", true)}))};
    });
    adapter.on("variables", [state](const rpc::Json& arguments) {
        const std::int64_t reference = arguments.integerAt("variablesReference").value_or(0);
        rpc::Json list = rpc::Json::array();
        if (reference == 100) {
            std::int64_t length = 0;
            {
                LockGuard lock(state->mutex);
                length = state->program.arrayLength;
            }
            list.push(variable("x", "42", "int"));
            list.push(variable("p", "{x:1, y:2}", "Point", 101));
            list.push(variable("values", std::format("int[{}]", length), std::format("int[{}]", length), 102, length));
        } else if (reference == 101) {
            list.push(variable("x", "1", "int"));
            list.push(variable("y", "2", "int"));
        } else if (reference == 102) {
            std::int64_t length = 0;
            {
                LockGuard lock(state->mutex);
                length = state->program.arrayLength;
            }
            const std::int64_t start = arguments.integerAt("start").value_or(0);
            const std::int64_t count = arguments.integerAt("count").value_or(length);
            for (std::int64_t i = start; i < length && i < start + count; ++i) {
                list.push(variable(std::format("[{}]", i), std::to_string(i * 10), "int"));
            }
        } else if (reference == 900) {
            list.push(variable("rip", "0x00007ff6a0001000", "unsigned long"));
        }
        return Reply{.body = rpc::Json::object().set("variables", std::move(list))};
    });
    adapter.on("evaluate", [](const rpc::Json& arguments) {
        const std::string_view expression = arguments.stringAt("expression");
        if (expression == "x") {
            return Reply{.body =
                             rpc::Json::object().set("result", "42").set("type", "int").set("variablesReference", 0)};
        }
        if (expression == "x + 1") {
            return Reply{.body =
                             rpc::Json::object().set("result", "43").set("type", "int").set("variablesReference", 0)};
        }
        if (expression == "p") {
            return Reply{.body = rpc::Json::object()
                                     .set("result", "{x:1, y:2}")
                                     .set("type", "Point")
                                     .set("variablesReference", 101)};
        }
        return Reply{.success = false,
                     .body = rpc::Json::object().set(
                         "error", rpc::Json::object()
                                      .set("id", 1)
                                      .set("format", "use of undeclared identifier '{name}'")
                                      .set("variables", rpc::Json::object().set("name", expression))),
                     .message = "evaluate failed"};
    });
    const auto step = [state](const rpc::Json&) {
        LockGuard lock(state->mutex);
        ++state->line;
        return Reply{.events = {{"stopped", stopped("step")}}};
    };
    adapter.on("next", step);
    adapter.on("stepIn", step);
    adapter.on("stepOut", step);
    adapter.on("pause", [](const rpc::Json&) { return Reply{.events = {{"stopped", stopped("pause")}}}; });
    adapter.on("continue", [state](const rpc::Json&) {
        LockGuard lock(state->mutex);
        Reply reply{.body = rpc::Json::object().set("allThreadsContinued", true)};
        state->crashed = false;
        if (const auto next = nextStop(state->bound, state->line)) {
            state->line = *next;
            reply.events.emplace_back("stopped", stopped("breakpoint"));
        } else if (!state->program.runsForever) {
            reply.events.emplace_back("exited", rpc::Json::object().set("exitCode", state->program.exitCode));
            reply.events.emplace_back("terminated", rpc::Json());
        }
        return reply;
    });
    adapter.on("disconnect", [](const rpc::Json&) { return Reply{.exitAfter = true}; });
}

} // namespace frost::debug
