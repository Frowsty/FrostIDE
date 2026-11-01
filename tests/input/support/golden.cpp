// SPDX-License-Identifier: Apache-2.0
//
// Golden-file comparison (golden.hpp).

#include "input/support/golden.hpp"

#include "sched/sched_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace frost::input::test {

namespace {

std::filesystem::path goldenPath(std::string_view name) {
    return std::filesystem::path(FROST_INPUT_GOLDEN_DIR) /
           std::filesystem::path(std::u8string(name.begin(), name.end()));
}

// The text of the line of `s` that starts at byte `start`.
std::string lineAt(std::string_view s, std::size_t start) {
    if (start >= s.size()) {
        return "<end>";
    }
    const std::size_t end = s.find('\n', start);
    return std::string(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

// The 1-based line of the first difference, with both lines' text.
std::string firstDifference(std::string_view expected, std::string_view actual) {
    std::size_t line = 1;
    std::size_t start = 0;
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            break;
        }
        if (expected[i] == '\n') {
            ++line;
            start = i + 1;
        }
    }
    std::ostringstream out;
    out << "first difference at line " << line << "\n  golden: " << lineAt(expected, start)
        << "\n  actual: " << lineAt(actual, start);
    return out.str();
}

} // namespace

void expectGolden(std::string_view name, std::string_view actual) {
    const std::filesystem::path path = goldenPath(name);
    if (frost::sched::test::environment("FROST_UPDATE_GOLDENS") == "1") {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(actual.data(), static_cast<std::streamsize>(actual.size()));
        ADD_FAILURE() << "golden updated: review the diff (" << path.string() << ")";
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "missing golden " << path.string() << "; run with FROST_UPDATE_GOLDENS=1 and review it";
        return;
    }
    const std::string expected((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (expected != actual) {
        ADD_FAILURE() << "golden " << name << " differs; " << firstDifference(expected, actual);
    }
}

} // namespace frost::input::test
