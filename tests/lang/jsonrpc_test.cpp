// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the LSP base protocol (jsonrpc.hpp): FrameDecoder on split, joined, malformed
// and oversized input, and JsonRpcConnection against a scripted in-memory server: results,
// out-of-order responses, malformed messages, notifications, server requests, cancellation with
// $/cancelRequest, the callback flavour and a server that dies mid-request.

#include "frost/lang/jsonrpc.hpp"

#include "lang/fake_server.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::Result;
using frost::lang::ConnectionHandlers;
using frost::lang::encodeFrame;
using frost::lang::FrameDecoder;
using frost::lang::Json;
using frost::lang::JsonRpcConnection;
using frost::sched::Lane;
using frost::test::eventually;
using frost::test::FakeServer;
using frost::test::LangEnv;
using frost::test::MemoryPipe;

std::vector<Result<std::string>> drain(FrameDecoder& decoder) {
    std::vector<Result<std::string>> out;
    while (auto next = decoder.next()) {
        out.push_back(std::move(*next));
    }
    return out;
}

TEST(FrameDecoder, DecodesFramesSplitAndJoined) {
    const std::string stream =
        encodeFrame(R"({"a":1})") + encodeFrame("{}") + "Content-Type: x\r\n" + "content-length: 2\r\n\r\n[]";
    FrameDecoder decoder;
    std::vector<std::string> payloads;
    for (char c : stream) { // One byte at a time.
        decoder.feed(std::string_view(&c, 1));
        for (auto& frame : drain(decoder)) {
            ASSERT_TRUE(frame.has_value()) << frame.error().describe();
            payloads.push_back(*frame);
        }
    }
    EXPECT_EQ(payloads, (std::vector<std::string>{R"({"a":1})", "{}", "[]"}));
    EXPECT_EQ(decoder.buffered(), 0U);
}

TEST(FrameDecoder, ResynchronisesAfterMalformedHeaders) {
    FrameDecoder decoder;
    decoder.feed("garbage line\r\n\r\n" + encodeFrame("1") + "Content-Length: x\r\n\r\n" + encodeFrame("2") +
                 "X-Other: 3\r\n\r\n" + encodeFrame("3"));
    const auto frames = drain(decoder);
    std::vector<std::string> good;
    int errors = 0;
    for (const auto& frame : frames) {
        if (frame) {
            good.push_back(*frame);
        } else {
            EXPECT_EQ(frame.error().code(), ErrorCode::Corrupt);
            ++errors;
        }
    }
    EXPECT_EQ(good, (std::vector<std::string>{"1", "2", "3"}));
    EXPECT_EQ(errors, 3);
}

TEST(FrameDecoder, SkipsOversizedFrames) {
    FrameDecoder decoder(8);
    decoder.feed("Content-Length: 20\r\n\r\n0123456789");
    auto first = drain(decoder);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_FALSE(first[0].has_value());
    decoder.feed("0123456789" + encodeFrame("ok"));
    auto second = drain(decoder);
    ASSERT_EQ(second.size(), 1U);
    ASSERT_TRUE(second[0].has_value());
    EXPECT_EQ(*second[0], "ok");
}

// What a connection's handlers saw. Shared with the handlers, which may still run while the
// connection's reader finishes after the test's objects are gone.
struct Seen {
    frost::Mutex mutex;
    std::vector<std::pair<std::string, Json>> notifications FROST_GUARDED_BY(mutex);
    std::atomic<int> protocolErrors{0};
    std::atomic<int> closedCalls{0};
};

// The connection plus what its handlers saw.
struct Client {
    explicit Client(LangEnv& env, MemoryPipe& pipe) : seen(std::make_shared<Seen>()) {
        ConnectionHandlers handlers;
        handlers.onNotification = [seen = seen](std::string_view method, const Json& params) {
            frost::LockGuard lock(seen->mutex);
            seen->notifications.emplace_back(std::string(method), params);
        };
        handlers.onRequest = [](std::string_view method,
                                const Json& params) -> std::expected<Json, frost::lang::ResponseError> {
            if (method == "client/echo") {
                return params;
            }
            return std::unexpected(frost::lang::ResponseError{
                .code = frost::lang::rpc_error::kMethodNotFound, .message = "no", .data = {}});
        };
        handlers.onProtocolError = [seen = seen](const frost::Error&) { ++seen->protocolErrors; };
        handlers.onClosed = [seen = seen](const frost::Error&) { ++seen->closedCalls; };
        connection = std::make_unique<JsonRpcConnection>(pipe.clientStream(), env.pool(), std::move(handlers));
    }

    std::shared_ptr<Seen> seen;
    std::unique_ptr<JsonRpcConnection> connection;
};

// A script that answers every request with {"echo": params}.
bool echo(FakeServer& server, const Json& message) {
    if (const Json* id = message.find("id"); id != nullptr && message.find("method") != nullptr) {
        server.respond(*id, Json::object().set("echo", message.find("params") ? *message.find("params") : Json()));
    }
    return true;
}

