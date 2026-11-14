// SPDX-License-Identifier: Apache-2.0
#define BOOST_TEST_MODULE FrostFrameworkFixture
#include <boost/test/included/unit_test.hpp>
BOOST_AUTO_TEST_CASE(passes) { BOOST_TEST(2 + 2 == 4); }
BOOST_AUTO_TEST_CASE(fails, * boost::unit_test::disabled()) { BOOST_TEST(2 + 2 == 5); }
BOOST_AUTO_TEST_SUITE(nested)
BOOST_AUTO_TEST_CASE(value) { BOOST_TEST(7 > 0); }
BOOST_AUTO_TEST_SUITE_END()
