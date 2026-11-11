// SPDX-License-Identifier: Apache-2.0
// Mandatory RUN-11 corpus acceptance: actual typed editor and player launches through RunManager
// establish persistent authenticated EngineSessions, receive real state and disconnect on owned Stop.

#include "frost/engine/session.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "run/run_collector.hpp"
#include "support/real_engine_session.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace frost::run {
namespace {
using namespace std::chrono_literals;

// Keep the production RunManager path intact while giving every real fixture child a private
// desktop. This adapter delegates to the actual launcher and never produces synthetic output.
class PrivateEngineLauncher final : public io::IProcessLauncher {
public:
    bool valid() const {
#ifdef _WIN32
        return m_desktop.valid();
#else
        return true;
#endif
    }
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& original) override {
        if (!valid()) {
            return fail(ErrorCode::IoError, "private desktop unavailable; refusing a visible engine fixture");
        }
        auto spec = original;
#ifdef _WIN32
        spec.desktop = m_desktop.name();
#endif
        return io::realProcessLauncher().launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& original,
                                                           const io::AsyncContext& context) override {
        if (!valid()) {
            return fail(ErrorCode::IoError, "private desktop unavailable; refusing a visible engine fixture");
        }
        auto spec = original;
#ifdef _WIN32
        spec.desktop = m_desktop.name();
#endif
        return io::realProcessLauncher().launchAsync(spec, context);
    }

private:
#ifdef _WIN32
    test::PrivateDesktop m_desktop;
#endif
};

struct SessionEvents {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<engine::EngineIdentity> identity;
    bool disconnected = false;
    std::optional<engine::EngineState> state;

    engine::EngineSessionCallbacks callbacks() {
        engine::EngineSessionCallbacks result;
        result.connected = [this](const auto& connected) {
            std::lock_guard lock(mutex);
            identity = connected;
            changed.notify_all();
        };
        result.state = [this](const auto& value) {
            std::lock_guard lock(mutex);
            state = value;
            changed.notify_all();
        };
        result.disconnected = [this](const auto&) {
            std::lock_guard lock(mutex);
            disconnected = true;
            changed.notify_all();
        };
        return result;
    }
    bool wait(const std::function<bool()>& predicate) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 30s, predicate);
    }
};

