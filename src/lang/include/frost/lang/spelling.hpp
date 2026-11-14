// SPDX-License-Identifier: Apache-2.0
// Spelling from installed OS dictionaries, never a fabricated bundled word list.
#pragma once
#include "frost/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace frost::lang {
struct SpellingIssue {
    std::uint32_t begin = 0, end = 0; // UTF-8 byte offsets in the exact supplied text.
    std::string word;
    std::vector<std::string> suggestions;
};
// Thread-safe independent calls; Windows uses the installed SpellChecking API. Other platforms
// return Unsupported (no dictionary/tool is claimed). No child processes or network calls.
[[nodiscard]] Result<std::vector<std::string>> spellingLanguages();
// Max64KiB UTF-8, 256issues, 16suggestions each; refuses truncation. Caller owns selected prose,
// current-document identity/version and applying corrections through its normal editor history.
[[nodiscard]] Result<std::vector<SpellingIssue>> checkSpelling(std::string_view text, std::string_view language);
// Explicit user command only: persists this one word through the installed dictionary provider.
[[nodiscard]] Result<void> addSpellingWord(std::string_view word, std::string_view language);
} // namespace frost::lang
