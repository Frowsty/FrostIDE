// SPDX-License-Identifier: Apache-2.0
//
// SaveManager's external-change handling (save_manager.hpp, ADR 0028): which watcher changes touch
// an open file, the disk check against the base, the clean reload as one undoable edit, the dirty
// buffer's three-way merge model, and the merge decisions.

#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/diff/merge.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/text/line_separator.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace frost::document {
namespace {

// Whether `ancestor` is a proper ancestor directory of `path` (both normalised in `style`).
bool isAncestor(std::string_view ancestor, std::string_view path, io::PathStyle style) noexcept {
    if (ancestor.size() >= path.size() || !path.starts_with(ancestor)) {
        return false;
    }
    const auto isSeparator = [&](char c) { return c == '/' || (style == io::PathStyle::Windows && c == '\\'); };
    // A root ("/", "C:\") already ends with its separator.
    return isSeparator(ancestor.back()) || isSeparator(path[ancestor.size()]);
}

// file_watcher.hpp's rule for a consumer interested in the single path `target`.
bool touches(const io::FileChange& change, std::string_view target, io::PathStyle style) noexcept {
    if (change.path == target) {
        return true;
    }
    if (change.kind == io::FileChangeKind::Renamed &&
        (change.oldPath == target || isAncestor(change.oldPath, target, style))) {
        return true;
    }
    if (!isAncestor(change.path, target, style)) {
        return false;
    }
    switch (change.kind) {
    case io::FileChangeKind::Created:
    case io::FileChangeKind::Removed:
    case io::FileChangeKind::Rescan:
    case io::FileChangeKind::Renamed: return true;
    case io::FileChangeKind::Modified: return change.type.has_value() && *change.type != io::FileType::Directory;
    }
    return false;
}

// Whether `snapshot`'s bytes equal `other`'s. O(n).
bool sameText(const text::Snapshot& snapshot, const text::Snapshot& other) {
    return snapshot.size() == other.size() && snapshot.text() == other.text();
}

// Applies `edits` as one undo step of its own (never coalesced with the user's typing).
Result<void> applyExternal(FileDocument& document, std::span<const text::Edit> edits) {
    document.document().breakCoalescing();
    auto changed = document.document().edit(edits, text::EditContext{text::EditKind::External, {}, {}});
    document.document().breakCoalescing();
    if (!changed) {
        return std::unexpected(std::move(changed.error()));
    }
    return {};
}

} // namespace

std::vector<ExternalChange> SaveManager::applyChanges(const io::FileChangeBatch& batch) {
    std::vector<ExternalChange> found;
    for (const auto& owned : m_documents) {
        FileDocument& document = *owned;
        if (document.untitled()) {
            continue;
        }
        const bool affected = std::ranges::any_of(batch.changes, [&](const io::FileChange& change) {
            return touches(change, document.path(), m_options.style);
        });
        if (!affected) {
            continue;
        }
        ExternalChange change = checkDisk(document);
        if (change.kind == ExternalChangeKind::None) {
            continue;
        }
        if (m_listener) {
            m_listener(change);
        }
        found.push_back(std::move(change));
    }
    return found;
}

sched::Task<Result<void>> SaveManager::follow(io::IDirectoryWatch& watch, std::stop_token token) {
    for (;;) {
        auto batch = co_await watch.next(token);
        if (!batch) {
            co_return std::unexpected(std::move(batch.error()));
        }
        static_cast<void>(applyChanges(*batch));
    }
}

ExternalChange SaveManager::markOutOfDate(FileDocument& document, Error reason) {
    document.m_diskState = DiskState::OutOfDate;
    document.m_pendingMerge.reset();
    document.m_outOfDate = reason;
    return ExternalChange{&document, ExternalChangeKind::OutOfDate, std::move(reason)};
}

