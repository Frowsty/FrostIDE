// SPDX-License-Identifier: Apache-2.0
//
// One-line escaping of headers and labels (escape.hpp).

#include "escape.hpp"

namespace frost::diff::detail {

void appendEscapedLine(std::string& out, std::string_view text) noexcept {
    for (const char c : text) {
        switch (c) {
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\\': out += "\\\\"; break;
        default: out += c; break;
        }
    }
}

} // namespace frost::diff::detail