class FrostEngineRunCorpus : public testing::Test {
protected:
    void SetUp() override {
        const auto prepared = build.prepare({"FrostEditor", "FrostRuntime"});
        ASSERT_TRUE(prepared) << prepared.error().describe();
        project = scratch.join("project with spaces");
        const auto copied = build.copyProject(project);
        ASSERT_TRUE(copied) << copied.error().describe();
        ASSERT_TRUE(launcher.valid());
    }
    RunConfiguration configuration(RunType type) const {
        RunConfiguration result;
        result.id = std::string(toString(type));
        result.name = std::string(toString(type)) + " pinned corpus";
        result.type = type;
        result.program = build.program;
        result.engineProjectPath = project;
        result.workingDirectory = project;
        return result;
    }
    // Both editor and player use this exact real launch/session path; only the host-specific flags
    // and credential provisioning differ. Parse complete startup records, never log their contents.
    void authenticateAndStop(const RunConfiguration& config, std::string knownSecret = {}) {
        const auto endpointIn = [&](std::string_view output) -> std::optional<engine::EngineEndpoint> {
            while (!output.empty()) {
                const auto newline = output.find('\n');
                if (newline == std::string_view::npos) {
                    break;
                }
                auto parsed = rpc::parseJson(output.substr(0, newline));
                output.remove_prefix(newline + 1);
                if (!parsed) {
                    continue;
                }
                const auto* record = parsed->find("result");
                if (record == nullptr || !record->isObject()) {
                    record = &*parsed;
                }
                const auto port = record->unsignedAt("port").value_or(0);
                if (port == 0 || port > 65535) {
                    continue;
                }
                engine::EngineEndpoint endpoint;
                endpoint.port = static_cast<std::uint16_t>(port);
                endpoint.projectPath = std::string(record->stringAt("projectPath"));
                endpoint.instanceId = std::string(record->stringAt("instanceId"));
                endpoint.generation = std::string(record->stringAt("generation"));
                endpoint.secret = knownSecret.empty() ? std::string(record->stringAt("token")) : knownSecret;
                return endpoint;
            }
            return std::nullopt;
        };
        test::RunCollector run;
        auto manager = RunManager::create(launcher);
        auto started = manager->start(config, run.handlers());
        ASSERT_TRUE(started.has_value()) << started.error().describe();
        const auto output = run.waitUntil(
            [&](const auto& snapshot) { return endpointIn(snapshot.out).has_value() || snapshot.result.has_value(); },
            120s);
        ASSERT_FALSE(output.result.has_value()) << "the real engine exited before session establishment";
        const auto endpoint = endpointIn(output.out);
        ASSERT_TRUE(endpoint.has_value()) << "real engine did not publish an automation startup record";
        ASSERT_FALSE(endpoint->secret.empty());
        // The endpoint must belong to the exact project selected by the typed configuration.
        EXPECT_EQ(io::normalizePath(endpoint->projectPath), io::normalizePath(project));
        const auto active = manager->status(*started);
        ASSERT_TRUE(active.has_value());
        EXPECT_EQ(active->configuration.type, config.type);
        EXPECT_EQ(active->phase, RunPhase::Running);
        EXPECT_NE(active->processId, 0U);

        engine::EngineSessionOptions options;
        options.endpoint = *endpoint;
        options.trustRevision = 1;
        options.trusted = [expected = endpoint->projectPath](std::string_view path, std::uint64_t revision) {
            return path == expected && revision == 1;
        };
        options.admitCommand = [](std::string_view, const rpc::Json&, const engine::EngineIdentity&) -> Result<void> {
            return {};
        };
        SessionEvents seen;
        auto session = engine::EngineSession::create(std::move(options), seen.callbacks());
        ASSERT_TRUE(session.has_value()) << session.error().describe();
        ASSERT_TRUE(seen.wait([&] { return seen.identity.has_value() && seen.state.has_value(); }))
            << "real engine did not authenticate and push state for the run session";
        {
            std::lock_guard lock(seen.mutex);
            EXPECT_EQ(seen.identity->instanceId, endpoint->instanceId);
            EXPECT_EQ(seen.identity->generation, endpoint->generation);
            EXPECT_EQ(seen.identity->projectPath, endpoint->projectPath);
            EXPECT_EQ(seen.state->identity.instanceId, endpoint->instanceId);
            EXPECT_FALSE(seen.state->state.empty());
            if (config.type == RunType::FrostPlayer) {
                EXPECT_EQ(seen.identity->role, "player")
                    << "authenticated server.info identifies the actual runtime host";
            }
        }
        EXPECT_TRUE((*session)->connected());
        {
            std::lock_guard lock(seen.mutex);
            seen.disconnected = false;
        }
        manager->stop(*started);
        const auto finished = run.waitFinished(30s).result;
        ASSERT_TRUE(finished.has_value());
        EXPECT_TRUE(finished->stopped);
        EXPECT_FALSE(finished->cleanupFailed);
        EXPECT_TRUE(manager->waitForIdle(30s));
        EXPECT_TRUE(seen.wait([&] { return seen.disconnected; }));
        (*session)->shutdown();
    }

    test::RealEngineBuild build;
    test::ScratchDir scratch{"run11-corpus"};
    std::string project;
    PrivateEngineLauncher launcher;
};

TEST_F(FrostEngineRunCorpus, TypedEditorEstablishesAuthenticatedPersistentSessionThenOwnedStopDisconnects) {
    auto config = configuration(RunType::FrostEditor);
    config.arguments = {"--offscreen",         "--size", "320x240", "--automation-port", "0", "--config-dir",
                        scratch.join("config")};
    authenticateAndStop(config);
}

TEST_F(FrostEngineRunCorpus, TypedActualPlayerEstablishesAuthenticatedPersistentSessionThenOwnedStopDisconnects) {
    auto config = configuration(RunType::FrostPlayer);
#ifdef _WIN32
    constexpr std::string_view executable = "FrostRuntime.exe";
#else
    constexpr std::string_view executable = "FrostRuntime";
#endif
    const auto runtime = io::joinPath(io::parentPath(build.program), executable);
    ASSERT_TRUE(runtime.has_value());
    config.program = *runtime;
    config.arguments = {"--headless", "--automation-port", "0"};
    // Runtime credentials live only in this launch's environment and the session. The player
    // deliberately omits them from stdout and discovery; generate a fresh fixture credential.
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string secret;
    for (int i = 0; i < 64; ++i) {
        secret += digits[random() & 15U];
    }
    config.environment.push_back({"FROST_AUTOMATION_TOKEN", secret});
    authenticateAndStop(config, secret);
}

} // namespace
} // namespace frost::run
