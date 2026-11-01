// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the frost::project tests: absolute paths into tests/project/fixtures, and a MemFs
// tree under a native root for in-memory solutions. Header-only; test code only.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace frost::project::test {

// `relative` ('/'-separated) turned into native separators.
inline std::string native(std::string_view relative) {
    std::string out(relative);
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        for (char& c : out) {
            if (c == '/') {
                c = '\\';
            }
        }
    }
    return out;
}

// The absolute, normalised path of tests/project/fixtures/<relative>.
inline std::string fixture(std::string_view relative) {
    auto base = io::normalizePath(FROST_PROJECT_FIXTURE_DIR);
    EXPECT_TRUE(base.has_value()) << FROST_PROJECT_FIXTURE_DIR;
    if (!base || relative.empty()) {
        return base ? *base : std::string();
    }
    auto joined = io::joinPath(*base, native(relative));
    EXPECT_TRUE(joined.has_value());
    auto normalized = joined ? io::normalizePath(*joined) : joined;
    return normalized ? *normalized : std::string();
}

// The SDK-shaped solution: tests/project/fixtures/sdk_solution/<relative>.
inline std::string sdk(std::string_view relative = {}) {
    return relative.empty() ? fixture("sdk_solution") : fixture(std::string("sdk_solution/") + std::string(relative));
}

// An in-memory file system rooted at "C:\w" (Windows) or "/w" (POSIX).
class MemTree {
public:
    MemTree() { EXPECT_TRUE(fs.createDirectories(root()).has_value()); }

    [[nodiscard]] static std::string root() {
        return io::kNativePathStyle == io::PathStyle::Windows ? std::string("C:\\w") : std::string("/w");
    }

    // root()/<relative> with native separators.
    [[nodiscard]] static std::string path(std::string_view relative) {
        return root() + (io::kNativePathStyle == io::PathStyle::Windows ? "\\" : "/") + native(relative);
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

} // namespace frost::project::test