ExternalChange SaveManager::checkDisk(FileDocument& document) {
    ExternalChange none{&document, ExternalChangeKind::None, std::nullopt};
    if (document.untitled()) {
        return none;
    }
    auto status = m_services.fs->status(document.path());
    if (!status) {
        if (status.error().code() != ErrorCode::NotFound) {
            return ExternalChange{&document, ExternalChangeKind::Failed, std::move(status.error())};
        }
        if (document.m_diskState == DiskState::Deleted || document.m_diskState == DiskState::Unsaved) {
            return none;
        }
        document.m_diskState = DiskState::Deleted;
        document.m_pendingMerge.reset();
        return ExternalChange{&document, ExternalChangeKind::Deleted, std::nullopt};
    }
    const auto matches = [&](const storage::DiskBase& base) {
        return status->size == base.size && status->mtime == base.mtime;
    };
    if (document.m_diskState != DiskState::Deleted && document.m_disk && matches(*document.m_disk)) {
        return none;
    }
    if (document.m_pendingMerge && matches(document.m_pendingMerge->theirsDisk)) {
        return none; // The change the merge was built for.
    }
    auto raw = m_loader.read(document.path());
    if (!raw) {
        if (raw.error().code() == ErrorCode::OutOfRange) {
            return markOutOfDate(document, std::move(raw.error()));
        }
        return ExternalChange{&document, ExternalChangeKind::Failed, std::move(raw.error())};
    }
    const auto sameBytes = [&](const storage::DiskBase& base) {
        return raw->disk.size == base.size && raw->disk.fingerprint == base.fingerprint;
    };
    if (document.m_disk && sameBytes(*document.m_disk)) {
        // Our own save's echo, a touch, or the base back (which also drops a waiting merge or an
        // out-of-date mark): only the time moved.
        document.m_disk->mtime = raw->disk.mtime;
        document.m_diskState = DiskState::InSync;
        document.m_pendingMerge.reset();
        document.m_outOfDate.reset();
        return ExternalChange{&document, ExternalChangeKind::Touched, std::nullopt};
    }
    if (document.m_pendingMerge && sameBytes(document.m_pendingMerge->theirsDisk)) {
        document.m_pendingMerge->theirsDisk.mtime = raw->disk.mtime;
        return none;
    }
    std::vector<Error> warnings; // Local history is best effort here.
    recordHistory(document, storage::RevisionKind::ExternalChange, raw->bytes, warnings);
    // A user override ("Reload in") stays in force; otherwise the disk's bytes are detected afresh.
    std::optional<text::Encoding> encoding;
    if (document.m_encoding.detection.source == text::DetectionSource::UserOverride) {
        encoding = document.m_encoding.format.encoding;
    }
    auto theirs = m_loader.decode(std::move(*raw), document.path(), encoding);
    if (!theirs) {
        return markOutOfDate(document, std::move(theirs.error()));
    }
    if (!document.dirty()) {
        return reloadClean(document, std::move(*theirs));
    }
    return prepareMerge(document, Theirs{theirs->buffer.snapshot(), theirs->encoding,
                                         theirs->disk.value_or(storage::DiskBase{}), theirs->readOnly});
}

Result<void> SaveManager::replaceText(FileDocument& document, const text::Snapshot& with) {
    const text::Snapshot ours = document.m_document.committedText();
    std::vector<text::Edit> edits;
    auto oursLines = diff::LineSequence::fromSnapshot(ours);
    auto theirsLines = diff::LineSequence::fromSnapshot(with);
    std::string whole; // Backs the single whole-text edit when the line diff is not available.
    if (oursLines && theirsLines) {
        if (auto lineDiff = diff::diffLines(*oursLines, *theirsLines)) {
            edits = diff::toEdits(*oursLines, *theirsLines, *lineDiff);
        }
    }
    if (edits.empty() && !sameText(ours, with)) {
        // Above the diff's limits (64 MiB, 2^21 lines): one edit replaces the whole text.
        whole = with.text();
        edits.push_back(text::Edit{text::ByteRange{ByteOffset{0}, ours.size()}, whole});
    }
    if (edits.empty()) {
        return {};
    }
    return applyExternal(document, edits);
}

ExternalChange SaveManager::reloadClean(FileDocument& document, LoadedDocument theirs) {
    const text::Snapshot text = theirs.buffer.snapshot();
    if (auto replaced = replaceText(document, text); !replaced) {
        return markOutOfDate(document, std::move(replaced.error()));
    }
    document.m_encoding = theirs.encoding;
    document.m_readOnly = theirs.readOnly;
    document.m_disk = theirs.disk;
    document.m_baseText = document.m_document.committedText();
    document.m_transformBase.reset(); // The disk text is the new starting point.
    document.m_savedStateId = document.m_document.undoState().stateId;
    document.m_formatDirty = false;
    document.m_diskState = DiskState::InSync;
    document.m_pendingMerge.reset();
    document.m_outOfDate.reset();
    return ExternalChange{&document, ExternalChangeKind::Reloaded, std::nullopt};
}

