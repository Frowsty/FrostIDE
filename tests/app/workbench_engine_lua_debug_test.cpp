// SPDX-License-Identifier: Apache-2.0
// FER-9 GUI coordinator over independent real R6/DAP loopback peers; no real-engine VM claims.
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/ui/debug_session_registry.hpp"

#include "../../src/app/src/engine_integration.hpp"
#include "../../src/app/src/engine_native_debug.hpp"
#include "../../src/app/src/workbench_engine_lua_debug.hpp"
#include "../debug/lua_debug_peer.hpp"
#include "engine_session_peer.hpp"
#include "workbench_rig.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace {
using namespace frost;
using namespace frost::app;
using namespace frost::app::test;
using namespace std::chrono_literals;
using Json = rpc::Json;

class HeldPublicationFs final : public io::IFileSystem {
public:
    std::atomic<bool> hold{false}, entered{false};
    Result<std::string> readFile(std::string_view p) override { return real.readFile(p); }
    Result<void> writeFileAtomic(std::string_view p, std::string_view d) override { return real.writeFileAtomic(p, d); }
    Result<io::FileStatus> status(std::string_view p) override { return real.status(p); }
    Result<std::vector<io::DirEntry>> listDirectory(std::string_view p) override { return real.listDirectory(p); }
    Result<io::DirListing> listDirectoryWithDiagnostics(std::string_view p) override {
        return real.listDirectoryWithDiagnostics(p);
    }
    Result<void> createDirectories(std::string_view p) override { return real.createDirectories(p); }
    Result<void> removeFile(std::string_view p) override { return real.removeFile(p); }
    Result<void> removeDirectory(std::string_view p) override { return real.removeDirectory(p); }
    Result<bool> exists(std::string_view p) override { return real.exists(p); }
    Result<std::string> canonicalPath(std::string_view p) override {
        if (p.ends_with("HeldPublication.lua") && hold.load()) {
            std::unique_lock lock(mutex);
            entered = true;
            (void)changed.wait_for(lock, 10s, [this] { return !hold.load(); });
        }
        return real.canonicalPath(p);
    }
    void release() {
        hold = false;
        changed.notify_all();
    }

private:
    io::IFileSystem& real = io::realFileSystem();
    std::mutex mutex;
    std::condition_variable changed;
};

