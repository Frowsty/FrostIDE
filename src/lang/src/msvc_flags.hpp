// SPDX-License-Identifier: Apache-2.0
//
// Private helpers shared by the compile-command providers (compile_commands.cpp and
// msbuild_compile_commands.cpp): ASCII case and trimming, MSBuild ';' lists, paths relative to a
// project, and the MSVC metadata values mapped to clang-cl flags. Pure and thread-safe.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang::detail {

[[nodiscard]] inline char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// ';'-separated entries, trimmed, without empty ones and without unresolved $(...) / %(...) references.
[[nodiscard]] inline std::vector<std::string> resolvedEntries(std::string_view list) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = std::min(list.find(';', start), list.size());
        const std::string_view entry = trim(list.substr(start, end - start));
        if (!entry.empty() && entry.find("$(") == std::string_view::npos &&
            entry.find("%(") == std::string_view::npos && entry.find("@(") == std::string_view::npos) {
            out.emplace_back(entry);
        }
        start = end + 1;
    }
    return out;
}

// `entry` (absolute, or relative to `directory`, with '\' or '/') as a normalised absolute path.
[[nodiscard]] inline std::optional<std::string> resolvePath(std::string_view directory, std::string_view entry) {
    std::string text(entry);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Posix) {
        std::ranges::replace(text, '\\', '/');
    }
    while (text.size() > 1 && (text.back() == '\\' || text.back() == '/')) {
        text.pop_back();
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (io::isAbsolutePath(text)) {
        auto normalized = io::normalizePath(text);
        return normalized ? std::optional<std::string>(std::move(*normalized)) : std::nullopt;
    }
    auto joined = io::joinPath(directory, text);
    if (!joined) {
        return std::nullopt;
    }
    auto normalized = io::normalizePath(*joined);
    return normalized ? std::optional<std::string>(std::move(*normalized)) : std::nullopt;
}

[[nodiscard]] inline std::string metadataOf(const std::map<std::string, std::string, std::less<>>& metadata,
                                            std::string_view name) {
    auto it = metadata.find(name);
    return it != metadata.end() ? std::string(trim(it->second)) : std::string();
}

[[nodiscard]] inline std::string_view targetFor(std::string_view platform) noexcept {
    if (iequals(platform, "Win32") || iequals(platform, "x86")) {
        return "--target=i686-pc-windows-msvc";
    }
    if (iequals(platform, "ARM64") || iequals(platform, "ARM64EC")) {
        return "--target=aarch64-pc-windows-msvc";
    }
    if (iequals(platform, "ARM")) {
        return "--target=thumbv7-pc-windows-msvc";
    }
    return "--target=x86_64-pc-windows-msvc";
}

[[nodiscard]] inline std::string cxxStandard(std::string_view value) {
    const std::string v = lower(value);
    if (v == "stdcpp17") {
        return "/std:c++17";
    }
    if (v == "stdcpp20") {
        return "/std:c++20";
    }
    if (v == "stdcpplatest" || v == "stdcpp23") {
        return "/std:c++latest";
    }
    return "/std:c++14"; // stdcpp14 and MSVC's default.
}

[[nodiscard]] inline std::optional<std::string> cStandard(std::string_view value) {
    const std::string v = lower(value);
    if (v == "stdc11") {
        return "/std:c11";
    }
    if (v == "stdc17" || v == "stdclatest") {
        return "/std:c17";
    }
    return std::nullopt; // Default: clang's C default.
}

[[nodiscard]] inline std::string runtimeFlag(std::string_view value, std::string_view configuration) {
    const std::string v = lower(value);
    if (v == "multithreaded") {
        return "/MT";
    }
    if (v == "multithreadeddebug") {
        return "/MTd";
    }
    if (v == "multithreadeddll") {
        return "/MD";
    }
    if (v == "multithreadeddebugdll") {
        return "/MDd";
    }
    // Not in the project file (it usually comes from Microsoft.Cpp.props): guess by name.
    return lower(configuration).find("debug") != std::string::npos ? "/MDd" : "/MD";
}

[[nodiscard]] inline bool isCSource(std::string_view path) noexcept {
    const std::size_t dot = path.find_last_of('.');
    return dot != std::string_view::npos && iequals(path.substr(dot), ".c");
}

} // namespace frost::lang::detail