TEST(JsonRpcConnection, RequestCompletesWithItsResult) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, echo);
    Client client(env, pipe);
    auto result = env.run(client.connection->request("test/echo", Json::object().set("x", 1)));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->find("echo")->integerAt("x"), 1);
    const auto received = server.received();
    ASSERT_EQ(received.size(), 1U);
    EXPECT_EQ(received[0].stringAt("jsonrpc"), "2.0");
    EXPECT_EQ(received[0].integerAt("id"), 1);
    EXPECT_EQ(received[0].stringAt("method"), "test/echo");
    EXPECT_EQ(client.connection->pendingRequests(), 0U);
}

TEST(JsonRpcConnection, DeferredServerRequestCanBeAnsweredAfterReaderReturns) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        if (message.stringAt("method") == "test/trigger") {
            s.requestClient(77, "client/deferred", Json::object());
            s.respond(*message.find("id"), Json::object());
        }
        return true;
    });

    struct Deferred {
        std::mutex mutex;
        std::function<void(std::expected<Json, frost::lang::ResponseError>)> reply;
    };
    auto deferred = std::make_shared<Deferred>();
    ConnectionHandlers handlers;
    handlers.onAsyncRequest = [deferred](std::string_view method, const Json&,
                                         std::function<void(std::expected<Json, frost::lang::ResponseError>)> done) {
        if (method != "client/deferred") {
            return false;
        }
        std::lock_guard lock(deferred->mutex);
        deferred->reply = std::move(done);
        return true;
    };
    JsonRpcConnection connection(pipe.clientStream(), env.pool(), std::move(handlers));
    auto result = env.run(connection.request("test/trigger", Json::object()));
    ASSERT_TRUE(result.has_value()) << result.error().describe();

    auto received = server.received();
    EXPECT_FALSE(std::ranges::any_of(received, [](const Json& message) {
        return message.find("id") != nullptr && message.find("id")->asInteger() == 77;
    }));
    std::function<void(std::expected<Json, frost::lang::ResponseError>)> reply;
    {
        std::lock_guard lock(deferred->mutex);
        reply = std::move(deferred->reply);
    }
    ASSERT_TRUE(reply);
    reply(Json::object().set("applied", true));
    ASSERT_TRUE(eventually([&] {
        const auto messages = server.received();
        return std::ranges::any_of(messages, [](const Json& message) {
            return message.find("id") != nullptr && message.find("id")->asInteger() == 77 &&
                   message.find("result") != nullptr && message.find("result")->boolAt("applied").value_or(false);
        });
    }));
}

TEST(JsonRpcConnection, OutOfOrderResponsesReachTheirRequests) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    std::vector<Json> ids;
    FakeServer server(pipe, [&ids](FakeServer& s, const Json& message) {
        ids.push_back(*message.find("id"));
        if (ids.size() == 3) { // Answer 3, 1, 2.
            for (std::size_t index : {2U, 0U, 1U}) {
                s.respond(ids[index], Json(ids[index].asInteger().value() * 100));
            }
        }
        return true;
    });
    Client client(env, pipe);
    auto a = env.pool().spawn(Lane::UserVisible, client.connection->request("m", Json()));
    auto b = env.pool().spawn(Lane::UserVisible, client.connection->request("m", Json()));
    auto c = env.pool().spawn(Lane::UserVisible, client.connection->request("m", Json()));
    std::vector<std::int64_t> results;
    for (auto* handle : {&a, &b, &c}) {
        auto value = handle->join();
        ASSERT_TRUE(value.has_value()) << value.error().describe();
        results.push_back(value->asInteger().value());
    }
    // Ids follow send order, which the spawns do not fix; each result must be its own id * 100.
    std::sort(results.begin(), results.end());
    EXPECT_EQ(results, (std::vector<std::int64_t>{100, 200, 300}));
    EXPECT_EQ(client.seen->protocolErrors.load(), 0);
}

TEST(JsonRpcConnection, MalformedMessagesAreReportedAndSkipped) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        s.sendRaw("Content-Lenght: 5\r\n\r\nhello"); // Misspelt header.
        s.sendRaw(encodeFrame("{not json"));
        s.sendRaw(encodeFrame("42"));
        s.sendRaw(encodeFrame(R"({"jsonrpc":"2.0","id":999,"result":1})")); // Unknown id.
        s.sendRaw(encodeFrame(R"({"jsonrpc":"2.0"})"));                     // Neither kind.
        s.respond(*message.find("id"), Json("fine"));
        return true;
    });
    Client client(env, pipe);
    auto result = env.run(client.connection->request("m", Json()));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(*result->asString(), "fine");
    EXPECT_EQ(client.seen->protocolErrors.load(), 5);
    EXPECT_FALSE(client.connection->closed());
}

