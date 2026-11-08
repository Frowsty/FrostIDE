// The doctest 2.5.3 program whose real output the RUN-8 parser fixtures beside it are (not built here).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <stdexcept>

TEST_SUITE("Math") {
    TEST_CASE("adds") {
        CHECK(1 + 1 == 2);
    }
    TEST_CASE("compares, with a comma") {
        int a = 1;
        CHECK(a == 2);
        CHECK(a == 1);
    }
    TEST_CASE("requires") {
        REQUIRE(2 * 2 == 5);
    }
    TEST_CASE("has subcases") {
        SUBCASE("first") { CHECK(true); }
        SUBCASE("second") {
            INFO("the value is ", 7);
            CHECK(7 == 8);
        }
    }
    TEST_CASE("throws") {
        throw std::runtime_error("boom");
    }
    TEST_CASE("messages") {
        MESSAGE("just a note");
        FAIL_CHECK("a failure message");
    }
    TEST_CASE("is skipped" * doctest::skip()) {
        CHECK(false);
    }
    TEST_CASE("may fail" * doctest::may_fail()) {
        CHECK(false);
    }
}

TEST_CASE("outside every suite") {
    CHECK(true);
}

TEST_SUITE("Exit") {
    TEST_CASE("passes first") {
        CHECK(true);
    }
    TEST_CASE("exits the process") {
        std::_Exit(3);
    }
    TEST_CASE("never runs") {
        CHECK(true);
    }
}
