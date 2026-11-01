// SPDX-License-Identifier: Apache-2.0
//
// One-line escaping for unified-diff headers and merge-marker labels (ADR 0019, D9 and D11), so a
// caller-supplied name can never start a new line (and so never inject a hunk header or a marker).
#pragma once

#include <string>
#include <string_view>

namespace frost::diff::detail {

// Appends `text` to `out` with CR, LF and backslash written as \r, \n and \\; every other byte is
// copied. O(|text|). TODO(M2): git's full path quoting (core.quotePath).
void appendEscapedLine(std::string& out, std::string_view text) noexcept;

} // namespace frost::diff::detail
