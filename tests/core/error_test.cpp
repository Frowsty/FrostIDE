// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::Error and frost::Result: construction, context chaining,
// formatting, and the expected-based propagation pattern used across modules.

#include "frost/core/error.hpp"

#include <gtest/gtest.h>

#include <format>
#include <string>

namespace {

frost::Result<int> parsePositive(int value) {
    if (value <= 0) {
        return frost::fail(frost::ErrorCode::InvalidArgument, std::format("{} is not positive", value));
    }
    return value;
}

// Propagates an error outward, adding context the way callers are expected to.
frost::Result<int> loadSetting(int raw) {
    auto parsed = parsePositive(raw);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()).withContext("while loading setting 'tabSize'"));
    }
    return *parsed * 2;
}

TEST(Error, CarriesCodeMessageAndLocation) {
    const frost::Error error(frost::ErrorCode::NotFound, "no such file");
    EXPECT_EQ(error.code(), frost::ErrorCode::NotFound);
    EXPECT_EQ(error.message(), "no such file");
    EXPECT_GT(error.where().line(), 0u);
    EXPECT_NE(std::string(error.where().file_name()).find("error_test.cpp"), std::string::npos);
}

TEST(Error, ResultSuccessAndFailure) {
    EXPECT_EQ(loadSetting(4).value(), 8);
    const auto failed = loadSetting(-1);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), frost::ErrorCode::InvalidArgument);
    ASSERT_EQ(failed.error().context().size(), 1u);
    EXPECT_EQ(failed.error().context()[0], "while loading setting 'tabSize'");
}

TEST(Error, DescribeIncludesCodeMessageContextAndFile) {
    const auto failed = loadSetting(0);
    const std::string text = failed.error().describe();
    EXPECT_NE(text.find("InvalidArgument: 0 is not positive"), std::string::npos) << text;
    EXPECT_NE(text.find("[while loading setting 'tabSize']"), std::string::npos) << text;
    EXPECT_NE(text.find("(error_test.cpp:"), std::string::npos) << text;
    // Only the file name is kept, never the build machine's directory.
    EXPECT_EQ(text.find('/'), std::string::npos) << text;
    EXPECT_EQ(text.find('\\'), std::string::npos) << text;
}

TEST(Error, FormatsThroughStdFormat) {
    const frost::Error error(frost::ErrorCode::Timeout, "clangd did not answer");
    EXPECT_EQ(std::format("{}", error), error.describe());
}

TEST(Error, ToStringCoversEveryCode) {
    EXPECT_EQ(frost::toString(frost::ErrorCode::Cancelled), "Cancelled");
    EXPECT_EQ(frost::toString(frost::ErrorCode::Internal), "Internal");
    EXPECT_EQ(frost::toString(static_cast<frost::ErrorCode>(999)), "ErrorCode(?)");
}

TEST(Error, EqualityComparesCodeAndMessage) {
    EXPECT_EQ(frost::Error(frost::ErrorCode::Busy, "x"), frost::Error(frost::ErrorCode::Busy, "x"));
    EXPECT_FALSE(frost::Error(frost::ErrorCode::Busy, "x") == frost::Error(frost::ErrorCode::Busy, "y"));
}

} // namespace