struct CoordinatorRig {
    WorkDir dir{"lua-coordinator"};
    HeldPublicationFs fileSystem;
    std::unique_ptr<EngineSessionPeer> engine;
    std::unique_ptr<debug::test::LuaDapPeer> dap;
    std::unique_ptr<WorkbenchRig> host;
    std::unique_ptr<EngineIntegration> integration;
    std::unique_ptr<EngineLuaDebugCoordinator> coordinator;
    std::atomic<bool> allowConfiguration{false}, configurationSeen{false}, allowDetach{false}, detachSeen{false},
        ending{false}, allowAttach{true}, configurationAcknowledged{false};
    std::atomic<bool> attachEntry{false};
    std::optional<ui::DebugSessionId> sessionId;
    unsigned ownedStopped = 0, playObserved = 0;
    std::vector<QString> errors;
    std::string generation = std::string(32, 'e'), operation = std::string(32, 'd');
    std::string expectedStoppedGeneration = generation;
    explicit CoordinatorRig(bool startedNow = true, debug::FakeDebugAdapter* native = nullptr) {
        dir.write("project/Game.frostproj", "{}");
        dir.write("project/Scripts/HeldPublication.lua", "return true");
        engine = std::make_unique<EngineSessionPeer>(dir.join("project"));
        auto identity = debug::test::luaIdentity();
        const auto record = engine->record();
        identity.projectPath = record.stringAt("projectPath");
        identity.instanceId = record.stringAt("instanceId");
        identity.generation = record.stringAt("generation");
        dap = std::make_unique<debug::test::LuaDapPeer>(
            [this](debug::test::LuaDapPeer& peer) {
                if (!peer.authenticate())
                    return;
                std::optional<Json> pendingAttach;
                while (!ending) {
                    const auto request = peer.receive();
                    if (!request)
                        return;
                    const auto command = request->stringAt("command");
                    if (command == "initialize") {
                        peer.respond(*request, Json::object().set("supportsConfigurationDoneRequest", true));
                    } else if (command == "attach") {
                        const auto* args = request->find("arguments");
                        attachEntry = args && args->boolAt("stopOnEntry") == true;
                        if (allowAttach)
                            peer.respond(*request);
                        else
                            pendingAttach = *request;
                        peer.event("initialized", Json::object());
                    } else if (command == "configurationDone") {
                        configurationSeen = true;
                        const auto limit = std::chrono::steady_clock::now() + 5s;
                        while (!allowConfiguration && !ending && std::chrono::steady_clock::now() < limit)
                            std::this_thread::sleep_for(2ms);
                        if (!ending && allowConfiguration) {
                            peer.respond(*request);
                            configurationAcknowledged = true;
                            if (pendingAttach) {
                                while (!allowAttach && !ending && std::chrono::steady_clock::now() < limit)
                                    std::this_thread::sleep_for(2ms);
                                if (!ending && allowAttach)
                                    peer.respond(*pendingAttach);
                                pendingAttach.reset();
                            }
                        }
                    } else if (command == "disconnect") {
                        detachSeen = true;
                        const auto limit = std::chrono::steady_clock::now() + 5s;
                        while (!allowDetach && !ending && std::chrono::steady_clock::now() < limit)
                            std::this_thread::sleep_for(2ms);
                        if (!ending && allowDetach)
                            peer.respond(*request);
                    } else
                        peer.respond(*request);
                }
            },
            identity);
        engine->processId = 12345;
        engine->luaDebugEndpoint(dap->port);
        engine->result("play.start", receipt(startedNow));
        dir.write("automation/Instances/" + engine->instanceId() + ".json", engine->record().dump());
        RigOptions options;
        options.disableProjectEvaluation = true;
        if (native) {
            dir.write("project/Native.exe", "protocol fixture executable identity");
            dir.write("project/Native.cpp", "int main() { return 0; }");
            debug::scriptProgram(*native, {.sourcePath = dir.join("project/Native.cpp"), .runsForever = true});
            native->on("configurationDone", [](const Json&) { return debug::FakeDebugAdapter::Reply{}; });
            native->on("launch", [](const Json&) {
                return debug::FakeDebugAdapter::Reply{
                    .events = {{"process", Json::object().set("systemProcessId", 12345)},
                               {"initialized", Json::object()}}};
            });
            options.workbench.debugAdapterFactory =
                [native](debug::DapHandlers handlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
                return debug::makeDapClient(native->transport(), std::move(handlers));
            };
        }
        options.fileSystem = &fileSystem;
        options.workbench.engineAutomationDirectory = dir.join("automation");
        host = std::make_unique<WorkbenchRig>(dir, options);
        EXPECT_TRUE(host->settings->set(settings::Scope::Application, settings::key::toolsLuaLanguageServerPath,
                                        dir.join("missing-lua-server")));
        EXPECT_TRUE(host->settings->set(settings::Scope::Application, settings::key::toolsManagedServerPath,
                                        dir.join("missing-managed-server.dll")));
        EXPECT_TRUE(host->trust->trust(dir.join("project")));
        EXPECT_TRUE(host->workbench->openPath(dir.join("project")));
        // Public coordinators are composed independently here; the Workbench's own idle engine
        // integration never pairs, so only this selected connection has authority over either peer.
        integration = std::make_unique<EngineIntegration>(*host->workbench);
        coordinator = std::make_unique<EngineLuaDebugCoordinator>(*integration);
        QObject::connect(coordinator.get(), &EngineLuaDebugCoordinator::failed, coordinator.get(),
                         [this](const QString& error) { errors.push_back(error); });
        QObject::connect(coordinator.get(), &EngineLuaDebugCoordinator::playStartObserved, coordinator.get(),
                         [this](std::uint64_t id, bool, const QString&) {
                             EXPECT_EQ(sessionId, id);
                             ++playObserved;
                         });
        QObject::connect(coordinator.get(), &EngineLuaDebugCoordinator::ownedPlayStopped, coordinator.get(),
                         [this](std::uint64_t id, const QString& observed) {
                             EXPECT_EQ(sessionId, id);
                             EXPECT_EQ(observed.toStdString(), expectedStoppedGeneration);
                             ++ownedStopped;
                         });
    }
    ~CoordinatorRig() {
        fileSystem.release();
        ending = true;
        allowConfiguration = true;
        allowDetach = true;
        allowAttach = true;
        coordinator.reset();
        if (integration)
            integration->shutdown();
        integration.reset();
        if (host && host->workbench)
            host->workbench->shutdown();
        host.reset();
        dap.reset();
        engine.reset();
    }
    bool pair() {
        if (!waitFor([&] {
                const auto state = integration->snapshot();
                const auto* instances = state.find("instances");
                return instances && std::ranges::any_of(instances->items(), [this](const Json& value) {
                           return value.stringAt("instanceId") == engine->instanceId();
                       });
            }))
            return false;
        auto accepted = integration->pair(engine->instanceId());
        EXPECT_TRUE(accepted);
        return accepted && waitFor([&] { return integration->captureLuaDebugOwner().has_value(); });
    }
    bool prepare() {
        auto result = coordinator->prepare(true, {}, [this](Result<PreparedDebugLaunch> prepared) {
            EXPECT_TRUE(prepared) << (prepared ? "" : prepared.error().message());
            if (!prepared)
                return;
            auto started = prepared->start(*host->workbench->debugSessions(), 0);
            EXPECT_TRUE(started) << (started ? "" : started.error().message());
            if (started)
                sessionId = *started;
        });
        EXPECT_TRUE(result);
        return result && waitFor([&] { return configurationSeen.load(); });
    }
    ui::DebugController* child() {
        return sessionId ? host->workbench->debugSessions()->controller(*sessionId) : nullptr;
    }
    Json receipt(bool accepted = true) const {
        return Json::object()
            .set("accepted", accepted)
            .set("pending", accepted)
            .set("startedNow", false)
            .set("operationId", operation)
            .set("phase", accepted ? "pending" : "playing")
            .set("playGeneration", accepted ? std::string{} : generation);
    }
    void push(std::string state, std::string playGeneration, bool identified = true,
              std::string terminal = "completed") {
        engine->event("play.state",
                      Json::object()
                          .set("state", state)
                          .set("mode", state)
                          .set("paused", false)
                          .set("frames", 1)
                          .set("playGeneration", std::move(playGeneration))
                          .set("operationId", identified ? operation : std::string{})
                          .set("phase", identified ? (state == "Play" ? "playing" : terminal) : std::string{}));
    }

