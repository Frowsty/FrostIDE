// SPDX-License-Identifier: Apache-2.0
// Test Mono wire packet encoding, framing and bounds.
#include "frost/debug/mono_wire.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace frost::debug::mono {
namespace {
static_assert(!std::is_copy_constructible_v<Wire>);
static_assert(!std::is_move_constructible_v<Wire>);
std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

TEST(MonoWire, FragmentedHandshakeAndIndependentEventIds) {
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes("DWP-")));
    EXPECT_FALSE(wire.ready());
    ASSERT_TRUE(wire.feed(bytes("Handshake")));
    ASSERT_TRUE(wire.ready());
    const auto version = wire.version("version");
    ASSERT_TRUE(version);
    EXPECT_EQ(*version, (std::vector<std::uint8_t>{0, 0, 0, 11, 0, 0, 0, 1, 0, 1, 1}));
    const std::array<std::uint8_t, 22> incoming{0, 0, 0, 11, 0, 0, 0, 1, 0,    64, 100,
                                                0, 0, 0, 11, 0, 0, 0, 1, 0x80, 0,  0};
    auto packets = wire.feed(incoming);
    ASSERT_TRUE(packets);
    ASSERT_EQ(packets->size(), 2u);
    EXPECT_FALSE((*packets)[0].reply);
    EXPECT_EQ((*packets)[0].id, 1u);
    EXPECT_TRUE((*packets)[1].reply);
    EXPECT_EQ((*packets)[1].label, "version");
}

TEST(MonoWire, RejectsMalformedAndUnexpectedReplies) {
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
    const std::array<std::uint8_t, 11> unknown{0, 0, 0, 11, 0, 0, 0, 8, 0x80, 0, 0};
    EXPECT_FALSE(wire.feed(unknown));
    EXPECT_FALSE(wire.ready());
    EXPECT_FALSE(wire.version("after close"));
    Wire bad;
    EXPECT_FALSE(bad.feed(bytes("bad")));
    EXPECT_FALSE(bad.ready());
}

TEST(MonoWire, NegotiationEncodingAndPendingLimit) {
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
    auto version = wire.negotiate258("protocol");
    ASSERT_TRUE(version);
    EXPECT_EQ(*version, (std::vector<std::uint8_t>{0, 0, 0, 19, 0, 0, 0, 1, 0, 1, 8, 0, 0, 0, 2, 0, 0, 0, 58}));
    for (std::size_t i = 1; i < Wire::maxPending; ++i)
        ASSERT_TRUE(wire.version("pending"));
    EXPECT_FALSE(wire.dispose("overflow"));
    const std::array<std::uint8_t, 11> firstReply{0, 0, 0, 11, 0, 0, 0, 1, 0x80, 0, 0};
    ASSERT_TRUE(wire.feed(firstReply));
    EXPECT_TRUE(wire.dispose("capacity restored"));
}

TEST(MonoWire, FragmentedHeaderAndPayloadAreRetained) {
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
    ASSERT_TRUE(wire.version("version"));
    const std::array<std::uint8_t, 14> reply{0, 0, 0, 14, 0, 0, 0, 1, 0x80, 0, 0, 4, 5, 6};
    auto first = wire.feed(std::span(reply).first(2));
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->empty());
    auto second = wire.feed(std::span(reply).subspan(2, 10));
    ASSERT_TRUE(second);
    EXPECT_TRUE(second->empty());
    auto last = wire.feed(std::span(reply).subspan(12));
    ASSERT_TRUE(last);
    ASSERT_EQ(last->size(), 1u);
    EXPECT_EQ((*last)[0].payload, (std::vector<std::uint8_t>{4, 5, 6}));
    EXPECT_FALSE(wire.feed(reply)); // A duplicate reply permanently closes the connection.
    EXPECT_FALSE(wire.ready());
    EXPECT_FALSE(wire.feed({}));
}

