// SPDX-License-Identifier: Apache-2.0
// Byte spans of CMake commands and arguments for conservative membership planning.
#pragma once
#include "frost/core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace frost::project {
struct CMakeSyntaxArgument {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string literal; // Empty if unsupported or empty; check isLiteral.
    bool isLiteral = false;
};
struct CMakeSyntaxCommand {
    std::string name;
    std::size_t line = 0;
    std::size_t close = 0;
    std::size_t scopeDepth = 0; // if, loops, functions, macros and block scopes.
    bool nestedArguments = false;
    std::vector<CMakeSyntaxArgument> arguments;
};
// Scan whole CMake source without modifying it. Spans are byte offsets into text, valid for
// text's unchanged bytes. Complex arguments remain opaque. O(text.size()); malformed input is
// Unsupported. Thread-safe; returned values own their strings.
[[nodiscard]] Result<std::vector<CMakeSyntaxCommand>> scanCMakeSyntax(std::string_view text);
} // namespace frost::project
