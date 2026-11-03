// SPDX-License-Identifier: Apache-2.0
// Test Mono session reply correlation and event ordering.
#include "frost/debug/mono_protocol_session.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace frost::debug::mono {
namespace {
std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
void readyWithBreakpoint(ProtocolSession& session) {
    ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
    ASSERT_TRUE(session.version());
    const std::vector<std::uint8_t> version{0, 0, 0, 24,  0, 0, 0, 1, 0x80, 0, 0, 0,
                                            0, 0, 1, 'x', 0, 0, 0, 2, 0,    0, 0, 58};
    ASSERT_TRUE(session.feed(version));
    ASSERT_TRUE(session.negotiate());
    const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                            1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
    ASSERT_TRUE(session.feed(startup));
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 2, 0x80, 0, 0};
    ASSERT_TRUE(session.feed(ack));
    ASSERT_TRUE(session.breakpoint(2, 4));
    const std::vector<std::uint8_t> bp{0, 0, 0, 15, 0, 0, 0, 3, 0x80, 0, 0, 0, 0, 0, 9};
    ASSERT_TRUE(session.feed(bp));
}
TEST(MonoProtocolSession, ResumeInvalidatesImmediatelyAndDefersEarlyStop) {
    ProtocolSession session;
    readyWithBreakpoint(session);
    const auto generation = session.state().generation();
    ASSERT_TRUE(session.resume());
    EXPECT_FALSE(session.state().current(generation));
    const std::vector<std::uint8_t> stop{0, 0, 0, 37, 0, 0, 0, 7, 0, 64, 100, 2, 0, 0, 0, 1, 10, 0, 0,
                                         0, 9, 0, 0,  0, 1, 0, 0, 0, 2,  0,   0, 0, 0, 0, 0, 0,  0};
    auto early = session.feed(stop);
    ASSERT_TRUE(early);
    EXPECT_TRUE(early->empty());
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 4, 0x80, 0, 0};
    auto delivered = session.feed(ack);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->size(), 2u);
    EXPECT_EQ(session.state().phase(), Phase::Suspended);
}
TEST(MonoProtocolSession, ResumeAckThenStopInOneBuffer) {
    ProtocolSession session;
    readyWithBreakpoint(session);
    ASSERT_TRUE(session.resume());
    std::vector<std::uint8_t> data{0, 0, 0, 11, 0, 0, 0, 4, 0x80, 0, 0};
    const std::vector<std::uint8_t> stop{0, 0, 0, 37, 0, 0, 0, 7, 0, 64, 100, 2, 0, 0, 0, 1, 10, 0, 0,
                                         0, 9, 0, 0,  0, 1, 0, 0, 0, 2,  0,   0, 0, 0, 0, 0, 0,  0};
    data.insert(data.end(), stop.begin(), stop.end());
    const auto delivered = session.feed(data);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->size(), 2u);
    EXPECT_EQ(session.state().phase(), Phase::Suspended);
}
TEST(MonoProtocolSession, StopThenResumeAckInOneBuffer) {
    ProtocolSession session;
    readyWithBreakpoint(session);
    ASSERT_TRUE(session.resume());
    std::vector<std::uint8_t> data{0, 0, 0, 37, 0, 0, 0, 7, 0, 64, 100, 2, 0, 0, 0, 1, 10, 0, 0,
                                   0, 9, 0, 0,  0, 1, 0, 0, 0, 2,  0,   0, 0, 0, 0, 0, 0,  0};
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 4, 0x80, 0, 0};
    data.insert(data.end(), ack.begin(), ack.end());
    const auto delivered = session.feed(data);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->size(), 2u);
    EXPECT_EQ(session.state().phase(), Phase::Suspended);
}
TEST(MonoProtocolSession, ReloadTerminatesAndRejectsLateReply) {
    ProtocolSession session;
    readyWithBreakpoint(session);
    ASSERT_TRUE(session.frames(1));
    session.reload();
    EXPECT_EQ(session.state().phase(), Phase::Ended);
    EXPECT_TRUE(session.uncleanClose());
    const std::vector<std::uint8_t> late{0, 0, 0, 11, 0, 0, 0, 4, 0x80, 0, 0};
    EXPECT_FALSE(session.feed(late));
    EXPECT_FALSE(session.resume());
}
TEST(MonoProtocolSession, RequiresNegotiationAndCorrelatesReplies) {
    ProtocolSession session;
    EXPECT_FALSE(session.resume());
    ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
    ASSERT_TRUE(session.version());
    const std::vector<std::uint8_t> versionReply{0, 0, 0, 24,  0, 0, 0, 1, 0x80, 0, 0, 0,
                                                 0, 0, 1, 'x', 0, 0, 0, 2, 0,    0, 0, 58};
    ASSERT_TRUE(session.feed(versionReply));
    ASSERT_TRUE(session.negotiate());
    const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                            1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
    ASSERT_TRUE(session.feed(startup));
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 2, 0x80, 0, 0};
    ASSERT_TRUE(session.feed(ack));
    EXPECT_EQ(session.state().phase(), Phase::Suspended);
    ASSERT_TRUE(session.resume());
    const std::vector<std::uint8_t> resumeAck{0, 0, 0, 11, 0, 0, 0, 3, 0x80, 0, 0};
    ASSERT_TRUE(session.feed(resumeAck));
    EXPECT_EQ(session.state().phase(), Phase::Running);
    const std::vector<std::uint8_t> unowned{0, 0,  0, 37, 0, 0, 0, 2, 0, 64, 100, 2, 0, 0, 0, 1, 10, 0, 0,
                                            0, 99, 0, 0,  0, 1, 0, 0, 0, 2,  0,   0, 0, 0, 0, 0, 0,  0};
    EXPECT_FALSE(session.feed(unowned));
    EXPECT_EQ(session.state().phase(), Phase::Ended);
}
TEST(MonoProtocolSession, NegotiationNeedsStartupEvidence) {
    ProtocolSession session;
    ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
    ASSERT_TRUE(session.version());
    const std::vector<std::uint8_t> versionReply{0, 0, 0, 24,  0, 0, 0, 1, 0x80, 0, 0, 0,
                                                 0, 0, 1, 'x', 0, 0, 0, 2, 0,    0, 0, 58};
    ASSERT_TRUE(session.feed(versionReply));
    ASSERT_TRUE(session.negotiate());
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 2, 0x80, 0, 0};
    const auto deferred = session.feed(ack);
    ASSERT_TRUE(deferred);
    EXPECT_TRUE(deferred->empty());
    EXPECT_EQ(session.state().phase(), Phase::Negotiating);
    EXPECT_FALSE(session.resume());
    const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                            1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
    const auto delivered = session.feed(startup);
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->size(), 2u);
    EXPECT_EQ(session.state().phase(), Phase::Suspended);
}
TEST(MonoProtocolSession, StartupAndNegotiationAckCanBeCoalescedInEitherOrder) {
    const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                            1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 2, 0x80, 0, 0};
    for (const bool ackFirst : {false, true}) {
        ProtocolSession session;
        ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
        ASSERT_TRUE(session.version());
        const std::vector<std::uint8_t> version{0, 0, 0, 24,  0, 0, 0, 1, 0x80, 0, 0, 0,
                                                0, 0, 1, 'x', 0, 0, 0, 2, 0,    0, 0, 58};
        ASSERT_TRUE(session.feed(version));
        ASSERT_TRUE(session.negotiate());
        auto data = ackFirst ? ack : startup;
        const auto& second = ackFirst ? startup : ack;
        data.insert(data.end(), second.begin(), second.end());
        auto delivered = session.feed(data);
        ASSERT_TRUE(delivered);
        EXPECT_EQ(delivered->size(), 2u);
        EXPECT_EQ(session.state().phase(), Phase::Suspended);
    }
}
TEST(MonoProtocolSession, MissingStartupCanBeCancelledWithoutAdmittingCommands) {
    ProtocolSession session;
    ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
    ASSERT_TRUE(session.version());
    const std::vector<std::uint8_t> version{0, 0, 0, 24,  0, 0, 0, 1, 0x80, 0, 0, 0,
                                            0, 0, 1, 'x', 0, 0, 0, 2, 0,    0, 0, 58};
    ASSERT_TRUE(session.feed(version));
    ASSERT_TRUE(session.negotiate());
    const std::vector<std::uint8_t> ack{0, 0, 0, 11, 0, 0, 0, 2, 0x80, 0, 0};
    ASSERT_TRUE(session.feed(ack));
    EXPECT_FALSE(session.types("Fixture"));
    session.close();
    EXPECT_TRUE(session.uncleanClose());
    EXPECT_FALSE(session.resume());
}
TEST(MonoProtocolSession, RejectsDuplicateAndInvalidStartupEvents) {
    const std::vector<std::uint8_t> startup{0, 0, 0, 29, 0, 0, 0, 1, 0, 64, 100, 2, 0, 0, 0,
                                            1, 0, 0, 0,  0, 0, 0, 0, 0, 1,  0,   0, 0, 1};
    for (const bool duplicate : {false, true}) {
        ProtocolSession session;
        ASSERT_TRUE(session.feed(bytes(Wire::handshake)));
        if (duplicate) {
            ASSERT_TRUE(session.feed(startup));
            EXPECT_FALSE(session.feed(startup));
        } else {
            auto invalid = startup;
            invalid[11] = 1; // Unsupported startup suspension policy.
            EXPECT_FALSE(session.feed(invalid));
        }
        EXPECT_EQ(session.state().phase(), Phase::Ended);
    }
}
} // namespace
} // namespace frost::debug::mono
