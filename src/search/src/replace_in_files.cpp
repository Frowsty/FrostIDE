// SPDX-License-Identifier: Apache-2.0
//
// replaceInFiles (replace_in_files.hpp; ADR 0045): the per-file checks, the one-undo-step edit of
// open documents and the closed-file rewrite through the SaveManager.

#include "frost/search/replace_in_files.hpp"

#include "frost/document/file_document.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/search/find_in_files.hpp"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace frost::search {
namespace {

// The replacement texts of `plan`'s ranges in `text`, or the reason the file must be skipped.
Result<std::vector<document::RewriteEdit>> planEdits(const text::SearchPattern& pattern, const text::Snapshot& text,
                                                     const ReplaceFilePlan& plan, std::string_view replacement,
                                                     bool& changed) {
    changed = false;
    if (hashText(text) != plan.textHash) {
        changed = true;
        return fail(ErrorCode::Busy, std::format("'{}' changed since the search; search again", plan.path));
    }
    std::vector<document::RewriteEdit> edits;
    edits.reserve(plan.ranges.size());
    ByteOffset previous{0};
    for (const text::ByteRange& range : plan.ranges) {
        if (range.begin < previous || range.end > text.size() || !pattern.matches(text, range)) {
            changed = true;
            return fail(ErrorCode::Busy,
                        std::format("a match in '{}' is no longer where the search found it; search again", plan.path));
        }
        previous = range.end;
        edits.push_back(document::RewriteEdit{range, pattern.replacementFor(text, range, replacement)});
    }
    return edits;
}

[[nodiscard]] ReplaceSkipReason reasonFor(const Error& error) noexcept {
    switch (error.code()) {
    case ErrorCode::PermissionDenied: return ReplaceSkipReason::ReadOnly;
    case ErrorCode::Busy: return ReplaceSkipReason::Locked;
    case ErrorCode::Corrupt: return ReplaceSkipReason::Lossy;
    case ErrorCode::InvalidArgument: return ReplaceSkipReason::Unencodable;
    default: return ReplaceSkipReason::Failed;
    }
}

} // namespace

std::string_view toString(ReplaceSkipReason reason) noexcept {
    switch (reason) {
    case ReplaceSkipReason::ReadOnly: return "read-only";
    case ReplaceSkipReason::Locked: return "locked";
    case ReplaceSkipReason::Changed: return "changed since the search";
    case ReplaceSkipReason::Lossy: return "not decodable losslessly";
    case ReplaceSkipReason::Unencodable: return "replacement not representable in the file's encoding";
    case ReplaceSkipReason::Failed: return "failed";
    }
    return "failed";
}

Result<ReplaceSummary> replaceInFiles(document::SaveManager& documents, const ReplaceRequest& request,
                                      const DocumentEdited& edited) {
    auto pattern = text::SearchPattern::compile(request.pattern, request.options);
    if (!pattern) {
        return std::unexpected(std::move(pattern.error()));
    }
    ReplaceSummary summary;
    const auto skip = [&](const std::string& path, ReplaceSkipReason reason, Error error) {
        summary.skipped.push_back(SkippedReplaceFile{path, reason, std::move(error)});
    };
    for (const ReplaceFilePlan& plan : request.files) {
        if (plan.ranges.empty()) {
            continue;
        }
        bool changed = false;
        if (document::FileDocument* open = documents.find(plan.path)) {
            if (open->readOnly() != document::ReadOnlyReason::None) {
                skip(plan.path, ReplaceSkipReason::ReadOnly,
                     Error(ErrorCode::PermissionDenied, std::format("'{}' is open read-only", plan.path)));
                continue;
            }
            auto edits = planEdits(*pattern, open->document().committedText(), plan, request.replacement, changed);
            if (!edits) {
                skip(plan.path, changed ? ReplaceSkipReason::Changed : reasonFor(edits.error()), std::move(edits.error()));
                continue;
            }
            std::vector<text::Edit> batch;
            batch.reserve(edits->size());
            for (const document::RewriteEdit& one : *edits) {
                batch.push_back(text::Edit{one.range, one.text});
            }
            // One transaction: Other edits never coalesce, so one undo reverts the whole file.
            open->document().breakCoalescing();
            auto change = open->document().edit(batch, text::EditContext{text::EditKind::Other, {}, {}});
            if (!change) {
                skip(plan.path, ReplaceSkipReason::Failed, std::move(change.error()));
                continue;
            }
            open->document().breakCoalescing();
            if (edited) {
                edited(*open, *change);
            }
            summary.replacements += edits->size();
            summary.replaced.push_back(ReplacedFile{plan.path, edits->size(), true});
            continue;
        }
        auto rewritten = documents.rewriteClosedFile(
            plan.path, [&](const text::Snapshot& text) -> Result<std::vector<document::RewriteEdit>> {
                return planEdits(*pattern, text, plan, request.replacement, changed);
            });
        if (!rewritten) {
            const ReplaceSkipReason reason = changed ? ReplaceSkipReason::Changed : reasonFor(rewritten.error());
            skip(plan.path, reason, std::move(rewritten.error()));
            continue;
        }
        for (Error& warning : rewritten->warnings) {
            summary.warnings.push_back(std::move(warning));
        }
        if (rewritten->written) {
            summary.replacements += rewritten->edits;
            summary.replaced.push_back(ReplacedFile{plan.path, rewritten->edits, false});
        }
    }
    return summary;
}

} // namespace frost::search
