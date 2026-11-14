// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/cmake_dap.hpp"
#include <gtest/gtest.h>
namespace frost::debug {
TEST(CMakeDap, InvalidEndpointNeverConnectsAndCancelsOwnedConfigure) {
    int cancelled = 0;
    auto factory = cmakeDapFactory("tcp://127.0.0.1:1234", [&] { ++cancelled; });
    auto client = factory({});
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(cancelled, 1);
}
TEST(CMakeDap, RefusesEmbeddedNulAndNonpositiveDeadline) {
    auto factory = cmakeDapFactory(std::string("/private/dap\0hidden", 19), {}, std::chrono::milliseconds(0));
    auto client = factory({});
    ASSERT_FALSE(client);
    EXPECT_EQ(client.error().code(), ErrorCode::InvalidArgument);
}
} // namespace frost::debug
