// SPDX-License-Identifier: Apache-2.0
//
// FileDocument: one open file in the SaveManager (ADR 0028): its text::Document (text, undo tree,
// anchors), the encoding it is saved with, the base it was loaded from or last saved as, and where
// the disk stands relative to it (in sync, changed with a merge waiting, out of date, deleted).
/*
 * Dirty. A document is dirty when its committed text state (text::UndoState::stateId, decision 14
 * of ADR 0017) differs from the state last loaded or saved, or its format was converted since. Undo
 * back to the saved state makes it clean again. A document restored from the hot-exit journal, or
 * one whose disk file changed under unsaved edits, has no saved state (always dirty until saved).
 *
 * Ownership and threads: the SaveManager owns every FileDocument (stable addresses until close())
 * and is its only mutator besides the editor's edits through document(). Not thread-safe: one owner
 * thread, as text::Document. Snapshots taken from it are immutable and safe anywhere.
 */
#pragma once

#include "frost/diff/merge.hpp"
#include "frost/document/load_service.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/text/document.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_file.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace frost::document {

class SaveManager;

// Where a document's file stands relative to the buffer.
enum class DiskState : std::uint8_t {
    InSync,    // The disk holds the base (the bytes last loaded or saved).
    Changed,   // The disk changed under unsaved edits; a merge waits (FileDocument::pendingMerge()).
    OutOfDate, // The disk changed, but the large-file guard or the merge refused it: the buffer and its
               // journal entry stay as they were (outOfDateReason() says why).
    Deleted,   // The file was removed or moved away; saving creates it again.
    Unsaved,   // An untitled buffer, or a restored one whose file does not exist.
};

// A dirty buffer's external change, waiting for the user's decision (the minimal merge view).
struct PendingMerge {
    // base = the last loaded or saved text, ours = the buffer, theirs = the disk. The UI resolves
    // chunks on it (MergeModel::resolve) before SaveManager::resolveExternalChange(ApplyMerge).
    diff::MergeModel model;
    std::uint64_t oursVersion = 0; // The buffer version ours was taken at.
    text::Snapshot theirs;         // The disk's text, decoded.
    text::DocumentEncoding theirsEncoding;
    storage::DiskBase theirsDisk;
    ReadOnlyReason theirsReadOnly = ReadOnlyReason::None;
};

class FileDocument {
public:
    // The committed-state value that never matches a real stateId: "no saved state".
    static constexpr std::uint64_t kNoSavedState = std::numeric_limits<std::uint64_t>::max();

    FileDocument(const FileDocument&) = delete ("a FileDocument is owned by its SaveManager");
    FileDocument& operator=(const FileDocument&) = delete ("a FileDocument is owned by its SaveManager");
    ~FileDocument() = default;

    // The normalised path, or "" for an untitled buffer. O(1).
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    // The hot-exit journal and local-history key: the normalised path, or "untitled:<n>". O(1).
    [[nodiscard]] const std::string& key() const noexcept { return m_key; }
    [[nodiscard]] bool untitled() const noexcept { return m_path.empty(); }

    // The text, its undo tree and anchors; the editor edits through it. O(1).
    [[nodiscard]] text::Document& document() noexcept { return m_document; }
    [[nodiscard]] const text::Document& document() const noexcept { return m_document; }

    // How the document is saved (format, load fidelity, detection). O(1).
    [[nodiscard]] const text::DocumentEncoding& encoding() const noexcept { return m_encoding; }
    // "Convert to": the next save writes `encoding` (with or without a BOM); marks the document
    // dirty. Precondition (FROST_PRE): encoding.isValid() && (!bom || encoding.isUnicode()). O(1).
    void convertTo(text::Encoding encoding, bool bom) noexcept;
    // The separator new lines get (existing breaks are never rewritten). Marks the document dirty
    // when it changes. O(1).
    void setLineSeparator(text::LineSeparator separator) noexcept;

    // See the header comment. O(1).
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] ReadOnlyReason readOnly() const noexcept { return m_readOnly; }
    [[nodiscard]] DiskState diskState() const noexcept { return m_diskState; }
    // The disk file as last loaded or saved (nullopt: untitled, or restored without a file). O(1).
    [[nodiscard]] const std::optional<storage::DiskBase>& disk() const noexcept { return m_disk; }
    // The text last loaded or saved: the base of an external-change merge. O(1).
    [[nodiscard]] const text::Snapshot& baseText() const noexcept { return m_baseText; }
    // The waiting external change of a DiskState::Changed document, else nullptr. O(1).
    [[nodiscard]] PendingMerge* pendingMerge() noexcept { return m_pendingMerge ? &*m_pendingMerge : nullptr; }
    [[nodiscard]] const PendingMerge* pendingMerge() const noexcept {
        return m_pendingMerge ? &*m_pendingMerge : nullptr;
    }
    // Why a DiskState::OutOfDate document was not reloaded or merged. O(1).
    [[nodiscard]] const std::optional<Error>& outOfDateReason() const noexcept { return m_outOfDate; }
    // Whether the text came from the hot-exit journal (after a crash or hot exit). O(1).
    [[nodiscard]] bool restored() const noexcept { return m_restored; }

private:
    friend class SaveManager;
    FileDocument(std::string path, std::string key, text::Document document) noexcept;

    std::string m_path;
    std::string m_key;
    text::Document m_document;
    text::DocumentEncoding m_encoding;
    ReadOnlyReason m_readOnly = ReadOnlyReason::None;
    DiskState m_diskState = DiskState::InSync;
    std::optional<storage::DiskBase> m_disk;
    text::Snapshot m_baseText;
    std::uint64_t m_savedStateId = 0;
    bool m_formatDirty = false;
    std::uint64_t m_formatVersion = 0; // Bumped by every format change, for journal generations.
    std::optional<PendingMerge> m_pendingMerge;
    std::optional<Error> m_outOfDate;
    bool m_restored = false;
    // Hot-exit journal bookkeeping (SaveManager): the generation of the current committed state,
    // the state it was assigned for, and what the journal holds for this document.
    std::int64_t m_generation = 0;
    std::uint64_t m_generationStateId = kNoSavedState;
    std::uint64_t m_generationFormat = 0;
    std::int64_t m_journaledGeneration = 0; // 0: no entry.
};

} // namespace frost::document
