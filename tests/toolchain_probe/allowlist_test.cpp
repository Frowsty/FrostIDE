// SPDX-License-Identifier: Apache-2.0
//
// toolchain_probe: every C++26 feature on the allowlist (PLAN.md section 2.1,
// CLAUDE.md) compiles under our flags and behaves as specified. If a compiler
// upgrade drops one, this file stops compiling and the upgrade is blocked.

#include <gtest/gtest.h>

#include <string_view>
#include <tuple>
#include <utility>

namespace {

// The feature-test macros promise what the tests below use.
static_assert(__cpp_pack_indexing >= 202311L, "pack indexing (P2662)");
static_assert(__cpp_placeholder_variables >= 202306L, "_ placeholder variables (P2169)");
static_assert(__cpp_deleted_function >= 202403L, "= delete(\"reason\") (P2573)");
static_assert(__cpp_static_assert >= 202306L, "user-generated static_assert messages (P2741)");

// Pack indexing: types and values.
template <typename... Ts>
using First = Ts...[0];
template <auto... Vs>
constexpr auto kSecond = Vs...[1];

TEST(ToolchainProbe, PackIndexing) {
    static_assert(std::is_same_v<First<int, char, double>, int>);
    EXPECT_EQ((kSecond<1, 2, 3>), 2);
}

// Counts its own destruction, to show that each `_` is a distinct object.
struct Counted {
    int* destroyed;
    ~Counted() { ++*destroyed; }
};

TEST(ToolchainProbe, PlaceholderVariables) {
    int destroyed = 0;
    {
        // Several `_` in one scope are allowed, including in structured bindings.
        Counted _{&destroyed};
        Counted _{&destroyed};
        auto [_, second] = std::pair{1, 2};
        EXPECT_EQ(second, 2);
    }
    EXPECT_EQ(destroyed, 2);
}

// = delete with a reason.
int takesLong(long value) { return static_cast<int>(value); }
int takesLong(int) = delete("pass a long to avoid the implicit conversion");

TEST(ToolchainProbe, DeletedFunctionWithReason) {
    EXPECT_EQ(takesLong(5L), 5);
    // takesLong(5) would fail with the reason above.
}

// User-generated static_assert messages: any constant expression with data() and size().
struct Message {
    std::string_view text;
    [[nodiscard]] constexpr const char* data() const { return text.data(); }
    [[nodiscard]] constexpr std::size_t size() const { return text.size(); }
};
static_assert(sizeof(int) >= 4, Message{"FrostIDE assumes 32-bit int"});

TEST(ToolchainProbe, AttributesOnStructuredBindings) {
    auto [first [[maybe_unused]], second] = std::tuple{1, 2};
    EXPECT_EQ(second, 2);
}

} // namespace
