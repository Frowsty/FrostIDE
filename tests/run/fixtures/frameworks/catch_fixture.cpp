// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
TEST_CASE("passes", "[ordinary]") { REQUIRE(2 + 2 == 4); }
TEST_CASE("fails", "[failure]") { REQUIRE(2 + 2 == 5); }
TEST_CASE("skips", "[skip]") { SKIP("fixture requests a skip"); }
TEST_CASE("mixed sections", "[mixed]") {
    SECTION("failure") { CHECK(false); }
    SECTION("skip") { SKIP("another section skips"); }
}

TEST_CASE("slow", "[.slow]") {
#ifdef _WIN32
    wchar_t* value = nullptr;
    std::size_t length = 0;
    REQUIRE(_wdupenv_s(&value, &length, L"FROST_FRAMEWORK_READY") == 0);
    const std::filesystem::path ready(value ? value : L"");
    std::free(value);
#else
    const char* value = std::getenv("FROST_FRAMEWORK_READY");
    const std::filesystem::path ready(value ? value : "");
#endif
    if (!ready.empty()) {
        std::ofstream signal(ready);
        signal << "running";
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
    REQUIRE(true);
}
