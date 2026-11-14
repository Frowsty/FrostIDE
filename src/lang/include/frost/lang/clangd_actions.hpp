// SPDX-License-Identifier: Apache-2.0
//
// clangd's code actions by name (FND-3: REF-0 and the harm part of CPP-1; ADR 0078 truth rule):
// the stock clangd 23 tweaks the named refactor and generate commands map to, the order Refactor
// This lists them in, and the include-cleaner fix Optimize Includes applies. Every edit comes from
// clangd itself (a code action's edit, or the workspace/applyEdit clangd sends while executing
// clangd.applyTweak); nothing here edits text.
/*
 * Tweaks. clangd offers a tweak as a code action whose command is "clangd.applyTweak" with one
 * argument {file, selection, tweakID}; executing it (workspace/executeCommand) makes clangd send
 * the edit as a workspace/applyEdit request before it answers. tweakIdOf() reads the id;
 * findTweak() picks the action of a tweak by id and, for servers that leave the id out, by the
 * title clangd 23 gives it (kTweaks). Refactor This (orderRefactorActions) lists the known tweaks
 * in kTweaks order, then the other refactor actions and tweaks in the server's order; quick fixes
 * that are not tweaks and source actions stay with Alt+Enter.
 *
 * Include cleaner. clangd publishes "unused-includes" and (when its config enables them)
 * "missing-includes" diagnostics; textDocument/codeAction with those diagnostics in the context
 * answers with their fixes. pickIncludeCleanerFix prefers "fix all includes", then "remove all
 * unused includes", then "add all missing includes", then the first single fix.
 *
 * Threads: pure functions, safe anywhere.
 */
#pragma once

#include "frost/lang/diagnostics.hpp"
#include "frost/lang/refactoring.hpp"

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace frost::lang {

inline constexpr std::string_view kClangdApplyTweak = "clangd.applyTweak";

// A stock clangd tweak a named command maps to.
struct ClangdTweakInfo {
    std::string_view id;          // clangd's tweak id ("ExtractVariable").
    std::string_view titlePrefix; // The start of the title clangd 23 gives it.
};

// The tweaks the named commands use, in Refactor This order.
inline constexpr std::array<ClangdTweakInfo, 8> kTweaks{{
    {"ExtractVariable", "Extract subexpression to variable"},
    {"ExtractFunction", "Extract to function"},
    {"InlineVariable", "Inline variable"},
    {"DefineInline", "Move function body to declaration"},
    {"DefineOutline", "Move function body to out-of-line"},
    {"MemberwiseConstructor", "Define constructor"},
    {"SpecialMembers", "Declare implicit"},
    {"OverridePureVirtuals", "Override pure virtual methods"},
}};

// The tweak id of a clangd tweak action, or "" for any other action. O(1).
[[nodiscard]] std::string_view tweakIdOf(const CodeActionItem& action) noexcept;

// The action of `actions` for tweak `id` (by id, else by kTweaks' title for that id), or nullptr.
// O(actions).
[[nodiscard]] const CodeActionItem* findTweak(std::span<const CodeActionItem> actions, std::string_view id) noexcept;

// The actions Refactor This lists after Rename, in its order (see the header comment). Stable.
[[nodiscard]] std::vector<CodeActionItem> orderRefactorActions(std::vector<CodeActionItem> actions);

// True for clangd's include-cleaner diagnostics ("unused-includes", "missing-includes").
[[nodiscard]] bool isIncludeCleanerDiagnostic(const LiveDiagnostic& diagnostic) noexcept;

// The include-cleaner fix to apply (see the header comment), or nullopt when `actions` (the answer
// for the include-cleaner diagnostics) has none with an edit.
[[nodiscard]] std::optional<CodeActionItem> pickIncludeCleanerFix(std::span<const CodeActionItem> actions);

} // namespace frost::lang