    void pump() {
        (void)waitFor([] { return false; }, 100ms);
    }
};

TEST(EngineLuaDebugCoordinator, ConfigurationAcknowledgmentPrecedesPlayAndOldEditCannotRevokeNewLease) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    ASSERT_NE(rig.child(), nullptr);
    EXPECT_FALSE(rig.child()->configured());
    EXPECT_TRUE(rig.attachEntry.load());
    rig.pump();
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    rig.pump();
    EXPECT_TRUE(rig.child()->configured());
    EXPECT_TRUE(rig.child()->debugging());
    EXPECT_EQ(rig.integration->snapshot().stringAt("state"), "Edit"); // Still the actual old push.
    EXPECT_FALSE(rig.detachSeen.load()); // A reply must not manufacture/invalidate a Play state.
    EXPECT_TRUE(rig.errors.empty());
    EXPECT_EQ(rig.engine->requests("play.start").size(), 1U);
}

TEST(EngineLuaDebugCoordinator, WorkerReplyFenceRejectsLatePublicationOfNewerPreStartEdit) {
    CoordinatorRig rig;
    rig.engine->hold("play.start");
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("play.start").size() == 1; }));
    rig.fileSystem.hold = true;
    rig.engine->log("hold actual GUI publication", "Scripts/HeldPublication.lua", 1);
    ASSERT_TRUE(waitFor([&] { return rig.fileSystem.entered.load(); }));
    rig.push("Edit", "", false); // Sequence newer than the old GUI snapshot, before actual start reply.
    std::uint64_t wireFence = 0;
    auto owner = rig.integration->captureLuaDebugOwner();
    ASSERT_TRUE(owner);
    for (unsigned attempt = 0; attempt < 4 && wireFence < 2; ++attempt) {
        bool fenceObserved = false;
        ASSERT_TRUE(rig.integration->commandForLua(*owner, "play.state", Json::object(),
                                                   [&](Result<engine::EngineCommandObservation> result) {
                                                       EXPECT_TRUE(result);
                                                       if (result)
                                                           wireFence = result->stateSequenceAtReply;
                                                       fenceObserved = true;
                                                   },
                                                   {}));
        ASSERT_TRUE(waitFor([&] { return fenceObserved; }));
    }
    ASSERT_GE(wireFence, 2U); // Both actual log and pre-start Edit are processed on the wire.
    rig.engine->release("play.start", rig.receipt());
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    rig.fileSystem.release();
    ASSERT_TRUE(
        waitFor([&] { return rig.integration->snapshot().unsignedAt("eventSequence").value_or(0) >= wireFence; }));
    EXPECT_EQ(rig.integration->snapshot().stringAt("state"), "Edit");
    EXPECT_TRUE(rig.child()->debugging());
    EXPECT_FALSE(rig.detachSeen.load());
    rig.push("Play", rig.generation);
    ASSERT_TRUE(waitFor([&] { return rig.integration->snapshot().stringAt("state") == "Play"; }));
    EXPECT_TRUE(rig.child()->debugging());
}

