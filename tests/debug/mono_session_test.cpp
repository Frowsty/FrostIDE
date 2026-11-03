// SPDX-License-Identifier: Apache-2.0
// Test Mono session transitions and generation invalidation.
#include "frost/debug/mono_session.hpp"

#include <gtest/gtest.h>

namespace frost::debug::mono {
namespace {
TEST(MonoSession, NegotiationAndFrameGenerations) {
    SessionState state;
    EXPECT_FALSE(state.resumed());
    ASSERT_TRUE(state.handshakeComplete());
    EXPECT_FALSE(state.versionReply(2, 57));
    ASSERT_TRUE(state.versionReply(2, 58));
    EXPECT_EQ(state.phase(), Phase::Negotiating);
    ASSERT_TRUE(state.negotiated(true));
    const auto stop = state.generation();
    ASSERT_TRUE(state.resumed());
    EXPECT_FALSE(state.current(stop));
    const auto running = state.generation();
    ASSERT_TRUE(state.stopped());
    EXPECT_FALSE(state.current(running));
    const auto beforeReload = state.generation();
    state.reload();
    EXPECT_FALSE(state.current(beforeReload));
    EXPECT_EQ(state.phase(), Phase::Reloading);
    EXPECT_FALSE(state.resumed());
    ASSERT_TRUE(state.reloaded(false));
    EXPECT_EQ(state.phase(), Phase::Running);
    const auto beforeClose = state.generation();
    state.closing();
    EXPECT_FALSE(state.current(beforeClose));
    EXPECT_EQ(state.phase(), Phase::Closing);
    state.ended();
    EXPECT_EQ(state.phase(), Phase::Ended);
}
TEST(MonoSession, ReloadBeforeNegotiationFailsClosed) {
    SessionState state;
    state.reload();
    EXPECT_EQ(state.phase(), Phase::Ended);
    EXPECT_FALSE(state.reloaded(false));
}
} // namespace
} // namespace frost::debug::mono
