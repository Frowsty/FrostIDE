// SPDX-License-Identifier: Apache-2.0
//
// Installs the synthetic CMake File API reply (tests/project/fixtures/cmake/reply) into a file
// system for a given source and build directory: the placeholders @SOURCE@, @BUILD@, @OUTSIDE@,
// @CMAKE_ROOT@ and @COMPILER@ become absolute '/'-separated paths, as CMake writes them. Used by the
// frost::project and frost::build tests. Header-only; test code only.
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace frost::test {

// `path` with '\' turned into '/' (the File API's spelling).
inline std::string cmakeSlashes(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

inline void replaceAll(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

/*
 * Copies the reply fixture from `fixtureDir` (absolute, on disk) into
 * <buildDir>/.cmake/api/v1/reply on `fs`. `outside` is a directory outside the source tree (for
 * the external source). Fails the current test on an I/O error.
 */
inline void installCMakeReply(io::IFileSystem& fs, std::string_view fixtureDir, std::string_view sourceDir,
                              std::string_view buildDir, std::string_view outside) {
    const bool windows = io::kNativePathStyle == io::PathStyle::Windows;
    const std::string replyDir =
        io::joinPath(buildDir, windows ? ".cmake\\api\\v1\\reply" : ".cmake/api/v1/reply").value();
    ASSERT_TRUE(fs.createDirectories(replyDir).has_value()) << replyDir;
    auto entries = io::realFileSystem().listDirectory(fixtureDir);
    ASSERT_TRUE(entries.has_value()) << fixtureDir;
    for (const io::DirEntry& entry : *entries) {
        auto content = io::realFileSystem().readFile(io::joinPath(fixtureDir, entry.name).value());
        ASSERT_TRUE(content.has_value()) << entry.name;
        std::string text = *content;
        replaceAll(text, "@SOURCE@", cmakeSlashes(sourceDir));
        replaceAll(text, "@BUILD@", cmakeSlashes(buildDir));
        replaceAll(text, "@OUTSIDE@", cmakeSlashes(outside));
        replaceAll(text, "@CMAKE_ROOT@", windows ? "C:/Program Files/CMake" : "/usr");
        replaceAll(text, "@COMPILER@", windows ? "C:/LLVM/bin/clang.exe" : "/usr/bin/clang");
        ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(replyDir, entry.name).value(), text).has_value()) << entry.name;
    }
}

} // namespace frost::test
