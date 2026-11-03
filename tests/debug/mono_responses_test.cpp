// SPDX-License-Identifier: Apache-2.0
// Test bounded decoding of Mono replies and stop events.
#include "frost/debug/mono_responses.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace frost::debug::mono {
namespace {
TEST(MonoResponses, DomainAndAssemblyIdsAreBoundedAndExact) {
    EXPECT_EQ(*decodeId(std::vector<std::uint8_t>{0, 0, 0, 3}), 3U);
    EXPECT_FALSE(decodeId(std::vector<std::uint8_t>{0, 0, 0, 0}));
    EXPECT_FALSE(decodeId(std::vector<std::uint8_t>{0, 0, 0, 3, 0}));
    auto ids = decodeIds(std::vector<std::uint8_t>{0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4});
    ASSERT_TRUE(ids);
    EXPECT_EQ(*ids, (std::vector<std::uint32_t>{3, 4}));
    EXPECT_FALSE(decodeIds(std::vector<std::uint8_t>{0, 0, 0, 2, 0, 0, 0, 3}));
    EXPECT_FALSE(decodeIds(std::vector<std::uint8_t>{0, 0, 0, 1, 0, 0, 0, 0}));
    EXPECT_FALSE(decodeIds(std::vector<std::uint8_t>{0, 0, 0x40, 1}));
}

TEST(MonoResponses, OwnedBreakpointAndFrameTranscript) {
    const std::vector<std::uint8_t> event{2, 0, 0, 0, 1, 10, 0, 0, 0, 1, 0, 0, 0,
                                          1, 0, 0, 0, 2, 0,  0, 0, 0, 0, 0, 0, 0};
    const auto stop = decodeStop(event);
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->request, 1u);
    EXPECT_EQ(stop->method, 2u);
    EXPECT_EQ(stop->rawOffset, 0u);
    const std::vector<std::uint8_t> frame{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 4, 1};
    const auto frames = decodeFrames(frame);
    ASSERT_TRUE(frames);
    ASSERT_EQ(frames->size(), 1u);
    EXPECT_EQ((*frames)[0].id, 0u);
    EXPECT_EQ((*frames)[0].ilOffset, 4);
    EXPECT_TRUE(matchesBreakpoint(*stop, *frames, 1, 2, 4));
    EXPECT_FALSE(matchesBreakpoint(*stop, *frames, 2, 2, 4));
    EXPECT_FALSE(matchesBreakpoint(*stop, *frames, 1, 2, 5));
    auto badStop = *stop;
    badStop.thread = 0;
    EXPECT_FALSE(matchesBreakpoint(badStop, *frames, 1, 2, 4));
    EXPECT_FALSE(decodeFrames(std::span(frame).first(frame.size() - 1)));
}
TEST(MonoResponses, RejectsUnexpectedValuesAndBounds) {
    const std::vector<std::uint8_t> value{8, 0, 0, 0, 41};
    ASSERT_TRUE(decodeInt32(value));
    EXPECT_EQ(*decodeInt32(value), 41);
    const std::vector<std::uint8_t> bad{9, 0, 0, 0, 41};
    EXPECT_FALSE(decodeInt32(bad));
    const std::vector<std::uint8_t> huge{0, 0, 0, 129};
    EXPECT_FALSE(decodeFrames(huge));
}
TEST(MonoResponses, LoadedBlobMustExactlyMatchBoundedSnapshot) {
    const std::vector<std::uint8_t> snapshot{1, 2, 3};
    const std::vector<std::uint8_t> valid{0, 0, 0, 3, 1, 2, 3};
    EXPECT_TRUE(compareLoadedBlob(valid, snapshot));
    auto changed = valid;
    changed.back() = 4;
    EXPECT_FALSE(compareLoadedBlob(changed, snapshot));
    changed = valid;
    changed.pop_back();
    EXPECT_FALSE(compareLoadedBlob(changed, snapshot));
    changed = valid;
    changed.push_back(0);
    EXPECT_FALSE(compareLoadedBlob(changed, snapshot));
    EXPECT_FALSE(compareLoadedBlob(std::vector<std::uint8_t>{0, 0, 0, 0}, {}));
}
TEST(MonoResponses, DecodesBoundedSequencePoints) {
    std::vector<std::uint8_t> payload{0, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 1, 'a'};
    payload.insert(payload.end(), 16, 0);
    const std::vector<std::uint8_t> point{0, 0, 0, 1, 0, 0, 0, 4, 0, 0,  0, 13, 0, 0,
                                          0, 0, 0, 0, 0, 5, 0, 0, 0, 13, 0, 0,  0, 13};
    payload.insert(payload.end(), point.begin(), point.end());
    const auto info = decodeDebugInfo258(payload);
    ASSERT_TRUE(info);
    ASSERT_EQ(info->sources.size(), 1u);
    EXPECT_EQ(info->sources[0].path, "a");
    ASSERT_EQ(info->points.size(), 1u);
    EXPECT_EQ(info->points[0].ilOffset, 4);
    EXPECT_EQ(info->points[0].line, 13);
    payload.pop_back();
    EXPECT_FALSE(decodeDebugInfo258(payload));
}
TEST(MonoResponses, DecodesLocalsAndRejectsInvalidNamesAndScopes) {
    const std::vector<std::uint8_t> valid{0, 0, 0, 1, 0, 0, 0, 0,   0, 0, 0, 16, 0, 0, 0, 1, 0,
                                          0, 0, 9, 0, 0, 0, 1, 'v', 0, 0, 0, 0,  0, 0, 0, 16};
    const auto locals = decodeLocals258(valid);
    ASSERT_TRUE(locals);
    ASSERT_EQ(locals->size(), 1u);
    EXPECT_EQ((*locals)[0].name, "v");
    EXPECT_EQ((*locals)[0].start, 0);
    EXPECT_EQ((*locals)[0].end, 16);
    auto bad = valid;
    bad[24] = 0xff;
    EXPECT_FALSE(decodeLocals258(bad));
    bad = valid;
    bad.back() = 0;
    bad[28] = 1;
    EXPECT_FALSE(decodeLocals258(bad));
}
} // namespace
} // namespace frost::debug::mono