ExternalChange SaveManager::prepareMerge(FileDocument& document, const Theirs& theirs) {
    const text::Snapshot& theirsText = theirs.text;
    auto base = diff::LineSequence::fromSnapshot(document.m_baseText);
    auto ours = diff::LineSequence::fromSnapshot(document.m_document.committedText());
    auto theirsLines = diff::LineSequence::fromSnapshot(theirsText);
    if (!base || !ours || !theirsLines) {
        Error error = !base   ? std::move(base.error())
                      : !ours ? std::move(ours.error())
                              : std::move(theirsLines.error());
        return markOutOfDate(document, std::move(error).withContext("while building the external-change merge"));
    }
    diff::MergeOptions options;
    options.eol = text::lineBreakOf(document.m_encoding.format.lineSeparator);
    auto model = diff::MergeModel::create(std::move(*base), std::move(*ours), std::move(*theirsLines), options);
    if (!model) {
        return markOutOfDate(document,
                             std::move(model.error()).withContext("while building the external-change merge"));
    }
    document.m_pendingMerge.emplace(PendingMerge{std::move(*model), document.m_document.version(), theirs.text,
                                                 theirs.encoding, theirs.disk, theirs.readOnly});
    document.m_diskState = DiskState::Changed;
    document.m_outOfDate.reset();
    return ExternalChange{&document, ExternalChangeKind::MergePending, std::nullopt};
}

void SaveManager::adoptTheirs(FileDocument& document, PendingMerge& merge) {
    document.m_baseText = merge.theirs;
    document.m_transformBase.reset(); // The user's own edits now differ from theirs.
    document.m_disk = merge.theirsDisk;
    document.m_diskState = DiskState::InSync;
    document.m_outOfDate.reset();
    if (merge.theirsReadOnly != ReadOnlyReason::None) {
        document.m_readOnly = merge.theirsReadOnly;
    }
}

Result<void> SaveManager::resolveExternalChange(FileDocument& document, MergeDecision decision) {
    if (!document.m_pendingMerge) {
        return fail(ErrorCode::NotFound, std::format("no external change of '{}' is pending", document.path()));
    }
    PendingMerge& merge = *document.m_pendingMerge;
    switch (decision) {
    case MergeDecision::ApplyMerge: {
        if (document.m_document.version() != merge.oursVersion) {
            // The buffer moved on: rebuild the merge against it (from copies: the old merge goes).
            Theirs theirs{merge.theirs, merge.theirsEncoding, merge.theirsDisk, merge.theirsReadOnly};
            static_cast<void>(prepareMerge(document, theirs));
            return fail(ErrorCode::Busy,
                        "the buffer changed after the merge was built; it was rebuilt, resolve it again");
        }
        auto patch = merge.model.editsForOurs();
        if (!patch) {
            return std::unexpected(std::move(patch.error()));
        }
        if (!patch->edits().empty()) {
            if (auto changed = applyExternal(document, patch->edits()); !changed) {
                return changed;
            }
        }
        const bool equalsDisk = sameText(document.m_document.committedText(), merge.theirs);
        document.m_encoding = merge.theirsEncoding;
        adoptTheirs(document, merge);
        document.m_formatDirty = false;
        document.m_savedStateId = equalsDisk ? document.m_document.undoState().stateId : FileDocument::kNoSavedState;
        break;
    }
    case MergeDecision::KeepBuffer:
        adoptTheirs(document, merge);
        document.m_savedStateId = FileDocument::kNoSavedState; // The disk now differs from every buffer state.
        break;
    case MergeDecision::TakeDisk: {
        if (auto replaced = replaceText(document, merge.theirs); !replaced) {
            return std::unexpected(std::move(replaced.error()));
        }
        document.m_encoding = merge.theirsEncoding;
        adoptTheirs(document, merge);
        document.m_formatDirty = false;
        document.m_savedStateId = document.m_document.undoState().stateId;
        break;
    }
    }
    document.m_pendingMerge.reset();
    return {};
}

} // namespace frost::document
