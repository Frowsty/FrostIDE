// SPDX-License-Identifier: Apache-2.0
//
// Tests for the DAP client (frost/debug/dap_client.hpp) over the fake-dap double: Content-Length
// framing (split and joined frames, a malformed header), requests and their responses and errors,
// events, reverse requests (declined and answered), cancellation and an adapter crash.

#include "frost/debug/dap_client.hpp"
#include "frost/debug/dap_types.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <string>
#include <vector>

namespace frost::debug {
namespace {

using test::waitUntil;

// Records what the client's handlers saw.
struct Handlers {
    Mutex mutex;
    std::vector<std::string> events FROST_GUARDED_BY(mutex);
    std::vector<std::string> problems FROST_GUARDED_BY(mutex);
    std::optional<Error> closed FROST_GUARDED_BY(mutex);

    DapHandlers make(std::function<Result<rpc::Json>(std::string_view, const rpc::Json&)> reverse = {}) {
        return {.onEvent =
                    [this](std::string_view event, const rpc::Json& body) {
                        LockGuard lock(mutex);
                        events.push_back(std::string(event) + " " + body.dump());
                    },
                .onReverseRequest = std::move(reverse),
                .onProtocolError =
                    [this](const Error& error) {
                        LockGuard lock(mutex);
                        problems.push_back(error.message());
                    },
                .onClosed =
                    [this](const Error& why) {
                        LockGuard lock(mutex);
                        closed = why;
                    }};
    }
    std::vector<std::string> eventList() {
        LockGuard lock(mutex);
        return events;
    }
    std::size_t problemCount() {
        LockGuard lock(mutex);
        return problems.size();
    }
    std::optional<Error> closedWith() {
        LockGuard lock(mutex);
        return closed;
    }
};

// Sends `command` and waits for its completion.
Result<rpc::Json> call(IDebugAdapterClient& client, std::string command, rpc::Json arguments = {}) {
    std::promise<Result<rpc::Json>> promise;
    auto future = promise.get_future();
    client.request(std::move(command), std::move(arguments),
                   [&promise](Result<rpc::Json> body) { promise.set_value(std::move(body)); });
    if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        return fail(ErrorCode::Timeout, "no answer");
    }
    return future.get();
}

TEST(DapFraming, SplitAndJoinedFramesDecode) {
    const std::string one = rpc::encodeFrame(R"({"seq":1,"type":"event","event":"a"})");
    const std::string two = rpc::encodeFrame(R"({"seq":2,"type":"event","event":"b"})");
    EXPECT_TRUE(one.starts_with("Content-Length: 36\r\n\r\n")) << one;
    rpc::FrameDecoder decoder;
    const std::string both = one + two;
    decoder.feed(both.substr(0, 10));
    EXPECT_FALSE(decoder.next().has_value()) << "a partial header is not a frame";
    decoder.feed(both.substr(10));
    auto first = decoder.next();
    ASSERT_TRUE(first && *first);
    EXPECT_EQ(**first, R"({"seq":1,"type":"event","event":"a"})");
    auto second = decoder.next();
    ASSERT_TRUE(second && *second);
    EXPECT_EQ(**second, R"({"seq":2,"type":"event","event":"b"})");
    EXPECT_FALSE(decoder.next().has_value());
}

TEST(DapClient, RequestsGetTheirResponsesAndErrorsReadLikeTheAdapterWrote) {
    FakeDebugAdapter adapter;
    adapter.on("threads", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{
            .body = rpc::Json::object().set("threads", rpc::Json::array({rpc::Json::object().set("id", 1)}))};
    });
    adapter.on("evaluate", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{
            .success = false,
            .body = rpc::Json::object().set("error", rpc::Json::object()
                                                         .set("id", 7)
                                                         .set("format", "no symbol '{name}' here")
                                                         .set("variables", rpc::Json::object().set("name", "y"))),
            .message = "evaluate failed"};
    });
    adapter.on("next",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "notStopped"}; });
    Handlers handlers;
    auto client = makeDapClient(adapter.transport(), handlers.make());

    auto threads = call(*client, "threads");
    ASSERT_TRUE(threads.has_value()) << threads.error().describe();
    ASSERT_NE(threads->find("threads"), nullptr);
    EXPECT_EQ(threads->find("threads")->size(), 1U);

    auto failed = call(*client, "evaluate", rpc::Json::object().set("expression", "y"));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::IoError);
    EXPECT_EQ(failed.error().message(), "evaluate: no symbol 'y' here");
    auto busy = call(*client, "next");
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error().code(), ErrorCode::Busy) << "notStopped maps to Busy";

    // The requests went out in order with increasing seq numbers.
    const auto requests = adapter.requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[0].stringAt("type"), "request");
    EXPECT_LT(requests[0].integerAt("seq").value_or(0), requests[1].integerAt("seq").value_or(0));
    EXPECT_EQ(adapter.argumentsOf("evaluate").front().stringAt("expression"), "y");
    EXPECT_EQ(client->pendingRequests(), 0U);
}

TEST(DapClient, EventsAndRawFramesArriveInOrderAndMalformedHeadersAreReported) {
    FakeDebugAdapter adapter;
    Handlers handlers;
    auto client = makeDapClient(adapter.transport(), handlers.make());
    adapter.sendEvent("output", rpc::Json::object().set("output", "hi"));
    // Two frames in one write, then a frame without Content-Length, then a good one.
    adapter.sendRaw(rpc::encodeFrame(R"({"seq":90,"type":"event","event":"one"})") +
                    rpc::encodeFrame(R"({"seq":91,"type":"event","event":"two"})"));
    adapter.sendRaw("Bogus: 1\r\n\r\n{}");
    adapter.sendEvent("three");
    ASSERT_TRUE(waitUntil([&] { return handlers.eventList().size() == 4; }));
    const auto events = handlers.eventList();
    EXPECT_EQ(events[0], R"(output {"output":"hi"})");
    EXPECT_EQ(events[1], "one null");
    EXPECT_EQ(events[2], "two null");
    EXPECT_EQ(events[3], "three null");
    EXPECT_GE(handlers.problemCount(), 1U) << "the malformed header is reported";
}

