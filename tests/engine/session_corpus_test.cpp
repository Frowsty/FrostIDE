// SPDX-License-Identifier: Apache-2.0
// R6 mandatory real-engine acceptance: v2 authentication, push-only transitions and liveness.
#include "frost/engine/project_content.hpp"

#include "support/real_engine_session.hpp"
#include <gtest/gtest.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace frost::engine {
namespace {
using namespace std::chrono_literals;
using rpc::Json;

class SessionObservations {
public:
    EngineSessionCallbacks callbacks() {
        EngineSessionCallbacks callbacks;
        callbacks.connected = [this](const EngineIdentity& value) {
            std::lock_guard lock(mutex);
            identity = value;
            changed.notify_all();
        };
        callbacks.state = [this](const EngineState& value) {
            std::lock_guard lock(mutex);
            EngineState observation;
            observation.identity.instanceId = value.identity.instanceId;
            observation.identity.generation = value.identity.generation;
            observation.state = value.state;
            observation.mode = value.mode;
            observation.playGeneration = value.playGeneration;
            observation.operationId = value.operationId;
            observation.phase = value.phase;
            observation.playing = value.playing;
            observation.paused = value.paused;
            observation.frame = value.frame;
            observation.sequence = value.sequence;
            states.push_back(std::move(observation));
            changed.notify_all();
        };
        callbacks.disconnected = [this](const EngineDisconnect& value) {
            std::lock_guard lock(mutex);
            disconnects.push_back(value);
            changed.notify_all();
        };
        return callbacks;
    }

    bool wait(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 20s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, predicate);
    }

    Result<Json> command(EngineSession& session, std::string name, Json params = Json::object()) {
        struct Completion {
            std::mutex mutex;
            std::condition_variable changed;
            std::optional<Result<Json>> reply;
        };
        auto completion = std::make_shared<Completion>();
        auto request = session.request(std::move(name), std::move(params), [completion](Result<Json> reply) {
            std::lock_guard lock(completion->mutex);
            completion->reply = std::move(reply);
            completion->changed.notify_all();
        });
        if (!request)
            return std::unexpected(request.error());
        std::unique_lock lock(completion->mutex);
        if (!completion->changed.wait_for(lock, 120s, [&] { return completion->reply.has_value(); })) {
            session.cancel(*request);
            return fail(ErrorCode::Timeout, "real engine acceptance command did not reply within 120 seconds");
        }
        return std::move(*completion->reply);
    }

    std::size_t cursor() {
        std::lock_guard lock(mutex);
        return states.size();
    }

    // Replies are an independent oracle for actual command execution. Only a later state callback
    // can satisfy this predicate; no play.state request, refresh timer or cached UI state is used.
    bool pushedState(std::size_t after, const Json& oracle) {
        return wait([&] {
            return std::ranges::any_of(states.begin() + static_cast<std::ptrdiff_t>(after), states.end(),
                                       [&](const EngineState& pushed) {
                                           return pushed.state == oracle.stringAt("state") &&
                                                  pushed.paused == oracle.boolAt("paused").value_or(false) &&
                                                  pushed.operationId == oracle.stringAt("operationId") &&
                                                  pushed.phase == oracle.stringAt("phase") &&
                                                  pushed.playGeneration == oracle.stringAt("playGeneration") &&
                                                  pushed.identity.instanceId == identity->instanceId &&
                                                  pushed.identity.generation == identity->generation;
                                       });
        });
    }