TEST(JsonRpcConnection, ServerErrorsBecomeFrostErrors) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        s.send(Json::object()
                   .set("jsonrpc", "2.0")
                   .set("id", *message.find("id"))
                   .set("error",
                        Json::object().set("code", frost::lang::rpc_error::kContentModified).set("message", "stale")));
        return true;
    });
    Client client(env, pipe);
    auto result = env.run(client.connection->request("textDocument/completion", Json()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    EXPECT_NE(result.error().message().find("-32801"), std::string::npos);
}

TEST(JsonRpcConnection, NotificationsAndServerRequestsAreHandled) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        if (message.stringAt("method") == "start") {
            s.notifyClient("server/hello", Json::object().set("n", 1));
            s.requestClient(41, "client/echo", Json::object().set("v", "x"));
            s.requestClient(42, "client/unknown", Json());
        }
        return true;
    });
    Client client(env, pipe);
    ASSERT_TRUE(client.connection->notify("start", Json()).has_value());
    ASSERT_TRUE(server.waitForMessages(3)); // start + two responses.
    const auto received = server.received();
    EXPECT_EQ(received[1].integerAt("id"), 41);
    EXPECT_EQ(received[1].find("result")->stringAt("v"), "x");
    EXPECT_EQ(received[2].integerAt("id"), 42);
    EXPECT_EQ(received[2].find("error")->integerAt("code"), frost::lang::rpc_error::kMethodNotFound);
    ASSERT_TRUE(eventually([&] {
        frost::LockGuard lock(client.seen->mutex);
        return client.seen->notifications.size() == 1;
    }));
    frost::LockGuard lock(client.seen->mutex);
    EXPECT_EQ(client.seen->notifications[0].first, "server/hello");
    EXPECT_EQ(client.seen->notifications[0].second.integerAt("n"), 1);
}

TEST(JsonRpcConnection, CancellationSendsCancelRequestAndDropsTheLateAnswer) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json& message) {
        if (message.stringAt("method") == "$/cancelRequest") {
            // The late answer to the cancelled request, then an answer to the next one.
            s.respond(*message.find("params")->find("id"), Json("late"));
        } else if (message.stringAt("method") == "second") {
            s.respond(*message.find("id"), Json("second"));
        }
        return true; // "slow" is never answered by itself.
    });
    Client client(env, pipe);
    std::stop_source stop;
    auto slow = env.pool().spawn(Lane::UserVisible, client.connection->request("slow", Json(), stop.get_token()));
    ASSERT_TRUE(server.waitForMessages(1));
    stop.request_stop();
    auto cancelled = slow.join();
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    auto second = env.run(client.connection->request("second", Json()));
    ASSERT_TRUE(second.has_value()) << second.error().describe();
    const auto received = server.received();
    ASSERT_GE(received.size(), 2U);
    EXPECT_EQ(received[1].stringAt("method"), "$/cancelRequest");
    EXPECT_EQ(received[1].find("params")->integerAt("id"), received[0].integerAt("id"));
    EXPECT_EQ(client.seen->protocolErrors.load(), 0); // The late answer was expected, not an error.
}

TEST(JsonRpcConnection, CallbackRequestsRunTheirCallbackOnce) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, echo);
    Client client(env, pipe);
    std::atomic<int> calls{0};
    std::atomic<bool> ok{false};
    const std::int64_t id = client.connection->request("cb", Json(5), [&](Result<Json> result) {
        ok = result.has_value() && result->find("echo")->asInteger() == 5;
        ++calls;
    });
    EXPECT_EQ(id, 1);
    ASSERT_TRUE(eventually([&] { return calls.load() == 1; }));
    EXPECT_TRUE(ok.load());
}

TEST(JsonRpcConnection, ServerDeathFailsPendingRequests) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, [](FakeServer& s, const Json&) {
        s.closeOutput(); // Crash mid-request: no answer, EOF.
        return false;
    });
    Client client(env, pipe);
    auto result = env.run(client.connection->request("doomed", Json()));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    ASSERT_TRUE(eventually([&] { return client.seen->closedCalls.load() == 1; }));
    EXPECT_TRUE(client.connection->closed());
    // Later sends fail at once.
    EXPECT_FALSE(client.connection->notify("after", Json()).has_value());
    auto after = env.run(client.connection->request("after", Json()));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code(), ErrorCode::IoError);
}

TEST(JsonRpcConnection, CloseFailsPendingRequestsAndClosesTheStream) {
    LangEnv env;
    MemoryPipe pipe(env.pool());
    FakeServer server(pipe, nullptr); // Never answers.
    Client client(env, pipe);
    auto pending = env.pool().spawn(Lane::UserVisible, client.connection->request("never", Json()));
    ASSERT_TRUE(server.waitForMessages(1));
    client.connection->close();
    auto result = pending.join();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::IoError);
    EXPECT_TRUE(eventually([&] { return pipe.clientClosedWrite(); }));
    EXPECT_TRUE(eventually([&] { return client.seen->closedCalls.load() == 1; }));
}

} // namespace
