// SPDX-License-Identifier: Apache-2.0
//
// The binary file helpers of the frost::diff tests (files.hpp): std::ifstream/std::ofstream over
// char8_t paths, so non-ASCII names work on Windows.

#include "diff/support/files.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace frost::diff::test {

std::filesystem::path nativePath(std::string_view utf8) {
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

std::string utf8Path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Result<std::string> readBinaryFile(std::string_view path) {
    std::ifstream in(nativePath(path), std::ios::binary);
    if (!in) {
        std::error_code error;
        const bool exists = std::filesystem::exists(nativePath(path), error);
        return fail(exists ? ErrorCode::IoError : ErrorCode::NotFound, std::format("cannot open {}", path));
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return fail(ErrorCode::IoError, std::format("cannot read {}", path));
    }
    return bytes;
}

Result<void> writeBinaryFile(std::string_view path, std::string_view bytes) {
    std::ofstream out(nativePath(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(ErrorCode::IoError, std::format("cannot create {}", path));
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        return fail(ErrorCode::IoError, std::format("cannot write {}", path));
    }
    return {};
}

Result<void> makeDirectories(std::string_view path) {
    std::error_code error;
    std::filesystem::create_directories(nativePath(path), error);
    if (error) {
        return fail(ErrorCode::IoError, std::format("cannot create directory {}: {}", path, error.message()));
    }
    return {};
}

Result<std::vector<std::string>> listNames(std::string_view directory, bool directories) {
    std::error_code error;
    std::filesystem::directory_iterator it(nativePath(directory), error);
    if (error) {
        return fail(error == std::errc::no_such_file_or_directory ? ErrorCode::NotFound : ErrorCode::IoError,
                    std::format("cannot list {}: {}", directory, error.message()));
    }
    std::vector<std::string> names;
    for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
        const bool isDirectory = it->is_directory(error);
        if (isDirectory == directories && (directories || it->is_regular_file(error))) {
            names.push_back(utf8Path(it->path().filename()));
        }
    }
    if (error) {
        return fail(ErrorCode::IoError, std::format("cannot list {}: {}", directory, error.message()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string joinUtf8(std::string_view directory, std::string_view name) {
    std::string out(directory);
    if (!out.empty() && out.back() != '/' && out.back() != '\\') {
        out += '/';
    }
    out += name;
    return out;
}

} // namespace frost::diff::test
