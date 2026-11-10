// SPDX-License-Identifier: Apache-2.0
//
// file:// URIs for LSP (RFC 8089 and RFC 3986): converting absolute paths to the DocumentUri form
// language servers expect and back, and the path key the client matches documents by.
/*
 * Windows: "C:\a b\x.cpp" <-> "file:///C:/a%20b/x.cpp"; a UNC path "\\server\share\x" <->
 * "file://server/share/x". POSIX: "/home/a b/x.cpp" <-> "file:///home/a%20b/x.cpp". Every byte
 * outside RFC 3986's unreserved set (and '/', and the drive's ':') is percent-encoded as UTF-8.
 * Decoding accepts any case of the hex digits, an encoded drive colon ("c%3A", as VS Code writes
 * it) and a lower-case drive letter. All functions are pure and thread-safe; O(length).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/path.hpp"

#include <string>
#include <string_view>

namespace frost::lang {

// The file URI of `path`. Errors: InvalidArgument (not an absolute path of `style`).
[[nodiscard]] Result<std::string> pathToUri(std::string_view path, io::PathStyle style = io::kNativePathStyle);

// The normalised absolute path of a file URI. Errors: InvalidArgument (another scheme, a bad
// percent escape, or a path normalizePath refuses).
[[nodiscard]] Result<std::string> uriToPath(std::string_view uri, io::PathStyle style = io::kNativePathStyle);

// A key for comparing paths: normalised when possible, ASCII-lower-cased with Windows rules (the
// file system there ignores case). Equal keys name the same document.
[[nodiscard]] std::string documentKey(std::string_view path, io::PathStyle style = io::kNativePathStyle);

/*
 * A cheap test before documentKey(path) == key: false only when `path` cannot have that key because
 * its file name (the text after the last '/' or '\') differs from the key's even ignoring ASCII case.
 * A file name that normalisation may still change ("", ".", "..", or one ending in '.' or ' ') always
 * passes. Lets a caller compare one document against thousands of paths (a CMake model's sources)
 * without normalising each. Pure; O(file name length).
 */
[[nodiscard]] bool mayHaveDocumentKey(std::string_view path, std::string_view key) noexcept;

} // namespace frost::lang