TEST(DapClient, ReverseRequestsAreDeclinedOrAnswered) {
    {
        FakeDebugAdapter adapter;
        Handlers handlers;
        auto client = makeDapClient(adapter.transport(), handlers.make());
        const std::int64_t seq =
            adapter.sendReverseRequest("runInTerminal", rpc::Json::object().set("args", rpc::Json::array()));
        ASSERT_TRUE(waitUntil([&] { return adapter.reverseResponses().size() == 1; }));
        const rpc::Json answer = adapter.reverseResponses().front();
        EXPECT_EQ(answer.integerAt("request_seq"), seq);
        EXPECT_EQ(answer.stringAt("command"), "runInTerminal");
        EXPECT_EQ(answer.boolAt("success"), false);
        EXPECT_NE(answer.stringAt("message").find("not supported"), std::string_view::npos);
    }
    {
        FakeDebugAdapter adapter;
        Handlers handlers;
        auto client = makeDapClient(adapter.transport(),
                                    handlers.make([](std::string_view command, const rpc::Json&) -> Result<rpc::Json> {
                                        EXPECT_EQ(command, "startDebugging");
                                        return rpc::Json::object().set("ok", true);
                                    }));
        static_cast<void>(adapter.sendReverseRequest("startDebugging"));
        ASSERT_TRUE(waitUntil([&] { return adapter.reverseResponses().size() == 1; }));
        const rpc::Json answer = adapter.reverseResponses().front();
        EXPECT_EQ(answer.boolAt("success"), true);
        ASSERT_NE(answer.find("body"), nullptr);
        EXPECT_EQ(answer.find("body")->boolAt("ok"), true);
    }
}

TEST(DapClient, CancelCompletesWithCancelledAndTellsTheAdapter) {
    FakeDebugAdapter adapter;
    adapter.on("evaluate", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    Handlers handlers;
    auto client = makeDapClient(adapter.transport(), handlers.make());
    client->setSupportsCancel(true);
    std::promise<Result<rpc::Json>> promise;
    auto future = promise.get_future();
    const std::int64_t seq = client->request("evaluate", rpc::Json::object().set("expression", "slow"),
                                             [&](Result<rpc::Json> body) { promise.set_value(std::move(body)); });
    ASSERT_GT(seq, 0);
    ASSERT_TRUE(adapter.waitForRequest("evaluate"));
    client->cancel(seq);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    ASSERT_TRUE(adapter.waitForRequest("cancel"));
    EXPECT_EQ(adapter.argumentsOf("cancel").front().integerAt("requestId"), seq);
    // The cancel request's own answer arrives; the cancelled one is never waited for again.
    EXPECT_TRUE(waitUntil([&] { return client->pendingRequests() == 0; }));
}

TEST(DapClient, AnAdapterCrashFailsPendingRequestsAndClosesTheConnection) {
    FakeDebugAdapter adapter;
    adapter.on("continue", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    Handlers handlers;
    auto client = makeDapClient(adapter.transport(), handlers.make());
    std::promise<Result<rpc::Json>> promise;
    auto future = promise.get_future();
    client->request("continue", rpc::Json::object(),
                    [&](Result<rpc::Json> body) { promise.set_value(std::move(body)); });
    ASSERT_TRUE(adapter.waitForRequest("continue"));
    adapter.crash(3);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto result = future.get();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    ASSERT_TRUE(waitUntil([&] { return handlers.closedWith().has_value(); }));
    EXPECT_NE(handlers.closedWith()->message().find("exited with code 3"), std::string::npos)
        << handlers.closedWith()->message();
    EXPECT_TRUE(client->closed());
    // Requests after the end fail at once.
    auto late = call(*client, "threads");
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error().code(), ErrorCode::IoError);
}

TEST(DapTypes, ParsesTheShapesTheSessionReads) {
    const rpc::Json frame = rpc::Json::object()
                                .set("id", 5)
                                .set("name", "main()")
                                .set("line", 12)
                                .set("column", 3)
                                .set("source", rpc::Json::object().set("name", "main.cpp").set("path", "/w/main.cpp"));
    const dap::StackFrame parsed = dap::parseStackFrame(frame);
    EXPECT_EQ(parsed.id, 5);
    EXPECT_EQ(parsed.line, 12);
    ASSERT_TRUE(parsed.source.has_value());
    EXPECT_EQ(parsed.source->path, "/w/main.cpp");
    const dap::StoppedEvent stopped = dap::parseStoppedEvent(
        rpc::Json::object().set("reason", "exception").set("description", "Access violation").set("threadId", 4));
    EXPECT_EQ(stopped.reason, "exception");
    EXPECT_EQ(stopped.threadId, 4);
    const rpc::Json init = dap::initializeArguments();
    EXPECT_EQ(init.stringAt("adapterID"), "lldb-dap");
    EXPECT_EQ(init.boolAt("linesStartAt1"), true);
    EXPECT_EQ(init.boolAt("supportsRunInTerminalRequest"), false) << "FrostIDE declines runInTerminal";
}

} // namespace
} // namespace frost::debug