TEST(EngineLuaDebugCoordinator, SceneDebugReportsActualPrivateBootstrapFailure) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    rig.engine->hold("script.debug.bootstrap");
    ASSERT_TRUE(rig.coordinator->debugScene(*rig.host->workbench->debugSessions(), "Assets/Case.fscene", 1));
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("script.debug.bootstrap").size() == 1; }));
    // A held bootstrap receives an actual malformed reply; no DAP adoption or scene run follows.
    rig.engine->release("script.debug.bootstrap", Json::object().set("credential", "bad-private-credential"));
    ASSERT_TRUE(waitFor([&] { return !rig.errors.empty(); }));
    EXPECT_TRUE(rig.engine->requests("test.run").empty());
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_EQ(rig.dap->accepted.load(), 0);
    for (const auto& error : rig.errors)
        EXPECT_FALSE(error.contains(QStringLiteral("bad-private-credential")));
}

TEST(EngineLuaDebugCoordinator, ConfigurationAckCannotSubstituteForActualAttachAck) {
    CoordinatorRig rig;
    rig.allowAttach = false;
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.configurationAcknowledged.load() && rig.child()->configured(); }));
    rig.pump();
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_EQ(rig.playObserved, 0U);
    rig.allowAttach = true;
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    EXPECT_EQ(rig.engine->requests("play.start").size(), 1U);
}

