// SPDX-License-Identifier: Apache-2.0
//
// Rename (Shift+F6), quick fixes (Alt+Enter) and reformat (Ctrl+Alt+L) for W2 (ADR 0041):
// textDocument/prepareRename and rename, textDocument/codeAction with the diagnostics under the
// caret (clangd answers with its fix-its), and textDocument/formatting / rangeFormatting (clangd
// runs clang-format with the project's .clang-format). The resulting edits are applied by the
// caller through IWorkspaceEditApplier (workspace_edit.hpp) on the thread that owns the documents.
/*
 * Quick fixes and clangd tweaks (REF-3) and include management (REF-11). Direct WorkspaceEdit
 * actions are applied by the caller; command actions are sent through requestExecuteCommand, with
 * workspace/applyEdit handled by the existing server callback. The list is sorted preferred fixes
 * first, then quick fixes, then the rest, each in the server's order.
 * Threads: the requests are coroutines; the rest is pure.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lsp_client.hpp"
#include "frost/lang/lsp_types.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/sched/task.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::lang {

struct PrepareRenameResult {
    LspRange range;               // The name being renamed.
    std::string placeholder;      // The current name ("" when the server left it to the client).
    bool defaultBehavior = false; // {defaultBehavior: true}: the client picks the word at the caret.
};

/*
 * textDocument/prepareRename: nullopt when the symbol cannot be renamed (null). Servers without
 * prepare support are not asked: the result is defaultBehavior. Errors: request's (clangd
 * answers a non-renamable symbol with an error whose message says why).
 */
[[nodiscard]] sched::Task<Result<std::optional<PrepareRenameResult>>>
requestPrepareRename(ILspRequester& server, std::string path, LspPosition position, std::stop_token token = {});

// textDocument/rename to `newName`. Errors: request's, parseWorkspaceEdit's, InvalidArgument
// (an empty name).
[[nodiscard]] sched::Task<Result<WorkspaceEdit>> requestRename(ILspRequester& server, std::string path,
                                                               LspPosition position, std::string newName,
                                                               std::stop_token token = {});

struct CodeActionItem {
    std::string title;
    std::string kind; // "quickfix", "refactor.rewrite", ... or "".
    bool preferred = false;
    WorkspaceEdit edit;
    std::string commandName;
    std::vector<Json> commandArguments;
    std::string disabledReason; // Actual server refusal or unsupported resource operation; never partially applied.
};

// A diagnostic as LSP sends it (range, severity, code, source, message), for codeAction's context.
[[nodiscard]] Json diagnosticToJson(const LiveDiagnostic& diagnostic);

// The code actions of an answer (see the header comment for their order). Unsupported resource
// edits remain visible with disabledReason, never a partially decoded/applicable text action.
[[nodiscard]] std::vector<CodeActionItem> parseCodeActions(const Json& json);

// textDocument/codeAction for `range` with `diagnostics` (those overlapping the range).
// Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<CodeActionItem>>>
requestCodeActions(ILspRequester& server, std::string path, LspRange range, std::vector<LiveDiagnostic> diagnostics,
                   std::stop_token token = {});
struct ScopeFixes {
    WorkspaceEdit edit;
    std::vector<std::string> chosenActions;
    std::vector<std::string> unresolved;
};
// Caller captures all scope before-images and server/document versions BEFORE this request.
// Chooses only unique or uniquely preferred direct quickfix edits. Ambiguous fixes, commands and
// unsupported resources remain unresolved; conflicting ranges/versions fail the complete union.
[[nodiscard]] sched::Task<Result<ScopeFixes>>
requestFixesInScope(ILspRequester& server, std::vector<FileDiagnostics> scope, std::stop_token token = {});

// workspace/executeCommand for `command` with `arguments`. Errors: InvalidArgument (empty command),
// request's.
[[nodiscard]] sched::Task<Result<Json>> requestExecuteCommand(LspRequestSender& server, std::string_view command,
                                                              const std::vector<Json>& arguments = {},
                                                              std::stop_token token = {});

// Queries `textDocument/codeAction` across `diagnostics` (or the document) for clangd's
// include-cleaner unused-include removal and missing-include insertion quick fixes and merges
// their non-overlapping TextEdits into a single WorkspaceEdit (REF-11).
[[nodiscard]] sched::Task<Result<WorkspaceEdit>>
requestOptimizeIncludes(ILspRequester& server, std::string path, std::span<const LiveDiagnostic> diagnostics = {},
                        std::stop_token token = {});

// The diagnostics of `all` whose LSP range overlaps or touches `range`.
[[nodiscard]] std::vector<LiveDiagnostic> diagnosticsAt(std::span<const LiveDiagnostic> all, LspRange range);

struct FormattingOptions {
    std::uint32_t tabSize = 4;
    bool insertSpaces = true;
};

// textDocument/formatting (no range) or textDocument/rangeFormatting. Errors: request's.
[[nodiscard]] sched::Task<Result<std::vector<TextEdit>>> requestFormatting(ILspRequester& server, std::string path,
                                                                           std::optional<LspRange> range,
                                                                           FormattingOptions options,
                                                                           std::stop_token token = {});

} // namespace frost::lang
