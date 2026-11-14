// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <fstream>
TEST_CASE("passes", "[ordinary]") { REQUIRE(2 + 2 == 4); }
TEST_CASE("fails", "[failure]") { REQUIRE(2 + 2 == 5); }
TEST_CASE("skips", "[skip]") { SKIP("fixture requests a skip"); }
TEST_CASE("mixed sections", "[mixed]") {
    SECTION("failure") { CHECK(false); }
    SECTION("skip") { SKIP("another section skips"); }
}

TEST_CASE("slow", "[.slow]") {
    if (const char* ready = std::getenv("FROST_FRAMEWORK_READY")) { std::ofstream signal(ready); signal << "running"; }
    std::this_thread::sleep_for(std::chrono::seconds(30)); REQUIRE(true);
}