TEST(EngineLuaDebugCoordinator, OwnedStopRequiresActualDetachThenGuardedGenerationAndNewEditPush) {
    CoordinatorRig rig;
    rig.engine->hold("play.stop");
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    rig.push("Play", rig.generation);
    ASSERT_TRUE(waitFor([&] { return rig.integration->snapshot().stringAt("state") == "Play"; }));
    rig.child()->stop();
    ASSERT_TRUE(waitFor([&] { return rig.detachSeen.load(); }));
    EXPECT_TRUE(rig.engine->requests("play.stop").empty());
    EXPECT_EQ(rig.ownedStopped, 0U);
    rig.allowDetach = true;
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("play.stop").size() == 1; }));
    const auto request = rig.engine->requests("play.stop").front();
    ASSERT_NE(request.find("params"), nullptr);
    EXPECT_EQ(request.find("params")->stringAt("expectedPlayGeneration"), rig.generation);
    EXPECT_EQ(request.find("params")->stringAt("expectedOperationId"), rig.operation);
    rig.engine->release("play.stop", Json::object().set("stopping", true));
    ASSERT_TRUE(waitFor([&] { return rig.engine->replies("play.stop") == 1; }));
    rig.pump();
    EXPECT_EQ(rig.ownedStopped, 0U);
    EXPECT_EQ(rig.integration->snapshot().stringAt("state"), "Play");
    rig.push("Edit", "");
    ASSERT_TRUE(waitFor([&] { return rig.ownedStopped == 1; }));
    EXPECT_TRUE(rig.errors.empty());
}

TEST(EngineLuaDebugCoordinator, AlreadyPlayingAttachNeverAcquiresGuardedStopOwnership) {
    CoordinatorRig rig(false);
    rig.allowDetach = true;
    ASSERT_TRUE(rig.pair());
    rig.push("Play", rig.generation);
    ASSERT_TRUE(waitFor([&] { return rig.integration->snapshot().stringAt("state") == "Play"; }));
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    rig.child()->stop();
    ASSERT_TRUE(waitFor([&] { return rig.detachSeen.load(); }));
    rig.pump();
    EXPECT_TRUE(rig.engine->requests("play.stop").empty());
    EXPECT_EQ(rig.ownedStopped, 0U);
    EXPECT_EQ(rig.integration->snapshot().stringAt("state"), "Play");
}

TEST(EngineLuaDebugCoordinator, StopWhileOperationReceiptIsPendingWaitsForActualReceiptBeforeGuardedCancel) {
    CoordinatorRig rig;
    rig.engine->hold("play.start");
    rig.allowDetach = true;
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("play.start").size() == 1; }));
    rig.expectedStoppedGeneration.clear();
    rig.child()->stop();
    ASSERT_TRUE(waitFor([&] { return rig.detachSeen.load(); }));
    rig.pump();
    EXPECT_TRUE(rig.engine->requests("play.stop").empty());
    rig.engine->release("play.start", rig.receipt());
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("play.stop").size() == 1; }));
    const auto stop = rig.engine->requests("play.stop").front();
    ASSERT_NE(stop.find("params"), nullptr);
    EXPECT_EQ(stop.find("params")->stringAt("expectedOperationId"), rig.operation);
    EXPECT_EQ(stop.find("params")->find("expectedPlayGeneration"), nullptr);
    rig.push("Edit", "", true, "cancelled");
    ASSERT_TRUE(waitFor([&] { return rig.ownedStopped == 1; }));
}

TEST(EngineLuaDebugCoordinator, TrustOrSessionEpochRevocationBeforeConfigurationSendsNoPlay) {
    for (const bool revokeTrust : {true, false}) {
        SCOPED_TRACE(revokeTrust);
        CoordinatorRig rig;
        ASSERT_TRUE(rig.pair());
        ASSERT_TRUE(rig.prepare());
        if (revokeTrust) {
            ASSERT_TRUE(rig.host->trust->untrust(rig.dir.join("project")));
            rig.integration->trustChanged();
        } else
            rig.integration->cancel();
        rig.allowConfiguration = true;
        ASSERT_TRUE(waitFor([&] { return !rig.child() || !rig.child()->debugging(); }));
        EXPECT_TRUE(rig.engine->requests("play.start").empty());
        EXPECT_TRUE(rig.engine->requests("play.stop").empty());
        EXPECT_EQ(rig.ownedStopped, 0U);
    }
}