    Result<EngineState> committedPlay(std::size_t after, const Json& receipt) {
        std::optional<EngineState> committed;
        const bool observed = wait([&] {
            for (std::size_t i = after; i < states.size(); ++i) {
                const auto& pushed = states[i];
                if (pushed.operationId == receipt.stringAt("operationId") && pushed.phase == "playing" &&
                    pushed.state == "Play" && pushed.mode == "Play" && pushed.playing &&
                    !pushed.playGeneration.empty() && pushed.identity.instanceId == identity->instanceId &&
                    pushed.identity.generation == identity->generation) {
                    committed = pushed;
                    return true;
                }
            }
            return false;
        });
        if (!observed)
            return fail(ErrorCode::Timeout, "accepted Play operation did not publish its actual committed generation");
        return std::move(*committed);
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::optional<EngineIdentity> identity;
    std::vector<EngineState> states;
    std::vector<EngineDisconnect> disconnects;
};

EngineSessionOptions optionsFor(const EngineEndpoint& endpoint) {
    EngineSessionOptions options;
    options.endpoint = endpoint;
    options.trustRevision = 1;
    options.trusted = [project = endpoint.projectPath](std::string_view requested, std::uint64_t revision) {
        return project == requested && revision == 1;
    };
    // Transport acceptance uses this explicit test admission boundary; production policy is driven
    // through the Workbench in engine_session_acceptance_test.cpp and j4_engine_session.py.
    options.admitCommand = [](std::string_view, const Json&, const EngineIdentity&) -> Result<void> { return {}; };
    return options;
}

class RealSessionCorpus : public testing::Test {
protected:
    void SetUp() override {
        auto prepared = build.prepare();
        ASSERT_TRUE(prepared) << prepared.error().describe();
        project = scratch.join("project");
        auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
    }
    test::RealEngineBuild build;
    test::ScratchDir scratch{"session-corpus"};
    std::string project;
};

TEST_F(RealSessionCorpus, AuthenticationRejectsUnpairedWrongHmacReflectionAndReplayAgainstRealEditor) {
    test::RealEditor editor;
    auto endpoint = editor.start(build, project, scratch.join("config"));
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    // Python's standard hashlib/hmac is the independent cryptographic oracle. The launcher sends
    // credentials over stdin, never argv, and reports only verdicts and negotiated durations.
    io::ProcessSpec spec;
    spec.executable = io::normalizePath(FROST_TEST_PYTHON).value_or("");
    spec.arguments = {"-B", FROST_ENGINE_SESSION_ORACLE, "--probe-auth"};
    spec.stdinMode = io::Stdio::Pipe;
    spec.workingDirectory = scratch.path();
    auto input = Json::object()
                     .set("port", endpoint->port)
                     .set("projectPath", endpoint->projectPath)
                     .set("instanceId", endpoint->instanceId)
                     .set("generation", endpoint->generation)
                     .set("secret", endpoint->secret);
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, input.dump(), 120s);
    ASSERT_TRUE(result) << result.error().describe();
    auto verdict = rpc::parseJson(result->stdoutBytes);
    ASSERT_TRUE(verdict) << "authentication oracle did not emit a JSON verdict";
    EXPECT_EQ(result->exitCode, 0) << verdict->dump();
    EXPECT_EQ(verdict->boolAt("ok"), true) << verdict->dump();
}

