// SPDX-License-Identifier: Apache-2.0
// FER-16 cleanup cause regression. Protocol identity inputs exercise admission, not crash success.
#include "engine_crash_observer.hpp"
#include <gtest/gtest.h>
namespace frost::app {
namespace {
engine::EngineIdentity identity() {
    engine::EngineIdentity id;
    id.processId = 4321;
    id.projectPath = "project";
    id.instanceId = "instance";
    id.generation = "generation";
    id.connectionGeneration = 7;
    return id;
}
TEST(EngineCrashCleanup, AutomaticMarkedStoppedRetainsOnlyExactPriorUnexpectedLoss) {
    EngineCrashSessionHolder holder;
    const auto id = identity();
    holder.authenticated(id);
    auto live = holder.snapshot();
    ASSERT_TRUE(live);
    EXPECT_TRUE(live->live());
    EXPECT_FALSE(preserveEngineCrashObservation(live, id.processId, true, std::nullopt))
        << "Planned Stop of a live host is not a crash";
    ASSERT_TRUE(holder.lost(id, true));
    auto historical = holder.snapshot();
    ASSERT_TRUE(historical);
    EXPECT_FALSE(historical->live());
    ASSERT_TRUE(historical->admitted());
    EXPECT_TRUE(preserveEngineCrashObservation(historical, id.processId, true, std::nullopt))
        << "Automatic cleanup may mark the process stopped after actual unexpected RPC loss";
    EXPECT_FALSE(preserveEngineCrashObservation(historical, id.processId + 1, true, std::nullopt));
    holder.revoke();
    EXPECT_FALSE(preserveEngineCrashObservation(historical, id.processId, true, std::nullopt))
        << "Explicit Stop/project/trust revocation remains permanent";
}
TEST(EngineCrashCleanup, CleanupDecisionDoesNotInventUnexpectedLossOrValidateDump) {
    EngineCrashSessionHolder holder;
    const auto id = identity();
    holder.authenticated(id);
    auto lease = holder.snapshot();
    ASSERT_TRUE(lease);
    EXPECT_FALSE(preserveEngineCrashObservation(lease, id.processId, false, std::nullopt));
    EXPECT_TRUE(preserveEngineCrashObservation(lease, id.processId, false, 11));
    EXPECT_TRUE(lease->live()) << "Pure decision alone never manufactures a loss event or successful dump";
    EXPECT_FALSE(preserveEngineCrashObservation(lease, 0, false, 11));
    ASSERT_TRUE(holder.lost(id, false));
    EXPECT_FALSE(preserveEngineCrashObservation(lease, id.processId, true, 0));
}
} // namespace
} // namespace frost::app