TEST(EngineLuaDebugCoordinator, ReplacementPlayGenerationCannotInheritOldStopLease) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare());
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    rig.push("Play", rig.generation);
    ASSERT_TRUE(waitFor([&] { return rig.integration->snapshot().stringAt("state") == "Play"; }));
    rig.push("Play", std::string(32, 'f'));
    ASSERT_TRUE(waitFor([&] { return !rig.child()->debugging(); }));
    rig.child()->stop();
    rig.pump();
    EXPECT_TRUE(rig.engine->requests("play.stop").empty());
    EXPECT_EQ(rig.ownedStopped, 0U);
}

TEST(EngineLuaDebugCoordinator, WireConfiguredChildSurvivesNativePauseBeforeGuiBarrierAndWaitsForContinued) {
    debug::FakeDebugAdapter native;
    CoordinatorRig rig(true, &native);
    ASSERT_TRUE(rig.pair());
    debug::DebugTarget target;
    target.name = "native protocol host";
    target.program = rig.dir.join("project/Native.exe");
    target.trustRoots = {rig.dir.join("project")};
    ASSERT_TRUE(rig.host->workbench->debugController()->startTarget(target));
    ASSERT_TRUE(waitFor([&] { return rig.integration->nativeDebugBinding().has_value(); }));
    ASSERT_TRUE(rig.prepare());
    ASSERT_TRUE(waitFor([&] { return native.argumentsOf("threads").size() >= 1; }));
    const auto nativeThreadRequests = native.argumentsOf("threads").size();
    native.sendEvent("stopped",
                     Json::object().set("reason", "pause").set("threadId", 1).set("allThreadsStopped", true));
    const auto spinWire = [](const std::function<bool()>& observed) {
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (!observed() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        return observed();
    };
    // Do not pump Qt: the actual native stopped relay must precede the Lua GUI configuration relay.
    ASSERT_TRUE(spinWire([&] { return native.argumentsOf("threads").size() > nativeThreadRequests; }));
    rig.allowConfiguration = true;
    ASSERT_TRUE(spinWire([&] {
        return rig.child()->session()->state() == debug::DebugState::Running;
    })); // Core has both real ACKs; GUI still has neither new relay.
    ASSERT_TRUE(waitFor(
        [&] { return rig.child()->configured() && rig.host->workbench->debugController()->adapterStoppedEvidence(); }));
    EXPECT_TRUE(rig.child()->debugging());
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_TRUE(rig.errors.empty());
    EXPECT_TRUE(native.argumentsOf("continue").empty());
    native.sendEvent("continued", Json::object().set("allThreadsContinued", true));
    ASSERT_TRUE(waitFor([&] { return rig.playObserved == 1; }));
    EXPECT_EQ(rig.engine->requests("play.start").size(), 1U);
    EXPECT_TRUE(native.argumentsOf("continue").empty()); // Actual event observed; never an automatic request.
}

TEST(EngineLuaDebugCoordinator, ScopedUnfinishedChildCancellationSendsNoDisconnectOrPlayAndReportsNoFakeAck) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    ASSERT_TRUE(rig.prepare()); // Actual attach ACK; actual configurationDone ACK is held by the peer.
    ASSERT_NE(rig.child(), nullptr);
    const auto id = *rig.sessionId;
    const auto generation = rig.child()->sessionGeneration();
    ASSERT_TRUE(rig.coordinator->cancelPreparation(id, generation));
    ASSERT_TRUE(waitFor([&] { return !rig.child()->debugging(); }));
    EXPECT_FALSE(rig.detachSeen.load()); // This is local cancellation, never a fabricated detach response.
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_TRUE(rig.engine->requests("play.stop").empty());
    ASSERT_TRUE(rig.child()->lastOutcome());
    EXPECT_TRUE(rig.child()->lastOutcome()->stopped);
    EXPECT_FALSE(rig.child()->lastOutcome()->error);
    EXPECT_EQ(rig.ownedStopped, 0U);
    auto stale = rig.coordinator->cancelPreparation(id, generation);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code(), ErrorCode::NotFound);
}