TEST_F(RealSessionCorpus, TwentyPauseResumeCyclesAndStepAreAcknowledgedByPushThenKillDisconnects) {
    test::RealEditor editor;
    auto endpoint = editor.start(build, project, scratch.join("config"));
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    SessionObservations seen;
    auto session = EngineSession::create(optionsFor(*endpoint), seen.callbacks());
    ASSERT_TRUE(session) << session.error().describe();
    ASSERT_TRUE(seen.wait([&] { return seen.identity.has_value(); }));
    ASSERT_TRUE(seen.wait([&] { return !seen.states.empty(); }));
    auto after = seen.cursor();
    const auto requestedAt = std::chrono::steady_clock::now();
    auto receipt = seen.command(**session, "play.start");
    ASSERT_TRUE(receipt) << receipt.error().describe();
    EXPECT_LT(std::chrono::steady_clock::now() - requestedAt, 5s);
    ASSERT_EQ(receipt->boolAt("accepted"), true);
    ASSERT_EQ(receipt->boolAt("pending"), true);
    ASSERT_EQ(receipt->boolAt("startedNow"), false);
    ASSERT_EQ(receipt->stringAt("state"), "Edit");
    ASSERT_EQ(receipt->stringAt("phase"), "pending");
    ASSERT_TRUE(receipt->stringAt("playGeneration").empty());
    ASSERT_EQ(receipt->stringAt("operationId").size(), 32U);
    auto playing = seen.committedPlay(after, *receipt);
    ASSERT_TRUE(playing) << playing.error().describe();
    for (int cycle = 0; cycle < 20; ++cycle) {
        SCOPED_TRACE(cycle);
        for (bool pause : {true, false}) {
            after = seen.cursor();
            auto reply = seen.command(**session, "play.pause", Json::object().set("paused", pause));
            ASSERT_TRUE(reply) << reply.error().describe();
            ASSERT_EQ(reply->boolAt("paused"), pause);
            ASSERT_TRUE(seen.pushedState(after, *reply));
        }
    }
    auto paused = seen.command(**session, "play.pause", Json::object().set("paused", true));
    ASSERT_TRUE(paused) << paused.error().describe();
    const auto beforeFrame = paused->unsignedAt("frames");
    ASSERT_TRUE(beforeFrame);
    after = seen.cursor();
    auto stepped = seen.command(**session, "play.step", Json::object().set("frames", 1));
    ASSERT_TRUE(stepped) << stepped.error().describe();
    ASSERT_TRUE(seen.wait([&] {
        return std::ranges::any_of(
            seen.states.begin() + static_cast<std::ptrdiff_t>(after), seen.states.end(), [&](const EngineState& state) {
                return state.paused && state.frame > *beforeFrame && state.operationId == playing->operationId &&
                       state.playGeneration == playing->playGeneration;
            });
    }));
    after = seen.cursor();
    auto stopped = seen.command(**session, "play.stop",
                                Json::object()
                                    .set("expectedOperationId", playing->operationId)
                                    .set("expectedPlayGeneration", playing->playGeneration));
    ASSERT_TRUE(stopped) << stopped.error().describe();
    ASSERT_EQ(stopped->stringAt("operationId"), playing->operationId);
    ASSERT_EQ(stopped->stringAt("state"), "Edit");
    ASSERT_EQ(stopped->stringAt("phase"), "completed");
    ASSERT_TRUE(stopped->stringAt("playGeneration").empty());
    ASSERT_TRUE(seen.pushedState(after, *stopped));
    const auto killedAt = std::chrono::steady_clock::now();
    ASSERT_TRUE(editor.kill());
    const auto heartbeatTimeout = std::chrono::milliseconds(seen.identity->heartbeatTimeoutMs);
    ASSERT_GT(heartbeatTimeout.count(), 0);
    ASSERT_TRUE(seen.wait([&] { return !seen.disconnects.empty(); }, heartbeatTimeout));
    EXPECT_LE(std::chrono::steady_clock::now() - killedAt, heartbeatTimeout);
    EXPECT_FALSE((*session)->connected());
    (*session)->shutdown();
}

TEST_F(RealSessionCorpus, DistinctRealInstancesCannotBeAuthenticatedAsAnotherProject) {
    const std::string secondProject = scratch.join("second-project");
    ASSERT_TRUE(build.copyProject(secondProject));
    test::RealEditor first, second;
    auto a = first.start(build, project, scratch.join("first-config"));
    auto b = second.start(build, secondProject, scratch.join("second-config"));
    ASSERT_TRUE(a) << a.error().describe();
    ASSERT_TRUE(b) << b.error().describe();
    ASSERT_NE(a->instanceId, b->instanceId);
    ASSERT_NE(a->projectPath, b->projectPath);
    auto mismatched = *a;
    mismatched.projectPath = b->projectPath;
    SessionObservations rejected;
    auto wrong = EngineSession::create(optionsFor(mismatched), rejected.callbacks());
    ASSERT_TRUE(wrong) << wrong.error().describe();
    ASSERT_TRUE(rejected.wait([&] { return !rejected.disconnects.empty(); }));
    EXPECT_FALSE(rejected.identity);
    (*wrong)->shutdown();
    SessionObservations accepted;
    auto right = EngineSession::create(optionsFor(*b), accepted.callbacks());
    ASSERT_TRUE(right) << right.error().describe();
    ASSERT_TRUE(accepted.wait([&] { return accepted.identity.has_value(); }));
    EXPECT_EQ(accepted.identity->projectPath, b->projectPath);
    EXPECT_EQ(accepted.identity->instanceId, b->instanceId);
    (*right)->shutdown();
}

