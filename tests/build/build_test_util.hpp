// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the frost::build unit tests: native absolute paths under a fixed root and a MemFs
// tree to write small solutions into. Header-only; test code only.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace frost::build::test {

inline constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;

// `relative` ('/'-separated) with native separators.
inline std::string native(std::string_view relative) {
    std::string out(relative);
    if constexpr (kWindows) {
        for (char& c : out) {
            if (c == '/') {
                c = '\\';
            }
        }
    }
    return out;
}

// An in-memory file system rooted at "C:\work" (Windows) or "/work" (POSIX).
class MemTree {
public:
    MemTree() { EXPECT_TRUE(fs.createDirectories(root()).has_value()); }

    [[nodiscard]] static std::string root() { return kWindows ? std::string("C:\\work") : std::string("/work"); }

    // root()/<relative> with native separators.
    [[nodiscard]] static std::string path(std::string_view relative) {
        return root() + (kWindows ? "\\" : "/") + native(relative);
    }

    // Writes root()/<relative>, creating its directories.
    void write(std::string_view relative, std::string_view content) {
        const std::string full = path(relative);
        EXPECT_TRUE(fs.createDirectories(std::string(io::parentPath(full))).has_value()) << full;
        EXPECT_TRUE(fs.writeFileAtomic(full, content).has_value()) << full;
    }

    ManualClock clock;
    io::MemFs fs{clock};
};

} // namespace frost::build::test
