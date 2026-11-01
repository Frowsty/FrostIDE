// SPDX-License-Identifier: Apache-2.0
//
// Tests for the strong index types: arithmetic, ordering, hashing, formatting,
// and (at compile time) that distinct index types do not mix.

#include "frost/core/strong_type.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <format>
#include <unordered_set>
#include <vector>

namespace {

TEST(StrongIndex, MovesByCountAndMeasuresDistance) {
    frost::ByteOffset a{10};
    const frost::ByteOffset b = a + 5;
    EXPECT_EQ(b.value(), 15u);
    EXPECT_EQ(b - a, 5u);
    a += 3;
    ++a;
    EXPECT_EQ(a.value(), 14u);
    EXPECT_EQ((b - 1).value(), 14u);
}

TEST(StrongIndex, OrdersAndHashes) {
    EXPECT_LT(frost::LineIndex{1}, frost::LineIndex{2});
    EXPECT_EQ(frost::LineIndex{3}, frost::LineIndex{3});
    const std::unordered_set<frost::Utf16Offset> seen{frost::Utf16Offset{1}, frost::Utf16Offset{1},
                                                      frost::Utf16Offset{2}};
    EXPECT_EQ(seen.size(), 2u);
}

TEST(StrongIndex, Formats) { EXPECT_EQ(std::format("{}", frost::ByteColumn{42}), "42"); }

TEST(StrongIndex, DistinctTypesDoNotMix) {
    static_assert(!std::convertible_to<frost::ByteOffset, frost::Utf16Offset>);
    static_assert(!std::convertible_to<std::uint64_t, frost::ByteOffset>);
    static_assert(!std::equality_comparable_with<frost::ByteOffset, frost::Utf16Offset>);
    static_assert(sizeof(frost::LineIndex) == sizeof(std::uint32_t));
}

TEST(StrongIndex, Utf16ColumnIsItsOwnType) {
    // A UTF-16 column (LSP character offset) never mixes with a byte column or a UTF-16 offset.
    static_assert(!std::convertible_to<frost::Utf16Column, frost::ByteColumn>);
    static_assert(!std::equality_comparable_with<frost::Utf16Column, frost::ByteColumn>);
    static_assert(!std::equality_comparable_with<frost::Utf16Column, frost::Utf16Offset>);
    static_assert(sizeof(frost::Utf16Column) == sizeof(std::uint32_t));
    frost::Utf16Column column{3};
    column += 2;
    EXPECT_EQ(column.value(), 5u);
    EXPECT_EQ(column - frost::Utf16Column{1}, 4u);
    EXPECT_EQ(std::format("{}", column), "5");
}

TEST(StrongIndex, IsFormattableInsideContainers) {
    static_assert(std::formattable<frost::ByteOffset, char>);
#if defined(__cpp_lib_format_ranges)
    EXPECT_EQ(std::format("{}", std::vector{frost::ByteOffset{1}, frost::ByteOffset{2}}), "[1, 2]");
#endif // libstdc++ 14 has no range formatting; the concept check above still runs.
}

} // namespace
