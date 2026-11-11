// SPDX-License-Identifier: Apache-2.0
//
// FakeDebugAdapter: the fake-dap double (PLAN.md M4 and section 6.2; ADR 0050), a scripted debug
// adapter inside the test process. It hands out an IDapTransport whose bytes are real DAP frames, so
// the client's framing, seq matching, events and reverse requests run exactly as against lldb-dap.
/*
 * Scripts. on(command, handler) says how the adapter answers a request: a Reply with success or
 * failure, a body, and events to send right after the response (the order lldb-dap uses, such as
 * "initialized" after the launch response or "stopped" after a step's). Unscripted requests succeed
 * with no body; "disconnect" also ends the adapter (EOF, exit code 0) unless it is scripted. Handlers
 * run on the client's writer thread while the client writes the request; they may call
 * sendEvent() and the queries below.
 *
 * Pushing. sendEvent() and sendReverseRequest() queue messages at any time; crash() ends the stream
 * as an adapter crash would (the client reads EOF and gets the exit code); sendRaw() queues bytes as
 * they are (malformed frames).
 *
 * scriptProgram() installs a whole debuggee: threads, a paged stack, scopes, nested and paged
 * variables, evaluate, the stepping requests and the exit, which most session tests share.
 *
 * Thread safety: every member may be called from any thread. The adapter must outlive the transport
 * it handed out's use (the transport shares its state, so destroying the adapter first is safe too).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/debug/dap_client.hpp"
#include "frost/rpc/json.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::debug {

class FakeDebugAdapter {
public:
    // How the adapter answers one request.
    struct Reply {
        bool success = true;
        rpc::Json body{};
        std::string message{};                                   // A failed response's message.
        std::vector<std::pair<std::string, rpc::Json>> events{}; // Sent right after the response.
        bool respond = true;                                     // false: never answered (cancel tests).
        bool exitAfter = false;                                  // The adapter exits after the events.
    };
    using Handler = std::function<Reply(const rpc::Json& arguments)>;

    FakeDebugAdapter();
    ~FakeDebugAdapter();
    FakeDebugAdapter(const FakeDebugAdapter&) = delete ("one adapter per transport");
    FakeDebugAdapter& operator=(const FakeDebugAdapter&) = delete ("one adapter per transport");

    // The transport the client speaks over. Call once.
    [[nodiscard]] std::unique_ptr<IDapTransport> transport();

    // Scripts `command` (the last call for a command wins).
    void on(std::string command, Handler handler);
    // Queues an event now.
    void sendEvent(std::string event, rpc::Json body = {});
    // Queues a reverse request; the client's answer appears in reverseResponses(). Returns its seq.
    std::int64_t sendReverseRequest(std::string command, rpc::Json arguments = {});
    // Ends the stream as a crash would: the client reads EOF, and the transport reports `exitCode`.
    void crash(int exitCode = 3);
    // Queues raw bytes.
    void sendRaw(const std::string& bytes);

    // Every request the client sent (the whole message), in order.
    [[nodiscard]] std::vector<rpc::Json> requests() const;
    // Their commands, in order.
    [[nodiscard]] std::vector<std::string> commands() const;
    // The arguments of each request named `command`, in order.
    [[nodiscard]] std::vector<rpc::Json> argumentsOf(std::string_view command) const;
    // The client's responses to reverse requests, in order.
    [[nodiscard]] std::vector<rpc::Json> reverseResponses() const;
    // Whether the client closed the transport.
    [[nodiscard]] bool closedByClient() const;
    // Waits until `command` has been requested at least `count` times; false after `timeout`.
    [[nodiscard]] bool waitForRequest(std::string_view command, std::size_t count = 1,
                                      IClock::Duration timeout = std::chrono::seconds(10)) const;

    struct State; // Shared with the transport; src/debug/src/fake_debug_adapter.cpp.

private:
    std::shared_ptr<State> m_state;
};

// A scripted debuggee for scriptProgram (header comment).
struct FakeProgram {
    std::string sourcePath{};                         // The program's one source file (absolute).
    std::vector<std::int64_t> codeLines{};            // Lines a breakpoint binds to; others are unverified.
    std::int64_t entryLine = 1;                       // Where the first stop is when no breakpoint binds.
    std::int64_t frameCount = 3;                      // Frames of thread 1 (main), paged by stackTrace.
    std::int64_t arrayLength = 250;                   // Elements of the paged array variable `values`.
    int exitCode = 0;                                 // The exited event's code after continue.
    std::string output = "hello from the debuggee\n"; // A stdout output event after configurationDone.
    std::optional<std::string> crashReason{};         // Set: the first stop is this exception instead.
    bool runsForever = false;                         // continue past the last breakpoint keeps it running.
};

/*
 * Scripts `adapter` as `program`: initialize (configurationDone, cancel and terminate supported);
 * launch, then the initialized event; setBreakpoints verifying codeLines; configurationDone, then the
 * output and a stop (breakpoint at the first verified line, else the crash reason as an exception,
 * else exit); threads 1 "main" and 2 "worker"; stackTrace (frame ids 1000 + level, frame 0 at the
 * stop line of sourcePath, the others in "caller" functions, the last one without a source);
 * scopes (Locals = 100, Registers = 900); variables 100: x = 42 (int), p (Point, 101) and values
 * (int[N], 102, indexed); 101: x = 1, y = 2; 102: paged by start and count; evaluate: x -> 42,
 * x + 1 -> 43, p -> {x:1, y:2} with reference 101, anything else fails; next, stepIn and stepOut stop
 * with reason "step" one line further; pause stops with "pause"; continue exits with exitCode then
 * terminated (or, with runsForever, leaves it running until pause). A crash stop's stack starts
 * with a frame without a source ("abort" in ucrtbase.dll), so the faulting frame with a source is 1.
 */
void scriptProgram(FakeDebugAdapter& adapter, FakeProgram program);

} // namespace frost::debug
