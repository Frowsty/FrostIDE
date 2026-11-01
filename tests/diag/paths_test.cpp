// SPDX-License-Identifier: Apache-2.0
//
// The diagnostics cache root and layout (frost/diag/paths.hpp): resolver tables for both
// path styles (unset, empty, relative, device and UNC values of LOCALAPPDATA; XDG_CACHE_HOME
// and HOME on POSIX) and the directory layout below the root.

#include "frost/diag/paths.hpp"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

namespace {

using frost::ErrorCode;
using frost::io::PathStyle;

frost::diag::EnvironmentLookup lookupIn(std::map<std::string, std::string> variables) {
    return [variables = std::move(variables)](std::string_view name) -> std::optional<std::string> {
        const auto it = variables.find(std::string(name));
        return it == variables.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

struct Row {
    std::map<std::string, std::string> environment;
    std::string expected; // The root, or "" for an error.
    ErrorCode error = ErrorCode::Unknown;
};

TEST(DiagnosticsPaths, WindowsCacheRootTable) {
    const Row rows[] = {
        {{{"LOCALAPPDATA", "C:\\Users\\a\\AppData\\Local"}}, "C:\\Users\\a\\AppData\\Local\\FrostIDE\\caches"},
        // Forward slashes, a trailing separator and the \\?\ form are accepted (normalised by
        // frost::io; only the appended part is pinned here).
        {{{"LOCALAPPDATA", "c:/Users/a/AppData/Local/"}}, "*Users\\a\\AppData\\Local\\FrostIDE\\caches"},
        {{{"LOCALAPPDATA", "\\\\server\\share\\a"}}, "\\\\server\\share\\a\\FrostIDE\\caches"},
        {{{"LOCALAPPDATA", "\\\\?\\C:\\Users\\a"}}, "*Users\\a\\FrostIDE\\caches"},
        {{}, "", ErrorCode::NotFound},
        {{{"LOCALAPPDATA", ""}}, "", ErrorCode::NotFound},
        {{{"LOCALAPPDATA", "AppData\\Local"}}, "", ErrorCode::InvalidArgument},
        {{{"LOCALAPPDATA", "C:AppData"}}, "", ErrorCode::InvalidArgument},
        {{{"LOCALAPPDATA", "\\Users\\a"}}, "", ErrorCode::InvalidArgument},
        {{{"LOCALAPPDATA", "\\\\.\\C:\\Users"}}, "", ErrorCode::InvalidArgument},
        {{{"LOCALAPPDATA", std::string("C:\\a\0b", 6)}}, "", ErrorCode::InvalidArgument},
    };
    for (const Row& row : rows) {
        const auto root = frost::diag::resolveCacheRoot(lookupIn(row.environment), PathStyle::Windows);
        const std::string value =
            row.environment.contains("LOCALAPPDATA") ? row.environment.at("LOCALAPPDATA") : "<unset>";
        if (row.expected.empty()) {
            ASSERT_FALSE(root.has_value()) << value << " -> " << *root;
            EXPECT_EQ(root.error().code(), row.error) << value;
        } else if (row.expected.starts_with("*")) {
            ASSERT_TRUE(root.has_value()) << value << ": " << root.error().describe();
            EXPECT_TRUE(root->ends_with(row.expected.substr(1))) << value << " -> " << *root;
        } else {
            ASSERT_TRUE(root.has_value()) << value << ": " << root.error().describe();
            EXPECT_EQ(*root, row.expected) << value;
        }
    }
}

TEST(DiagnosticsPaths, PosixCacheRootTable) {
    const Row rows[] = {
        {{{"XDG_CACHE_HOME", "/home/a/.cache-x"}, {"HOME", "/home/a"}}, "/home/a/.cache-x/FrostIDE"},
        {{{"XDG_CACHE_HOME", "relative"}, {"HOME", "/home/a"}}, "/home/a/.cache/FrostIDE"}, // XDG: ignored.
        {{{"XDG_CACHE_HOME", ""}, {"HOME", "/home/a/"}}, "/home/a/.cache/FrostIDE"},
        {{{"HOME", "/home/a"}}, "/home/a/.cache/FrostIDE"},
        {{{"HOME", "home/a"}}, "", ErrorCode::InvalidArgument},
        {{}, "", ErrorCode::NotFound},
        {{{"XDG_CACHE_HOME", "relative"}}, "", ErrorCode::NotFound},
    };
    for (const Row& row : rows) {
        const auto root = frost::diag::resolveCacheRoot(lookupIn(row.environment), PathStyle::Posix);
        if (row.expected.empty()) {
            ASSERT_FALSE(root.has_value()) << *root;
            EXPECT_EQ(root.error().code(), row.error);
        } else {
            ASSERT_TRUE(root.has_value()) << root.error().describe();
            EXPECT_EQ(*root, row.expected);
        }
    }
}

TEST(DiagnosticsPaths, LayoutBelowTheRoot) {
    const auto posix = frost::diag::diagnosticsDirectories("/c/FrostIDE", PathStyle::Posix);
    ASSERT_TRUE(posix.has_value());
    EXPECT_EQ(posix->crashes, "/c/FrostIDE/crashes");
    EXPECT_EQ(posix->crashpadDatabase, "/c/FrostIDE/crashes/crashpad");
    EXPECT_EQ(posix->freezes, "/c/FrostIDE/crashes/freezes");
    EXPECT_EQ(posix->logs, "/c/FrostIDE/logs");
    EXPECT_EQ(posix->traces, "/c/FrostIDE/traces");
    const auto windows = frost::diag::diagnosticsDirectories("C:\\L\\FrostIDE\\caches", PathStyle::Windows);
    ASSERT_TRUE(windows.has_value());
    EXPECT_EQ(windows->crashpadDatabase, "C:\\L\\FrostIDE\\caches\\crashes\\crashpad");
    EXPECT_FALSE(frost::diag::diagnosticsDirectories("relative", PathStyle::Posix).has_value());
}

} // namespace
