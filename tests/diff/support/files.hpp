// SPDX-License-Identifier: Apache-2.0
//
// Plain binary file helpers for the frost::diff tests: goldens, fixtures and git batches. Paths are
// UTF-8 and become std::filesystem::path through char8_t (wide on Windows), never a narrow const
// char* path, which Windows would read in the ANSI code page. Writes are not durable (no flush,
// no fsync): they serve test data that is rewritten on every run. No gtest.
#pragma once

#include "frost/core/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff::test {

// The std::filesystem::path of a UTF-8 path, and back.
[[nodiscard]] std::filesystem::path nativePath(std::string_view utf8);
[[nodiscard]] std::string utf8Path(const std::filesystem::path& path);

// The file's bytes. Errors: NotFound, IoError (with the path).
[[nodiscard]] Result<std::string> readBinaryFile(std::string_view path);

// Creates or truncates the file and writes `bytes`, binary. Errors: IoError (with the path).
[[nodiscard]] Result<void> writeBinaryFile(std::string_view path, std::string_view bytes);

// Creates the directory and its parents. Errors: IoError.
[[nodiscard]] Result<void> makeDirectories(std::string_view path);

// The names of the entries of `directory` that are directories (`directories`) or regular files,
// sorted bytewise. Errors: NotFound, IoError.
[[nodiscard]] Result<std::vector<std::string>> listNames(std::string_view directory, bool directories);

// `name` appended to `directory` with '/'.
[[nodiscard]] std::string joinUtf8(std::string_view directory, std::string_view name);

} // namespace frost::diff::test
