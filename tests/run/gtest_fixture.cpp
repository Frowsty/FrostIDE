// SPDX-License-Identifier: Apache-2.0
//
// run_gtest_fixture: a Google Test program for the test runner tests (RUN-7), not a test itself. The
// ctest fixture (fixtures/ctest_project) runs it whole and as one filtered case; the tests compare
// what FrostIDE parses from its list and JSON reports with its own XML report (the oracle). It holds
// a pass, a failure, a skip, a disabled case, a value-parameterized and a typed suite.

#include <gtest/gtest.h>

#include <cstdint>

namespace {

TEST(Fixture, Passes) { EXPECT_EQ(1 + 1, 2); }

TEST(Fixture, Fails) {
    // The tests read this failure's line from the XML oracle, not from here.
    EXPECT_EQ(2 + 2, 5) << "an intended failure";
}

TEST(Fixture, Skips) { GTEST_SKIP() << "an intended skip"; }

TEST(Fixture, DISABLED_Off) { FAIL() << "a disabled case never runs"; }

class Numbers : public testing::TestWithParam<int> {};

TEST_P(Numbers, AreSmall) { EXPECT_LT(GetParam(), 10); }

INSTANTIATE_TEST_SUITE_P(Small, Numbers, testing::Values(1, 2));

template<typename T>
class Sizes : public testing::Test {};

using SizeTypes = testing::Types<std::uint8_t, std::uint32_t>;
TYPED_TEST_SUITE(Sizes, SizeTypes);

TYPED_TEST(Sizes, AreAtLeastOneByte) { EXPECT_GE(sizeof(TypeParam), 1U); }

} // namespace
