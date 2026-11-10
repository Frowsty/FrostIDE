// SPDX-License-Identifier: Apache-2.0
//
// The edits of the actions on save (save_actions.hpp): clangd's formatting and its include-cleaner fix.

#include "frost/lang/save_actions.hpp"

#include "frost/lang/clangd_actions.hpp"
#include "frost/lang/uri.hpp"

#include <algorithm>

namespace frost::lang {

sched::Task<Result<std::vector<ByteEdit>>> formatEditsForSave(ILspRequester& server, std::string path,
                                                              text::Snapshot snapshot, FormattingOptions options,
                                                              std::stop_token token) {
    const PositionEncoding encoding = server.encoding();
    auto edits = co_await requestFormatting(server, std::move(path), std::nullopt, options, std::move(token));
    if (!edits) {
        co_return std::unexpected(std::move(edits.error()));
    }
    co_return toByteEdits(snapshot, *edits, encoding);
}

sched::Task<Result<std::vector<ByteEdit>>> includeCleanerEditsForSave(ILspRequester& server, std::string path,
                                                                      text::Snapshot snapshot,
                                                                      std::vector<LiveDiagnostic> diagnostics,
                                                                      std::stop_token token) {
    std::erase_if(diagnostics,
                  [](const LiveDiagnostic& diagnostic) { return !isIncludeCleanerDiagnostic(diagnostic); });
    if (diagnostics.empty()) {
        co_return std::vector<ByteEdit>{};
    }
    // One request over the span of every include-cleaner diagnostic, as Optimize Includes sends it.
    LspRange range = diagnostics.front().lspRange;
    for (const LiveDiagnostic& diagnostic : diagnostics) {
        range.start = std::min(range.start, diagnostic.lspRange.start);
        range.end = std::max(range.end, diagnostic.lspRange.end);
    }
    const PositionEncoding encoding = server.encoding();
    auto actions = co_await requestCodeActions(server, path, range, std::move(diagnostics), std::move(token));
    if (!actions) {
        co_return std::unexpected(std::move(actions.error()));
    }
    const std::optional<CodeActionItem> fix = pickIncludeCleanerFix(*actions);
    if (!fix) {
        co_return std::vector<ByteEdit>{};
    }
    for (const FileEdit& file : fix->edit.files) {
        if (documentKey(file.path) == documentKey(path)) {
            co_return toByteEdits(snapshot, file.edits, encoding);
        }
    }
    co_return std::vector<ByteEdit>{};
}

} // namespace frost::lang
