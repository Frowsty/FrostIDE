// SPDX-License-Identifier: Apache-2.0
// One bounded template renderer and catalogue, sharing the LSP snippet syntax and tab stops.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/snippet.hpp"
#include <map>
#include <span>

namespace frost::lang {
using TemplateVariables = std::map<std::string, std::string, std::less<>>;
enum class TemplateKind { Live, Postfix, File };
struct TemplateDefinition {
    std::string_view id, label, language;
    TemplateKind kind;
    std::string_view body;
};
struct TemplateExpansion { std::string snippet; ParsedSnippet parsed; };
// Actual caller-supplied ${UPPER_CASE} variables are escaped as literal text, never code/macros.
// Numbered LSP placeholders retain their syntax. Unknown named variables refuse; there is no
// shell, environment lookup, clock, network or recursive evaluation. Max 256KiB input/output,
// 64KiB per variable and 64 nested placeholders. Pure/thread-safe, O(input + output).
[[nodiscard]] Result<TemplateExpansion> expandTemplate(std::string_view body, const TemplateVariables&);
[[nodiscard]] std::span<const TemplateDefinition> builtinTemplates() noexcept;
[[nodiscard]] const TemplateDefinition* findTemplate(std::string_view id) noexcept;
// A linked-placeholder session stores snippet-local offsets and immutable expected text. A
// user change must be wholly inside the active placeholder; edits elsewhere terminate it.
struct TemplateMirrorEdit { std::uint32_t begin, end; std::string text; };
struct LiveTemplateSession {
    ParsedSnippet expected;
    std::uint32_t active = 0;
};
struct TemplateAdvance {
    LiveTemplateSession next;
    std::vector<TemplateMirrorEdit> edits; // sorted, non-overlapping; apply together as one edit.
    std::optional<SnippetTabStop> selection;
};
[[nodiscard]] Result<TemplateAdvance> advanceTemplate(const LiveTemplateSession&, std::string_view edited,
                                                      std::uint32_t caret);
} // namespace frost::lang
