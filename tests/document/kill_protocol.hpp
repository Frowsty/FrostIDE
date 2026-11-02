// SPDX-License-Identifier: Apache-2.0
//
// The contents document_test_child alternates between in its save loop, shared with the kill test
// in real_disk_test.cpp so the parent can tell a whole save from a torn one. Test code only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace frost::document::test {

// The file the kill test starts from.
inline std::string killInitialContent() { return "initial\r\n"; }

// Save `n` (1, 2, ...) writes this: two different sizes, both larger than one write chunk, so a
// torn write would show as a size or byte mismatch.
inline std::string killContent(std::uint64_t n) {
    const std::size_t size = (n % 2 == 1) ? (std::size_t{3} << 20) : (std::size_t{2} << 20) + 4097;
    std::string text(size, n % 2 == 1 ? 'a' : 'b');
    text += "\r\n";
    return text;
}

} // namespace frost::document::test