TEST(EngineLuaDebugCoordinator, PublicGroupStopCancelsHeldBootstrapAndPreventsLateAdoption) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    rig.engine->hold("script.debug.bootstrap");
    ASSERT_TRUE(rig.coordinator->startAttached(*rig.host->workbench->debugSessions(), true));
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("script.debug.bootstrap").size() == 1; }));
    EXPECT_TRUE(rig.host->workbench->debugSessions()->hasPendingWork());
    rig.host->workbench->debugSessions()->stopAll();
    rig.engine->release("script.debug.bootstrap", Json::object().set("credential", "late-private-bootstrap"));
    ASSERT_TRUE(waitFor([&] { return !rig.host->workbench->debugSessions()->hasPendingWork(); }));
    rig.pump();
    EXPECT_EQ(rig.dap->accepted.load(), 0);
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_FALSE(rig.host->workbench->debugSessions()->anyDebugging());
}

TEST(EngineLuaDebugCoordinator, SceneDebugReportsImmediateNativePauseAdmissionRefusalWithoutTestRunBytes) {
    debug::FakeDebugAdapter native; // Actual DAP event evidence from a protocol double; no OS pause claim.
    CoordinatorRig rig(true, &native);
    ASSERT_TRUE(rig.pair());
    debug::DebugTarget target;
    target.name = "native protocol host";
    target.program = rig.dir.join("project/Native.exe");
    target.trustRoots = {rig.dir.join("project")};
    ASSERT_TRUE(rig.host->workbench->debugController()->startTarget(target));
    ASSERT_TRUE(waitFor([&] { return rig.integration->nativeDebugBinding().has_value(); }));
    EngineLuaDebugLifecycle lifecycle;
    lifecycle.configured = [&](const EngineLuaDebugBinding& binding) {
        rig.sessionId = binding.registrySessionId;
        native.sendEvent("stopped",
                         Json::object().set("reason", "pause").set("threadId", 1).set("allThreadsStopped", true));
        EXPECT_TRUE(waitFor([&] {
            return rig.host->workbench->debugController()->adapterStoppedEvidence() &&
                   rig.integration->snapshot().boolAt("debuggerPaused") == true;
        }));
    };
    ASSERT_TRUE(rig.coordinator->debugScene(*rig.host->workbench->debugSessions(), "Assets/Case.fscene", 1, {},
                                            std::move(lifecycle)));
    ASSERT_TRUE(waitFor([&] { return rig.configurationSeen.load(); }));
    rig.allowConfiguration = true;
    ASSERT_TRUE(waitFor([&] { return !rig.errors.empty(); }));
    EXPECT_TRUE(rig.engine->requests("test.run").empty());
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    EXPECT_TRUE(rig.host->workbench->debugController()->adapterStoppedEvidence());
    EXPECT_TRUE(native.argumentsOf("continue").empty());
    EXPECT_TRUE(native.argumentsOf("terminate").empty());
}

