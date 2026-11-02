// SPDX-License-Identifier: Apache-2.0
// Regression for bounded PTY output storage when the consumer never empties it.
#include "../../src/term/src/pty_buffer.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>

TEST(PtyOutputBuffer, ReclaimsConsumedPrefixWhileBacklogNeverEmpties) {
    frost::term::detail::OutputBuffer buffer;
    ASSERT_TRUE(buffer.append("x")); // Keep one unread byte throughout.
    const std::string block(4096, 'a');
    std::array<char, 4096> out{};
    for (int iteration = 0; iteration < 1024; ++iteration) {
        ASSERT_TRUE(buffer.append(block));
        ASSERT_EQ(buffer.take(out), out.size());
        EXPECT_EQ(out.front(), iteration == 0 ? 'x' : 'a');
        EXPECT_TRUE(std::all_of(out.begin() + 1, out.end(), [](char byte) { return byte == 'a'; }));
        EXPECT_LT(buffer.retainedCapacityForTesting(), 256U * 1024U);
    }
    buffer.finish();
    EXPECT_EQ(buffer.take(out), 1U);
    EXPECT_EQ(buffer.take(out), 0U);
}
