// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::add_sat, sub_sat, mul_sat and saturate_cast at the edges of
// signed and unsigned ranges.

#include "frost/compat/saturate.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

TEST(Saturate, AddClampsUnsigned) {
    EXPECT_EQ(frost::add_sat<std::uint8_t>(200, 100), 255);
    EXPECT_EQ(frost::add_sat<std::uint8_t>(100, 100), 200);
}

TEST(Saturate, AddClampsSignedBothWays) {
    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    constexpr auto min = std::numeric_limits<std::int32_t>::min();
    EXPECT_EQ(frost::add_sat<std::int32_t>(max, 1), max);
    EXPECT_EQ(frost::add_sat<std::int32_t>(min, -1), min);
    EXPECT_EQ(frost::add_sat<std::int32_t>(-5, 3), -2);
}

TEST(Saturate, SubClamps) {
    EXPECT_EQ(frost::sub_sat<std::uint32_t>(3, 5), 0u);
    EXPECT_EQ(frost::sub_sat<std::int8_t>(-100, 100), -128);
    EXPECT_EQ(frost::sub_sat<std::int8_t>(100, -100), 127);
}

TEST(Saturate, MulClampsBySign) {
    EXPECT_EQ(frost::mul_sat<std::int16_t>(300, 300), 32767);
    EXPECT_EQ(frost::mul_sat<std::int16_t>(-300, 300), -32768);
    EXPECT_EQ(frost::mul_sat<std::uint16_t>(300, 300), 65535);
    EXPECT_EQ(frost::mul_sat<std::int16_t>(-3, 4), -12);
}

TEST(Saturate, CastClampsToDestinationRange) {
    EXPECT_EQ(frost::saturate_cast<std::uint8_t>(-1), 0);
    EXPECT_EQ(frost::saturate_cast<std::uint8_t>(1000), 255);
    EXPECT_EQ(frost::saturate_cast<std::int8_t>(std::uint64_t{200}), 127);
    EXPECT_EQ(frost::saturate_cast<std::int64_t>(std::uint64_t{42}), 42);
}

TEST(Saturate, UsableInConstantExpressions) {
    static_assert(frost::add_sat<std::uint8_t>(250, 10) == 255);
    static_assert(frost::saturate_cast<std::uint16_t>(-7) == 0);
}

} // namespace
