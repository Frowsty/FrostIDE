// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the frost::build unit tests: native absolute paths under a fixed root, a MemFs
// tree to write small solutions into and a minimal PE image. Header-only; test code only.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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
        EXPECT_TRUE(fs.createDirectories(io::parentPath(full)).has_value()) << full;
        EXPECT_TRUE(fs.writeFileAtomic(full, content).has_value()) << full;
    }

    ManualClock clock;
    io::MemFs fs{clock};
};

// The headers of a PE image (MZ, e_lfanew = 0x40, "PE\0\0", a file header, an optional header)
// declaring `subsystem` (3 console, 2 GUI): enough for build::peSubsystem.
inline std::string peImage(std::uint16_t subsystem) {
    std::string image(0x40 + 4 + 20 + 96, '\0');
    image[0] = 'M';
    image[1] = 'Z';
    image[0x3C] = 0x40;
    image.replace(0x40, 4, std::string("PE\0\0", 4));
    image[0x40 + 4 + 20 + 68] = static_cast<char>(subsystem & 0xFF);
    return image;
}

} // namespace frost::build::test
