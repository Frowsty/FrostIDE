// SPDX-License-Identifier: Apache-2.0
// Private native DAP logging and final worker admission over explicitly scripted protocol doubles.
#include "frost/core/log.hpp"
#include "frost/debug/dap_client.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>

namespace frost::debug {
namespace {
using namespace std::chrono_literals;
class CapturedLogs final : public log::ISink {
public:
    CapturedLogs() : previous(log::setSink(this)), oldLevel(log::level()) { log::setLevel(log::Level::Trace); }
    ~CapturedLogs() {
        log::setSink(previous);
        log::setLevel(oldLevel);
    }
    void write(log::Level, std::string_view, std::string_view message) noexcept override {
        std::lock_guard lock(mutex);
        text.append(message);
        text += '\n';
    }
    void flush() noexcept override {}
    std::string snapshot() {
        std::lock_guard lock(mutex);
        return text;
    }

private:
    log::ISink* previous;
    log::Level oldLevel;
    std::mutex mutex;
    std::string text;
};
Result<rpc::Json> invoke(IDebugAdapterClient& client, std::string command) {
    auto promise = std::make_shared<std::promise<Result<rpc::Json>>>();
    auto result = promise->get_future();
    client.request(std::move(command), rpc::Json::object(),
                   [promise](auto value) { promise->set_value(std::move(value)); });
    if (result.wait_for(3s) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "scripted DAP did not reply");
    return result.get();
}
TEST(DapPrivate, FinalWriteAdmissionRefusesZeroFramesAndReportsOnlySafePrivateError) {
    FakeDebugAdapter adapter;
    std::atomic<bool> allowed{false};
    DapHandlers handlers;
    handlers.privateSession = true;
    handlers.beforeWrite = [&](std::string_view command) -> Result<void> {
        if (command == "launch" && !allowed.load())
            return fail(ErrorCode::Untrusted, "private-environment-secret");
        return {};
    };
    auto client = makeDapClient(adapter.transport(), std::move(handlers));
    auto refused = invoke(*client, "launch");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(refused.error().message().find("private-environment-secret"), std::string::npos);
    EXPECT_TRUE(adapter.requests().empty());
    ASSERT_TRUE(client->sentMessageCount());
    EXPECT_EQ(*client->sentMessageCount(), 0U);
    EXPECT_EQ(client->pendingRequests(), 0U);
    allowed = true;
    EXPECT_TRUE(invoke(*client, "launch"));
    ASSERT_TRUE(test::waitUntil([&] { return client->sentMessageCount() == 1; }, 1s));
    EXPECT_EQ(adapter.argumentsOf("launch").size(), 1U);
    client.reset();
}
TEST(DapPrivate, ConcurrentCancellationAndFinalAdmissionRefusalCompleteExactlyOnce) {
    FakeDebugAdapter adapter;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    DapHandlers handlers;
    handlers.beforeWrite = [&](std::string_view command) -> Result<void> {
        if (command != "launch")
            return {};
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return fail(ErrorCode::Untrusted, "revoked");
    };
    auto client = makeDapClient(adapter.transport(), std::move(handlers));
    struct ReleaseWriter {
        std::mutex& mutex;
        std::condition_variable& changed;
        bool& released;
        ~ReleaseWriter() {
            {
                std::lock_guard lock(mutex);
                released = true;
            }
            changed.notify_all();
        }
    } releaseOnFailure{mutex, changed, release};
    std::atomic<unsigned> completions{0};
    const auto seq = client->request("launch", rpc::Json::object(), [&](auto result) {
        EXPECT_FALSE(result);
        ++completions;
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 3s, [&] { return entered; }));
    }
    client->cancel(seq);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(invoke(*client, "threads")); // Serial writer has passed the refused launch.
    EXPECT_EQ(completions.load(), 1U);
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    ASSERT_TRUE(test::waitUntil([&] { return client->sentMessageCount() == 1; }, 1s));
    client.reset();
}
TEST(DapPrivate, PeerControlledProtocolAndResponseErrorsAreSanitizedBeforeLogAndCallbacks) {
    CapturedLogs logs;
    constexpr std::string_view secret = "never-log-private-endpoint-credential";
    FakeDebugAdapter adapter;
    adapter.on("launch", [](const auto&) {
        return FakeDebugAdapter::Reply{
            .success = false,
            .body = rpc::Json::object().set("error",
                                            rpc::Json::object().set("format", "never-log-private-endpoint-credential")),
            .message = "never-log-private-endpoint-credential"};
    });
    std::mutex mutex;
    std::vector<std::string> problems;
    std::optional<std::string> closed;
    DapHandlers handlers;
    handlers.privateSession = true;
    handlers.onProtocolError = [&](const Error& error) {
        std::lock_guard lock(mutex);
        problems.push_back(error.describe());
    };
    handlers.onClosed = [&](const Error& error) {
        std::lock_guard lock(mutex);
        closed = error.describe();
    };
    auto client = makeDapClient(adapter.transport(), std::move(handlers));
    auto failed = invoke(*client, "launch");
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().describe().find(secret), std::string::npos);
    adapter.sendRaw(rpc::encodeFrame(rpc::Json::object().set("type", secret).dump()));
    ASSERT_TRUE(test::waitUntil(
        [&] {
            std::lock_guard lock(mutex);
            return !problems.empty();
        },
        1s));
    adapter.crash();
    ASSERT_TRUE(test::waitUntil(
        [&] {
            std::lock_guard lock(mutex);
            return closed.has_value();
        },
        1s));
    client.reset(); // Join all log/callback writers before inspecting/restoring the global sink.
    for (const auto& problem : problems)
        EXPECT_EQ(problem.find(secret), std::string::npos);
    EXPECT_EQ(closed->find(secret), std::string::npos);
    EXPECT_EQ(logs.snapshot().find(secret), std::string::npos);
}
TEST(DapPrivate, PrivateAdapterStderrIsChunkedOnlyToPrivateSinkOrDiscarded) {
    CapturedLogs logs;
    const std::string secret(10000, 's'); // Deliberately no newline: private mode must not accumulate lines.
    for (const bool sink : {true, false}) {
        io::ScriptedProcessLauncher launcher;
        launcher.expect(io::ScriptedProcess{.stderrBytes = secret});
        io::ProcessSpec spec;
        spec.executable = test::workPath("private-adapter.exe");
        spec.stdinMode = io::Stdio::Pipe;
        auto process = launcher.launch(spec);
        ASSERT_TRUE(process);
        std::atomic<std::size_t> received{0}, largest{0};
        DapProcessOutput output;
        output.privateOutput = true;
        if (sink)
            output.onStderrChunk = [&](std::string_view bytes) {
                received += bytes.size();
                largest.store(std::max(largest.load(), bytes.size()));
            };
        auto transport = processTransport(std::move(*process), std::move(output));
        if (sink)
            ASSERT_TRUE(test::waitUntil([&] { return received.load() == secret.size(); }, 1s));
        transport.reset(); // Joins drain, including the discard path.
        if (sink)
            EXPECT_LE(largest.load(), 4096U);
        else
            EXPECT_EQ(received.load(), 0U);
    }
    EXPECT_EQ(logs.snapshot().find(std::string(64, 's')), std::string::npos);
}
} // namespace
} // namespace frost::debug
