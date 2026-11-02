// SPDX-License-Identifier: Apache-2.0
//
// Replace in Files (PLAN.md row 86; ADR 0045): applies the matches a Find in Files preview kept to
// the files, never corrupting or truncating one.
/*
 * The plan is the preview: per file, the text hash the search saw (FileMatches::textHash) and the
 * included matches' byte ranges. Each file is checked before anything is changed: its current text
 * must hash the same and every range must still be exactly a match of the pattern, or the whole
 * file is skipped as Changed ("search again"). Then:
 *   - a document open in an editor is edited in its buffer through its FileDocument, all of the
 *     file's replacements as one undo step; it is dirty afterwards and the normal autosave (or the
 *     user) saves it. A read-only editor (the long-line guard) is skipped as ReadOnly;
 *   - any other file is rewritten through SaveManager::rewriteClosedFile: its own encoding, BOM and
 *     line breaks, a lossy load refused (Lossy), a replacement the encoding cannot hold refused
 *     (Unencodable), a local-history snapshot of the old bytes first, and safeWrite, which refuses
 *     read-only (ReadOnly) and locked (Locked) files and leaves them as they were.
 * Regex replacements expand the same templates as the in-file replace ($0-$99, $&, $$, \n, \r, \t,
 * \\; text/editor/search.hpp).
 *
 * Threads: the SaveManager's owner thread (the UI thread), like every SaveManager call; closed files
 * are read and written there. TODO(M2): move the closed-file rewrites to a background lane once the
 * SaveManager's save path does (its own TODO(M1)).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/document.hpp"
#include "frost/text/editor/search.hpp"
#include "frost/text/positions.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace frost::document {
class FileDocument;
class SaveManager;
} // namespace frost::document

namespace frost::search {

// One file of a plan: the search's text hash and the included matches, ascending, non-overlapping.
struct ReplaceFilePlan {
    std::string path;
    std::uint64_t textHash = 0;
    std::vector<text::ByteRange> ranges;
};

struct ReplaceRequest {
    std::string pattern;
    text::SearchOptions options;
    std::string replacement;
    std::vector<ReplaceFilePlan> files;
};

// Why a file was left alone.
enum class ReplaceSkipReason : std::uint8_t {
    ReadOnly,    // A read-only file or editor.
    Locked,      // Another program holds the file, or it changed while it was written.
    Changed,     // The text differs from what the search saw.
    Lossy,       // The file does not decode losslessly, so writing it would change other bytes.
    Unencodable, // A replacement has a character the file's encoding cannot hold.
    Failed,      // Any other error (not found, I/O).
};

// "read-only", "locked", "changed since the search", "not decodable losslessly",
// "replacement not representable in the file's encoding", "failed".
[[nodiscard]] std::string_view toString(ReplaceSkipReason reason) noexcept;

struct ReplacedFile {
    std::string path;
    std::size_t replacements = 0;
    bool openDocument = false; // Edited in its buffer (dirty, not yet saved).
};

struct SkippedReplaceFile {
    std::string path;
    ReplaceSkipReason reason = ReplaceSkipReason::Failed;
    Error error{ErrorCode::Unknown};
};

struct ReplaceSummary {
    std::size_t replacements = 0;
    std::vector<ReplacedFile> replaced;
    std::vector<SkippedReplaceFile> skipped;
    std::vector<Error> warnings; // After successful writes (local history, the temp sweep).
};

// Told about every open document a replace edited, with the edit's ChangeSet, so editor views can
// follow it (Editor::follow). Owner thread.
using DocumentEdited = std::function<void(document::FileDocument& document, const text::ChangeSet& change)>;

/*
 * Applies `request` file by file (header comment); a file's problem never stops the others.
 * Errors: InvalidArgument when the pattern does not compile. O(total size of the files).
 */
[[nodiscard]] Result<ReplaceSummary> replaceInFiles(document::SaveManager& documents, const ReplaceRequest& request,
                                                    const DocumentEdited& edited = {});

} // namespace frost::search