TEST_F(RealSessionCorpus, NetworkHeartbeatSurvivesARealLongMainThreadLuaTestRun) {
    // Change only our scratch copy's existing script, retaining its real registered asset handle
    // and scene reference. Each callback is finite and remains below the Lua watchdog budget.
    constexpr std::string_view script =
        "local HeartbeatProbe = {}\n"
        "function HeartbeatProbe:OnUpdate()\n"
        "  local accumulator = 0\n"
        "  local untilTime = os.clock() + 0.05\n"
        "  repeat accumulator = (accumulator + 1) % 104729 until os.clock() >= untilTime\n"
        "  self.accumulator = accumulator\n"
        "end\n"
        "return HeartbeatProbe\n";
    ASSERT_TRUE(test::installAcceptanceScript(project, script));
    test::RealEditor editor;
    auto endpoint = editor.start(build, project, scratch.join("config"));
    ASSERT_TRUE(endpoint) << endpoint.error().describe();
    SessionObservations seen;
    auto session = EngineSession::create(optionsFor(*endpoint), seen.callbacks());
    ASSERT_TRUE(session) << session.error().describe();
    ASSERT_TRUE(seen.wait([&] { return seen.identity.has_value(); }));
    const auto timeout = std::chrono::milliseconds(seen.identity->heartbeatTimeoutMs);
    ASSERT_GT(timeout.count(), 0);
    auto startedAt = std::chrono::steady_clock::now();
    const auto frames = (seen.identity->heartbeatTimeoutMs * 2 + 49) / 50;
    auto result = seen.command(**session, "test.run",
                               Json::object()
                                   .set("scenes", Json::array().push("Tests/FeatureTest.fscene"))
                                   .set("frames", frames)
                                   .set("headless", true));
    const auto duration = std::chrono::steady_clock::now() - startedAt;
    ASSERT_TRUE(result) << result.error().describe();
    // A run shorter than the negotiated timeout has not exercised this acceptance criterion.
    // It fails with measured timing, rather than silently claiming a heartbeat success.
    EXPECT_GT(duration, timeout) << "fixture command did not span a heartbeat timeout";
    EXPECT_TRUE((*session)->connected());
    {
        std::lock_guard lock(seen.mutex);
        EXPECT_TRUE(seen.disconnects.empty());
    }
    auto followup = seen.command(**session, "play.state");
    EXPECT_TRUE(followup) << followup.error().describe();
    (*session)->shutdown();
}

TEST_F(RealSessionCorpus, SameSessionOwnerReauthenticatesAfterARealEditorRestartAndReceivesFreshBaseline) {
    test::RealEditor original;
    auto first = original.start(build, project, scratch.join("restart-config"));
    ASSERT_TRUE(first) << first.error().describe();
    std::mutex discoveryMutex;
    std::optional<EngineEndpoint> advertised = *first;
    auto options = optionsFor(*first);
    options.rediscover = [&]() -> Result<EngineEndpoint> {
        std::lock_guard lock(discoveryMutex);
        if (!advertised)
            return fail(ErrorCode::NotFound, "selected scratch editor is restarting");
        return *advertised;
    };
    SessionObservations seen;
    auto session = EngineSession::create(std::move(options), seen.callbacks());
    ASSERT_TRUE(session) << session.error().describe();
    ASSERT_TRUE(seen.wait([&] { return seen.identity.has_value() && !seen.states.empty(); }));
    EngineIdentity previous;
    {
        std::lock_guard lock(seen.mutex);
        previous = *seen.identity;
    }
    {
        std::lock_guard lock(discoveryMutex);
        advertised.reset();
    }
    ASSERT_TRUE(original.kill());
    ASSERT_TRUE(
        seen.wait([&] { return !seen.disconnects.empty(); }, std::chrono::milliseconds(previous.heartbeatTimeoutMs)));
    original.stop();
    const auto after = seen.cursor();
    test::RealEditor replacement;
    auto next = replacement.start(build, project, scratch.join("restart-config"));
    ASSERT_TRUE(next) << next.error().describe();
    ASSERT_NE(next->instanceId, previous.instanceId);
    ASSERT_NE(next->generation, previous.generation);
    {
        std::lock_guard lock(discoveryMutex);
        advertised = *next;
    }
    ASSERT_TRUE(seen.wait(
        [&] {
            return seen.identity && seen.identity->instanceId == next->instanceId &&
                   seen.identity->generation == next->generation &&
                   seen.identity->connectionGeneration > previous.connectionGeneration;
        },
        30s));
    ASSERT_TRUE(seen.wait([&] {
        return std::ranges::any_of(
            seen.states.begin() + static_cast<std::ptrdiff_t>(after), seen.states.end(), [&](const EngineState& state) {
                return state.identity.instanceId == next->instanceId && state.identity.generation == next->generation;
            });
    }));
    EXPECT_TRUE((*session)->connected());
    auto reply = seen.command(**session, "editor.focus");
    ASSERT_TRUE(reply) << reply.error().describe();
    (*session)->shutdown();
}
} // namespace
} // namespace frost::engine
