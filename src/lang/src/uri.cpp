// SPDX-License-Identifier: Apache-2.0
//
// file:// URI conversion (uri.hpp).

#include "frost/lang/uri.hpp"

#include <algorithm>
#include <format>

namespace frost::lang {
namespace {

[[nodiscard]] bool unreserved(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

[[nodiscard]] int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void appendEncoded(std::string& out, std::string_view text) {
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (char c : text) {
        if (unreserved(c) || c == '/') {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0xF];
        }
    }
}

[[nodiscard]] bool isDriveLetter(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

} // namespace

Result<std::string> pathToUri(std::string_view path, io::PathStyle style) {
    auto normalized = io::normalizePath(path, style);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    std::string out = "file://";
    if (style == io::PathStyle::Posix) {
        appendEncoded(out, *normalized);
        return out;
    }
    std::string slashes = *normalized;
    for (char& c : slashes) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (slashes.starts_with("//")) { // UNC: the server becomes the authority.
        appendEncoded(out, std::string_view(slashes).substr(2));
        return out;
    }
    if (slashes.size() >= 2 && isDriveLetter(slashes[0]) && slashes[1] == ':') {
        out += '/';
        // Upper-case, as uriToPath produces and clangd writes, so URIs of one file compare equal.
        out += (slashes[0] >= 'a' && slashes[0] <= 'z') ? static_cast<char>(slashes[0] - 'a' + 'A') : slashes[0];
        out += ':';
        appendEncoded(out, std::string_view(slashes).substr(2));
        return out;
    }
    return fail(ErrorCode::InvalidArgument, std::format("no file URI form for '{}'", path));
}

Result<std::string> uriToPath(std::string_view uri, io::PathStyle style) {
    constexpr std::string_view kScheme = "file://";
    if (uri.size() < kScheme.size() || !std::equal(kScheme.begin(), kScheme.end(), uri.begin(), [](char a, char b) {
            return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b);
        })) {
        return fail(ErrorCode::InvalidArgument, std::format("not a file URI: '{}'", uri));
    }
    std::string_view rest = uri.substr(kScheme.size());
    std::string decoded;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%') {
            const bool complete = i + 2 < rest.size();
            const int high = complete ? hexValue(rest[i + 1]) : -1;
            const int low = complete ? hexValue(rest[i + 2]) : -1;
            if (high < 0 || low < 0) {
                return fail(ErrorCode::InvalidArgument, std::format("bad percent escape in '{}'", uri));
            }
            decoded += static_cast<char>((high << 4) | low);
            i += 2;
        } else {
            decoded += rest[i];
        }
    }
    std::string path;
    if (style == io::PathStyle::Posix) {
        if (!decoded.starts_with('/')) {
            return fail(ErrorCode::InvalidArgument, std::format("file URI with an authority on POSIX: '{}'", uri));
        }
        path = std::move(decoded);
    } else if (decoded.starts_with('/')) { // "/C:/a" -> "C:/a"
        path = decoded.substr(1);
        if (path.size() >= 2 && isDriveLetter(path[0]) && path[1] == ':') {
            path[0] = static_cast<char>(path[0] >= 'a' ? path[0] - 'a' + 'A' : path[0]);
        }
    } else { // "server/share/x" -> "//server/share/x"
        path = "//" + decoded;
    }
    if (style == io::PathStyle::Windows) {
        for (char& c : path) {
            if (c == '/') {
                c = '\\';
            }
        }
    }
    return io::normalizePath(path, style);
}

std::string documentKey(std::string_view path, io::PathStyle style) {
    auto normalized = io::normalizePath(path, style);
    std::string key = normalized ? std::move(*normalized) : std::string(path);
    if (style == io::PathStyle::Windows) {
        for (char& c : key) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            } else if (c == '/') {
                c = '\\';
            }
        }
    }
    return key;
}

} // namespace frost::lang
