// SPDX-License-Identifier: Apache-2.0
//
// Batch layout of the git differential (ADR 0019, D16): the cases of one git run as files
// <dir>/a/c%06d and <dir>/b/c%06d, written non-durably (files.hpp: no flush, no fsync).
// io::writeFileAtomic fsyncs every file and its directory and is far too slow for thousands of
// files. No gtest.
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace frost::diff::test {

// One case of a git batch: the bytes of side a and side b.
struct GitCase {
    std::string a;
    std::string b;
};

// The file name of case `index` ("c000042").
[[nodiscard]] std::string caseName(std::size_t index);

// Creates <directory>/a and <directory>/b and writes every case into both. `directory` must not
// exist or be empty. Errors: IoError.
[[nodiscard]] Result<void> writeBatch(std::string_view directory, std::span<const GitCase> cases);

// Writes side a of every case into <directory> (the tree `git apply` patches). Errors: IoError.
[[nodiscard]] Result<void> writeSideA(std::string_view directory, std::span<const GitCase> cases);

} // namespace frost::diff::test