TEST(EngineLuaDebugCoordinator, AdditionalOwnerGuardSurvivesBootstrapAndLateAdoption) {
    CoordinatorRig rig;
    ASSERT_TRUE(rig.pair());
    auto owner = rig.integration->captureLuaDebugOwner();
    ASSERT_TRUE(owner);
    std::atomic<bool> nativeHostLease{true};
    const auto originalCurrent = owner->current;
    const auto originalAdmission = owner->admit;
    owner->current = [&] { return nativeHostLease.load() && originalCurrent(); };
    owner->admit = [&](const debug::LuaDebugIdentity& identity, std::string_view command) -> Result<void> {
        if (!nativeHostLease.load())
            return fail(ErrorCode::Cancelled, "exact additional host lease revoked");
        return originalAdmission(identity, command);
    };
    rig.engine->hold("script.debug.bootstrap");
    bool completed = false;
    ASSERT_TRUE(rig.coordinator->prepareForOwner(std::move(*owner), true, {}, [&](Result<PreparedDebugLaunch> result) {
        EXPECT_FALSE(result);
        completed = true;
    }));
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("script.debug.bootstrap").size() == 1; }));
    nativeHostLease = false;
    const auto record = rig.engine->record();
    rig.engine->release("script.debug.bootstrap", Json::object()
                                                      .set("protocolVersion", 1)
                                                      .set("host", "127.0.0.1")
                                                      .set("port", static_cast<std::uint64_t>(rig.dap->port))
                                                      .set("processId", 12345)
                                                      .set("expiresMs", 30000)
                                                      .set("instanceId", record.stringAt("instanceId"))
                                                      .set("generation", record.stringAt("generation"))
                                                      .set("projectPath", record.stringAt("projectPath"))
                                                      .set("debugSessionId", std::string(32, 'c'))
                                                      .set("credential", std::string(64, 'd')));
    ASSERT_TRUE(waitFor([&] { return completed; }));
    EXPECT_EQ(rig.dap->accepted.load(), 0);
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
}

TEST(EngineLuaDebugCoordinator, ActualNativePauseCancelsOnlyUnfinishedPrivateAttemptWithoutContinuingHost) {
    debug::FakeDebugAdapter native; // Framed native DAP protocol double, not an OS debugger oracle.
    CoordinatorRig rig(true, &native);
    ASSERT_TRUE(rig.pair());
    debug::DebugTarget target;
    target.name = "native protocol host";
    target.program = rig.dir.join("project/Native.exe");
    target.trustRoots = {rig.dir.join("project")};
    ASSERT_TRUE(rig.host->workbench->debugController()->startTarget(target));
    ASSERT_TRUE(waitFor([&] { return rig.integration->nativeDebugBinding().has_value(); }));
    rig.engine->hold("script.debug.bootstrap");
    std::optional<EngineLuaDebugInterruption> interruption;
    EngineLuaDebugLifecycle lifecycle;
    lifecycle.interrupted = [&](EngineLuaDebugInterruption reason, const Error&) { interruption = reason; };
    ASSERT_TRUE(rig.coordinator->prepare(
        true, {}, [](Result<PreparedDebugLaunch> prepared) { EXPECT_FALSE(prepared); }, true, std::move(lifecycle)));
    ASSERT_TRUE(waitFor([&] { return rig.engine->requests("script.debug.bootstrap").size() == 1; }));
    native.sendEvent("stopped",
                     Json::object().set("reason", "pause").set("threadId", 1).set("allThreadsStopped", true));
    ASSERT_TRUE(waitFor([&] { return interruption.has_value(); }));
    EXPECT_EQ(interruption, EngineLuaDebugInterruption::NativePaused);
    EXPECT_TRUE(rig.host->workbench->debugController()->adapterStoppedEvidence());
    EXPECT_TRUE(native.argumentsOf("continue").empty());
    EXPECT_TRUE(native.argumentsOf("terminate").empty());
    EXPECT_TRUE(native.argumentsOf("disconnect").empty());
    EXPECT_TRUE(rig.integration->snapshot().boolAt("connected").value_or(false));
    EXPECT_EQ(rig.dap->accepted.load(), 0);
    EXPECT_TRUE(rig.engine->requests("play.start").empty());
    rig.engine->release("script.debug.bootstrap", Json::object().set("credential", "late-private-bootstrap"));
    rig.pump();
    EXPECT_TRUE(rig.host->workbench->debugController()->adapterStoppedEvidence());
    EXPECT_TRUE(native.argumentsOf("continue").empty());
}
} // namespace
