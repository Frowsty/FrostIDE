// SPDX-License-Identifier: Apache-2.0
//
// Tests for LineSplitter and sanitizeUtf8 (frost/build/line_splitter.hpp).

#include "frost/build/line_splitter.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::build::LineSplitter;

struct Collector {
    std::vector<std::string> lines;
    LineSplitter::Sink sink() {
        return [this](std::string_view line) { lines.emplace_back(line); };
    }
};

TEST(LineSplitter, SplitsLfAndCrlfAcrossChunks) {
    LineSplitter splitter;
    Collector out;
    const auto sink = out.sink();
    splitter.feed("first\r\nsec", sink);
    splitter.feed("ond\n\nthi", sink);
    splitter.feed("rd\r", sink);
    splitter.feed("\nlast", sink);
    EXPECT_EQ(out.lines, (std::vector<std::string>{"first", "second", "", "third"}));
    splitter.finish(sink);
    EXPECT_EQ(out.lines.back(), "last");
    splitter.finish(sink); // Nothing left.
    EXPECT_EQ(out.lines.size(), 5U);
}

TEST(LineSplitter, ReplacesInvalidUtf8) {
    LineSplitter splitter;
    Collector out;
    const auto sink = out.sink();
    // "exp \x82 x": CP437's e-acute, as MSBuild writes it without a UTF-8 override.
    splitter.feed("exp \x82 x\n", sink);
    splitter.feed("caf\xC3", sink); // A UTF-8 sequence split across two chunks stays intact.
    splitter.feed("\xA9\n", sink);
    ASSERT_EQ(out.lines.size(), 2U);
    EXPECT_EQ(out.lines[0], "exp \xEF\xBF\xBD x");
    EXPECT_EQ(out.lines[1], "caf\xC3\xA9");
}

TEST(LineSplitter, CutsOverlongBufferedLines) {
    LineSplitter splitter;
    Collector out;
    const auto sink = out.sink();
    const std::string chunk(LineSplitter::kMaxLineBytes / 2 + 1, 'a');
    splitter.feed(chunk, sink);
    EXPECT_TRUE(out.lines.empty());
    splitter.feed(chunk, sink);
    ASSERT_EQ(out.lines.size(), 1U);
    EXPECT_EQ(out.lines[0].size(), LineSplitter::kMaxLineBytes);
    splitter.feed("\n", sink);
    ASSERT_EQ(out.lines.size(), 2U);
    EXPECT_EQ(out.lines[1].size(), 2 * chunk.size() - LineSplitter::kMaxLineBytes);
}

TEST(SanitizeUtf8, KeepsValidTextAndReplacesEachBadByte) {
    EXPECT_EQ(frost::build::sanitizeUtf8("plain ü"), "plain ü");
    EXPECT_EQ(frost::build::sanitizeUtf8("a\xFF\xFE"
                                         "b"),
              "a\xEF\xBF\xBD\xEF\xBF\xBD"
              "b");
    EXPECT_EQ(frost::build::sanitizeUtf8(""), "");
}

} // namespace
