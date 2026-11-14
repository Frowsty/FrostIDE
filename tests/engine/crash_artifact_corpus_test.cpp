// SPDX-License-Identifier: Apache-2.0
// Real contained CrashHandler/automation harness evidence, not a full FrostEditor window-crash journey.
#include "frost/engine/crash_artifact.hpp"
#include "frost/io/loopback_stream.hpp"

#include "support/real_engine_session.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <thread>

#include <sodium.h>

namespace frost::engine {
namespace {
using namespace std::chrono_literals;
using rpc::Json;
std::string mac(std::string_view key, std::string_view text) {
    crypto_auth_hmacsha256_state state{};
    std::array<unsigned char, 32> hash{};
    std::array<char, 65> output{};
    crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char*>(key.data()), key.size());
    crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    crypto_auth_hmacsha256_final(&state, hash.data());
    sodium_bin2hex(output.data(), output.size(), hash.data(), hash.size());
    return output.data();
}
std::string proof(const Json& challenge, std::string_view token, std::string_view role) {
    const std::string text =
        "FrostEngine.Session.v2\nrole=" + std::string(role) +
        "\ninstanceId=" + std::string(challenge.stringAt("instanceId")) +
        "\ngeneration=" + std::string(challenge.stringAt("generation")) +
        "\nprojectPath=" + std::string(challenge.stringAt("projectPath")) +
        "\nclientNonce=" + std::string(challenge.stringAt("clientNonce")) +
        "\nserverNonce=" + std::string(challenge.stringAt("serverNonce")) +
        "\ncapabilities=session.v2,auth.hmac-sha256,events.v1,heartbeat.v1\nheartbeatIntervalMs=" +
        std::to_string(challenge.integerAt("heartbeatIntervalMs").value()) +
        "\nheartbeatTimeoutMs=" + std::to_string(challenge.integerAt("heartbeatTimeoutMs").value()) + "\n";
    return mac(token, text);
}
class Peer {
public:
    std::unique_ptr<io::LoopbackStream> stream;
    std::string buffered;
    std::uint64_t next = 1;
    Result<Json> call(std::string_view method, Json params = Json::object(), const Json* identity = nullptr) {
        auto request = Json::object()
                           .set("jsonrpc", "2.0")
                           .set("id", next++)
                           .set("method", method)
                           .set("params", std::move(params));
        if (identity)
            request.set("session", *identity);
        auto sent = stream->write(request.dump() + "\n");
        if (!sent)
            return std::unexpected(sent.error());
        std::array<char, 8192> bytes{};
        for (;;) {
            const auto end = buffered.find('\n');
            if (end != std::string::npos) {
                auto message = rpc::parseJson(std::string_view(buffered).substr(0, end));
                buffered.erase(0, end + 1);
                if (!message)
                    return std::unexpected(message.error());
                if (message->stringAt("method") == "engine.heartbeat")
                    continue;
                if (const auto* result = message->find("result"))
                    return *result;
                return fail(ErrorCode::Corrupt, "real helper refused RPC");
            }
            auto read = stream->read(bytes);
            if (!read)
                return std::unexpected(read.error());
            if (!*read)
                return fail(ErrorCode::IoError, "real helper closed");
            buffered.append(bytes.data(), *read);
            if (buffered.size() > 16 * 1024 * 1024)
                return fail(ErrorCode::OutOfRange, "helper frame too large");
        }
    }
};
TEST(CrashArtifactCorpus, AuthenticatedOwnedCrashProducesARealCorrelatedDumpAndPrivateCacheSnapshot) {
    test::RealEngineBuild build;
    auto prepared = build.prepare({"FrostEditor", "FrostTestCrashArtifactChild"});
    ASSERT_TRUE(prepared) << prepared.error().describe();
    test::ScratchDir scratch("real-engine-crash-artifact");
    ASSERT_TRUE(build.copyProject(scratch.join("project")));
    auto& fs = io::realFileSystem();
    const auto helper = io::joinPath(io::parentPath(build.program), "FrostTestCrashArtifactChild.exe");
    ASSERT_TRUE(helper);
    ASSERT_TRUE(fs.exists(*helper).value_or(false));
    ASSERT_GE(sodium_init(), 0);
    std::array<unsigned char, 32> random{};
    std::array<char, 65> encoded{};
    randombytes_buf(random.data(), random.size());
    sodium_bin2hex(encoded.data(), encoded.size(), random.data(), random.size());
    const std::string token(encoded.data());
    io::ProcessSpec specification;
    specification.executable = *helper;
    specification.arguments = {"--project", scratch.join("project"),   "--directory", scratch.join("crashes"),
                               "--ready",   scratch.join("ready.json")};
    specification.environment.emplace_back("FROST_AUTOMATION_TOKEN", token);
    test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    specification.desktop = desktop.name();
    auto process = io::realProcessLauncher().launch(specification);
    ASSERT_TRUE(process) << process.error().describe();
    const auto readyDeadline = std::chrono::steady_clock::now() + 60s;
    while (!fs.exists(scratch.join("ready.json")).value_or(false) && std::chrono::steady_clock::now() < readyDeadline)
        std::this_thread::sleep_for(10ms);
    auto readyText = fs.readFile(scratch.join("ready.json"));
    ASSERT_TRUE(readyText);
    auto ready = rpc::parseJson(*readyText);
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->unsignedAt("processId"), (*process)->pid());
    auto connected = io::LoopbackStream::connect(static_cast<std::uint16_t>(ready->unsignedAt("port").value()),
                                                 std::chrono::steady_clock::now() + 30s);
    ASSERT_TRUE(connected);
    Peer peer;
    peer.stream = std::move(*connected);
    auto challenge = peer.call("auth.challenge", Json::object()
                                                     .set("contractVersion", 2)
                                                     .set("projectPath", ready->stringAt("projectPath"))
                                                     .set("clientNonce", std::string(64, 'e')));
    ASSERT_TRUE(challenge);
    ASSERT_EQ(challenge->stringAt("instanceId"), ready->stringAt("instanceId"));
    ASSERT_EQ(challenge->stringAt("generation"), ready->stringAt("generation"));
    ASSERT_EQ(challenge->stringAt("serverProof"), proof(*challenge, token, "server"));
    ASSERT_TRUE(peer.call("auth.prove", Json::object().set("proof", proof(*challenge, token, "client"))));
    const auto identity = Json::object()
                              .set("instanceId", challenge->stringAt("instanceId"))
                              .set("generation", challenge->stringAt("generation"))
                              .set("projectPath", challenge->stringAt("projectPath"));
    auto info = peer.call("server.info", Json::object(), &identity);
    ASSERT_TRUE(info);
    const auto* rawSource = info->find("crashArtifactSource");
    ASSERT_NE(rawSource, nullptr);
    auto source =
        parseCrashArtifactSource(*rawSource, ready->stringAt("instanceId"), ready->stringAt("generation"),
                                 ready->stringAt("projectPath"), static_cast<std::int64_t>((*process)->pid()));
    ASSERT_TRUE(source);
    auto observer = CrashArtifactObserver::capture(fs, *source, systemClock());
    ASSERT_TRUE(observer);
    auto trigger = Json::object()
                       .set("jsonrpc", "2.0")
                       .set("id", 99)
                       .set("method", "artifact.test-crash")
                       .set("params", Json::object())
                       .set("session", identity);
    ASSERT_TRUE(peer.stream->write(trigger.dump() + "\n"));
    auto crashed = (*process)->communicate({}, 120s);
    ASSERT_TRUE(crashed) << crashed.error().describe();
    ASSERT_NE(crashed->exitCode, 0);
    auto scan = (*observer)->poll(fs);
    ASSERT_TRUE(scan);
    ASSERT_TRUE(scan->artifact) << "actual handler dump unavailable or invalid";
    EXPECT_EQ(scan->artifact->source(), *source);
    EXPECT_GE(scan->artifact->crashUnixMs(), source->processStartUnixMs);
    EXPECT_TRUE(validateCrashMinidump(scan->artifact->dumpBytes(), source->processId));
    ASSERT_TRUE(fs.createDirectories(scratch.join("cache")));
    auto cacheDir = fs.canonicalPath(scratch.join("cache"));
    ASSERT_TRUE(cacheDir);
    auto copy = scan->artifact->materialize(fs, *cacheDir);
    ASSERT_TRUE(copy);
    EXPECT_EQ(fs.readFile(*copy).value_or(""), scan->artifact->dumpBytes());
}
} // namespace
} // namespace frost::engine