TEST(MonoWire, RejectsInvalidLengthsFlagsAndCommands) {
    const std::array<std::array<std::uint8_t, 11>, 4> invalid{{
        {0, 0, 0, 10, 0, 0, 0, 1, 0, 64, 100},
        {0, 16, 0, 1, 0, 0, 0, 1, 0, 64, 100},
        {0, 0, 0, 11, 0, 0, 0, 1, 1, 64, 100},
        {0, 0, 0, 11, 0, 0, 0, 1, 0, 64, 99},
    }};
    for (const auto& packet : invalid) {
        Wire wire;
        ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
        EXPECT_FALSE(wire.feed(packet));
        EXPECT_FALSE(wire.ready());
        EXPECT_FALSE(wire.feed({}));
    }
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
    const std::vector<std::uint8_t> tooLarge(Wire::maxPacket + sizeof(Wire::handshake), 0);
    EXPECT_FALSE(wire.feed(tooLarge));
    EXPECT_FALSE(wire.ready());
}

TEST(MonoWire, OwnedOracleCommandShapes) {
    Wire wire;
    ASSERT_TRUE(wire.feed(bytes(Wire::handshake)));
    auto bp = wire.breakpoint(2, 4, "breakpoint");
    ASSERT_TRUE(bp);
    EXPECT_EQ(*bp, (std::vector<std::uint8_t>{0, 0, 0, 27, 0, 0, 0, 1, 0, 15, 1, 10, 2, 1,
                                              7, 0, 0, 0,  2, 0, 0, 0, 0, 0,  0, 0,  4}));
    auto frames = wire.frames(1, "frames");
    ASSERT_TRUE(frames);
    EXPECT_EQ(*frames, (std::vector<std::uint8_t>{0, 0, 0, 23, 0, 0, 0, 2,   0,   11,  1,  0,
                                                  0, 0, 1, 0,  0, 0, 0, 255, 255, 255, 255}));
    EXPECT_FALSE(wire.breakpoint(0, 0, "bad"));
    EXPECT_FALSE(wire.clearEvent(1, 1, "bad"));
    EXPECT_FALSE(wire.local(0, 0, 0, "bad"));
    auto image = wire.metadata(MetadataQuery::AssemblyImage, 1, "image");
    ASSERT_TRUE(image);
    EXPECT_EQ((*image)[9], 21);
    EXPECT_EQ((*image)[10], 8);
    auto pdb = wire.metadata(MetadataQuery::AssemblyPdb, 1, "pdb");
    ASSERT_TRUE(pdb);
    EXPECT_EQ((*pdb)[9], 21);
    EXPECT_EQ((*pdb)[10], 10);
    auto method = wire.methodFromToken(0x01020304, 0x0600002a, "method");
    ASSERT_TRUE(method);
    EXPECT_EQ(*method, (std::vector<std::uint8_t>{0, 0, 0, 19, 0, 0, 0, 5, 0, 21, 12, 1, 2, 3, 4, 6, 0, 0, 42}));
    EXPECT_FALSE(wire.methodFromToken(0, 0x06000001, "bad"));
    EXPECT_FALSE(wire.methodFromToken(1, 0x06000000, "bad"));
    EXPECT_FALSE(wire.methodFromToken(1, 0x02000001, "bad"));
    auto domain = wire.rootDomain("root");
    ASSERT_TRUE(domain);
    EXPECT_EQ(*domain, (std::vector<std::uint8_t>{0, 0, 0, 11, 0, 0, 0, 6, 0, 20, 1}));
    auto assemblies = wire.domainAssemblies(0x01020304, "assemblies");
    ASSERT_TRUE(assemblies);
    EXPECT_EQ(*assemblies, (std::vector<std::uint8_t>{0, 0, 0, 15, 0, 0, 0, 7, 0, 20, 3, 1, 2, 3, 4}));
    EXPECT_FALSE(wire.domainAssemblies(0, "bad"));
    auto threads = wire.allThreads("threads");
    ASSERT_TRUE(threads);
    EXPECT_EQ(*threads, (std::vector<std::uint8_t>{0, 0, 0, 11, 0, 0, 0, 8, 0, 1, 2}));
}
} // namespace
} // namespace frost::debug::mono
