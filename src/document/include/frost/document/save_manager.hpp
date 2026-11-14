// SPDX-License-Identifier: Apache-2.0
//
// SaveManager: FrostIDE's data-safety core for open files (PLAN.md section 2.2 "Data safety", I2;
// ADR 0028). It opens files through the load service, saves them byte-exactly and atomically
// (io::IFileSystem::safeWrite: ReplaceFileW or rename, keeping attributes, ACLs, hard links and
// symlinks), journals unsaved buffers for hot exit and crash recovery (storage::HotExitJournal),
// records local-history revisions on save, and handles external changes from the file watcher.
/*
 * Saving. save() encodes the committed text with the document's format (BOM, encoding, and its own
 * line breaks, never rewritten; ADR 0018) and writes it with safeWrite, so a locked, read-only or
 * full target fails with a clear error (Busy, PermissionDenied, IoError) and keeps its old bytes;
 * the buffer then stays dirty and its journal entry stays. A lossy load (U+FFFD) needs
 * SaveOptions::consent. After the write: the base becomes the saved text, the journal entry is
 * cleared, local history records the bytes, settings scope files are validated (SettingsStore::
 * checkSavedFile), and stale temp files in the directory are swept once per session.
 *
 * External changes. applyChanges() takes file-watcher batches (or follow() pumps a watch) and runs
 * checkDisk() for every open file a change touches (file_watcher.hpp's single-path rules). A disk
 * file whose size and time, or failing that its XXH3-128, equal the base is unchanged (the echo of
 * our own save). A clean buffer reloads as one undoable edit (text::EditKind::External, the line
 * diff of buffer and disk); a dirty one gets a PendingMerge (base = last saved, ours = buffer,
 * theirs = disk) for the merge view, resolved with resolveExternalChange(). A file that crossed the
 * large-file threshold, or a merge that cannot be built, leaves the buffer and its journal entry
 * alone and marks the document DiskState::OutOfDate. The disk's new content is recorded in local
 * history (RevisionKind::ExternalChange) first.
 *
 * Closed files. rewriteClosedFile() applies edits to a file no editor has open (Replace in Files,
 * ADR 0045) with the same guarantees as a save: the file's own encoding, BOM and line breaks, a
 * lossy load refused, a local-history snapshot first, and safeWrite.
 *
 * Hot exit. journalDirty() writes every dirty (or out-of-date) buffer whose state changed since its
 * last entry, with a fresh generation from one process-wide counter, and clears the entry of a
 * buffer that became clean (undo back to the saved state). closeJournal() journals and marks a clean
 * shutdown. restore() reopens the entries of journals that an exited or crashed process left
 * (storage::findRecoverableJournals) through the load service, puts them into this process's
 * journal, and discards the old journals; an entry the guard refuses is carried over unchanged.
 *
 * Threads and blocking: not thread-safe; one owner thread (the shell's document actor), which also
 * runs the editors' text::Document edits. Calls that touch the disk, the journal or local history
 * block. TODO(M1): run the encode and write of save() and the journal puts on a background lane
 * from snapshots captured on the owner thread (the steps are already split that way inside).
 * Lifetime: every service in SaveManagerServices outlives the manager.
 */
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/document/file_document.hpp"
#include "frost/document/load_service.hpp"
#include "frost/document/policies.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/task.hpp"
#include "frost/settings/store.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/text/document.hpp"
#include "frost/text/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::document {

// What the SaveManager works with. Non-owning; every pointer may be null except as noted.
struct SaveManagerServices {
    io::IFileSystem* fs = nullptr;              // Required.
    const IClock* clock = nullptr;              // Required: document undo timing, history pruning cadence, temp sweeps.
    storage::HotExitJournal* journal = nullptr; // This process's journal; null disables hot exit.
    storage::LocalHistory* history = nullptr;   // Null disables local history.
    settings::SettingsStore* settings = nullptr; // Validation on save of settings scope files.
    text::ITextCodecProvider* codecs = nullptr;  // Null: text::platformCodecs().
};

struct SaveManagerOptions {
    LoadServiceOptions load{};
    text::DocumentOptions document{};
    io::PathStyle style = io::kNativePathStyle; // The path style of SaveManagerServices::fs.
    // Local history is pruned after a save at most this often (and by pruneHistory()).
    IClock::Duration pruneInterval = std::chrono::hours(1);
    // Temp files ("*.frost-tmp-*") older than this, from other processes, are swept.
    IClock::Duration staleTempAge = std::chrono::hours(1);
};

// Why a save happened (local history labels, reports).
enum class SaveReason : std::uint8_t { Explicit, SaveAll, Autosave };

struct SaveOptions {
    // The user accepted that a lossy load's U+FFFD replacements are written (ADR 0018, D6).
    text::SaveConsent consent = text::SaveConsent::None;
    // The explicit "overwrite read-only file" action: the file is made writable and saved.
    bool overwriteReadOnly = false;
    // Save over a disk file that changed since the base (a waiting merge, an out-of-date document,
    // or a change the watcher has not reported yet).
    bool overwriteExternalChanges = false;
    SaveReason reason = SaveReason::Explicit;
};

// editor.trimTrailingWhitespace ("none", "modified", "all"; EditorConfig's trim_trailing_whitespace
// maps true to All and false to None).
enum class TrimTrailingWhitespace : std::uint8_t { None, Modified, All };

/*
 * What a save does to the text before it is encoded (the pre-save transform; editor.* "On Save"
 * settings). Trailing spaces and tabs are removed on every line (All) or on the lines that differ
 * from the last explicitly saved text (Modified, by line diff; none when the file is above the diff's
 * limits), except on keepLines, the lines a caret is on, so typing is never undone under the caret
 * (Rider's default). insertFinalNewline appends the document's line separator to a non-empty text
 * that does not end with a line break. Plain value.
 */
struct SaveTransform {
    TrimTrailingWhitespace trim = TrimTrailingWhitespace::None;
    bool insertFinalNewline = false;
    std::vector<LineIndex> keepLines;
};

// One replacement of saveTransformEdits: [range) of the text becomes `text`.
struct SaveEdit {
    text::ByteRange range;
    std::string text;

    friend bool operator==(const SaveEdit&, const SaveEdit&) = default;
};

/*
 * The edits that apply `transform` to `current` (lines as frost::text counts them: LF, CR and CRLF
 * end a line), ascending and non-overlapping, in `current`'s byte coordinates; empty when nothing
 * changes. `base` is the last saved text (for Modified); `newLine` is the separator a final newline
 * uses. Pure; O(size) plus the line diff for Modified (Budget: B21).
 */
[[nodiscard]] std::vector<SaveEdit> saveTransformEdits(const text::Snapshot& current, const text::Snapshot& base,
                                                       const SaveTransform& transform, std::string_view newLine);

struct SaveOutcome {
    bool written = false; // false: the document was clean and in sync, so nothing was written.
    std::string target;   // The file written (a symlink's target).
    io::SafeWriteMethod method = io::SafeWriteMethod::Replaced;
    std::uint64_t bytes = 0;
    // Problems after the file was safely written (journal, local history, temp sweep, status).
    std::vector<Error> warnings;
    std::optional<settings::SaveCheck> settingsCheck; // When the file is a settings scope file.
};

// Why saveAll() passed a document over.
enum class SkipReason : std::uint8_t {
    Untitled,     // Needs Save As.
    OutOfDate,    // DiskState::OutOfDate: needs an explicit save over the disk.
    MergePending, // DiskState::Changed: needs the merge decision.
    Deleted,      // The file was deleted on disk; an autosave never recreates it.
    NeedsConsent, // A lossy load; saving needs the user's consent.
};

struct SkippedDocument {
    FileDocument* document = nullptr;
    SkipReason reason = SkipReason::Untitled;
};

struct FailedSave {
    FileDocument* document = nullptr;
    Error error{ErrorCode::Unknown};
};

// The result of saveAll(): every dirty document is in exactly one list.
struct SaveAllResult {
    std::vector<FileDocument*> saved;
    std::vector<SkippedDocument> skipped;
    std::vector<FailedSave> failed;
    std::vector<FailedSave> warnings; // Saved, with a warning requiring user attention.
};

// One replacement of a closed-file rewrite (rewriteClosedFile): bytes `range` of the decoded UTF-8
// text become `text`.
struct RewriteEdit {
    text::ByteRange range;
    std::string text;
};

// Computes a rewrite's edits from the file's decoded text: ascending, non-overlapping, on char
// boundaries (TextBuffer::applyEdits' rules). An error cancels the rewrite; no edits writes nothing.
using RewriteFunction = frost::function_ref<Result<std::vector<RewriteEdit>>(const text::Snapshot& text)>;

// What rewriteClosedFile() did.
struct RewriteOutcome {
    bool written = false; // false: the function returned no edits, so nothing was written.
    std::string target;   // The file written (a symlink's target).
    std::uint64_t bytes = 0;
    std::size_t edits = 0;
    // Problems after the file was safely written (local history of the new bytes, the temp sweep).
    std::vector<Error> warnings;
};

// What checkDisk() found.
enum class ExternalChangeKind : std::uint8_t {
    None,         // Nothing changed (or the change is already pending).
    Touched,      // Same content, new time (our own save's echo, a touch): the base time was updated.
    Reloaded,     // A clean buffer took the disk's text as one undoable edit.
    MergePending, // A dirty buffer got a PendingMerge.
    OutOfDate,    // Kept the buffer; see FileDocument::outOfDateReason().
    Deleted,      // The file is gone.
    Failed,       // The disk could not be read (error set); nothing changed.
};

struct ExternalChange {
    FileDocument* document = nullptr;
    ExternalChangeKind kind = ExternalChangeKind::None;
    std::optional<Error> error; // Failed and OutOfDate.
};

// The user's answer to a PendingMerge.
enum class MergeDecision : std::uint8_t {
    ApplyMerge, // Apply the model's resolution (every conflict resolved) as one undoable edit.
    KeepBuffer, // Keep the buffer as it is; the disk's text becomes the base (the next save overwrites it).
    TakeDisk,   // Replace the buffer with the disk's text as one undoable edit; clean afterwards.
};

struct JournalReport {
    std::size_t written = 0; // Entries put.
    std::size_t cleared = 0; // Entries of buffers that became clean, removed.
    std::vector<Error> errors;
};

// A journal entry restore() could not reopen; it was carried over unchanged into this process's journal.
struct DeferredEntry {
    std::string documentKey;
    std::string path;
    Error reason{ErrorCode::Unknown};
};

struct RestoreReport {
    std::vector<FileDocument*> restored;
    std::vector<FileDocument*> diskChanged; // Restored documents whose file changed since the entry was written.
    std::vector<DeferredEntry> deferred;
    std::size_t journalsDiscarded = 0;
    std::vector<Error> errors; // Entries or journals that could not be read; their journals are kept.
};

class SaveManager {
public:
    // Preconditions: services.fs and services.clock are set; they and the other services outlive the
    // manager.
    explicit SaveManager(SaveManagerServices services, SaveManagerOptions options = {}, SavePolicies policies = {});
    ~SaveManager();
    SaveManager(const SaveManager&) = delete ("documents refer to their manager's services");
    SaveManager& operator=(const SaveManager&) = delete ("documents refer to their manager's services");

    // Owner-thread guard for temporary external operations; empty removes it.
    void setWriteGuard(std::function<Result<void>(std::string_view)> guard) { m_writeGuard = std::move(guard); }
    /*
     * The pre-save transform: before a dirty document (or a Save As) is encoded, `provider` says
     * what to do to its text (SaveTransform; the caller resolves the file's settings and
     * EditorConfig), and the edits are applied to the document as one undoable step, so the editor
     * shows exactly what was written. It runs for every SaveReason, autosaves included; the
     * provider's keepLines (the caret lines) keep typing from being trimmed under the user. Modified
     * lines are measured from the last explicit save: after an autosave, an explicit save (or Save
     * All) of the then clean document still runs the transform and writes only if it changes the
     * text, so what the autosave spared is trimmed then. Not run while an IME composition is
     * active. If the write fails after the edits were applied, they stay in the still dirty buffer
     * as an undoable step. Empty removes it. Owner thread; the provider must not save or close
     * documents.
     */
    void setSaveTransform(std::function<SaveTransform(const FileDocument&, SaveReason)> provider) {
        m_saveTransform = std::move(provider);
    }

    // --- Documents ---

    /*
     * Opens `path` through the load service, or returns the document already open for it. `encoding`
     * overrides detection. The first open in a directory sweeps its stale temp files. Errors: the load
     * service's (OutOfRange for the large-file guard), InvalidArgument (a relative path).
     */
    [[nodiscard]] Result<FileDocument*> open(std::string_view path,
                                             std::optional<text::Encoding> encoding = std::nullopt);
    // A new empty buffer (key "untitled:<n>") with `format`; DiskState::Unsaved. Saved with saveAs().
    FileDocument& createUntitled(text::TextFileFormat format);
    // The open document for `path` (normalised), or nullptr. O(documents).
    [[nodiscard]] FileDocument* find(std::string_view path) const;
    // Closes `document` (the caller has asked about unsaved changes): drops its journal entry; the
    // pointer dangles afterwards. Errors: the journal's (the document is closed regardless).
    [[nodiscard]] Result<void> close(FileDocument& document);
    // Every open document, in opening order.
    [[nodiscard]] std::span<const std::unique_ptr<FileDocument>> documents() const noexcept { return m_documents; }

    // --- Saving ---

    // Saves `document` to its file (see the header comment). Errors: InvalidArgument (untitled), Busy
    // (an external change is pending or the disk is out of date, without overwriteExternalChanges;
    // the file is in use), PermissionDenied (read-only), Corrupt (lossy, no consent), IoError (disk
    // full), the encoder's (InvalidArgument for an unencodable character). Each with the context
    // "while saving <path>"; the file on disk is unchanged on every error.
    [[nodiscard]] Result<SaveOutcome> save(FileDocument& document, const SaveOptions& options = {});
    // Saves `document` under `path` (Save As); afterwards the document is that file. Errors: as save(),
    // AlreadyExists (another open document has that path).
    [[nodiscard]] Result<SaveOutcome> saveAs(FileDocument& document, std::string_view path,
                                             const SaveOptions& options = {});
    // Rebinds `document`'s path and key to `newPath` after an on-disk rename without writing or clearing
    // dirty state. Errors: InvalidArgument, AlreadyExists (another open document has that path).
    [[nodiscard]] Result<void> renameDocument(FileDocument& document, std::string_view newPath);
    // Saves every dirty document that can be saved without asking (Save All; autosave uses it).
    SaveAllResult saveAll(SaveReason reason = SaveReason::SaveAll);
    // Whether saveAll() would pass `document` over, and why. O(1).
    [[nodiscard]] std::optional<SkipReason> skipReason(const FileDocument& document, SaveReason reason) const noexcept;
    // Whether any document is dirty. O(documents).
    [[nodiscard]] bool anyDirty() const noexcept;

    /*
     * Rewrites a file that is not open here (Replace in Files, ADR 0045) so that only the edited
     * bytes change:
     *   1. reads it through the load service (the large-file guard) and decodes it with detection;
     *   2. refuses a file that is open here (Busy: edit its document instead), a load that is not
     *      lossless (Corrupt: saving would not restore the other bytes) and a binary-looking file
     *      (InvalidArgument);
     *   3. asks `edit` for the edits on the decoded text; none: nothing is written;
     *   4. applies them, encodes with the file's own format (encoding and BOM; every line break stays
     *      as it is, D9) and checks that the bytes decode back to the edited text (Internal);
     *   5. records the old bytes in local history (RevisionKind::Labelled, "Before <label>") unless
     *      history is off; a failure there refuses the rewrite;
     *   6. checks that the file did not change since step 1 (Busy), then writes it with safeWrite: a
     *      read-only target fails with PermissionDenied, one in use with Busy.
     * Every error leaves the file as it was and has the context "while rewriting <path>". After the
     * write the new bytes are recorded in local history (RevisionKind::Save, `label`). Owner thread;
     * blocking. O(size).
     */
    [[nodiscard]] Result<RewriteOutcome> rewriteClosedFile(std::string_view path, RewriteFunction edit,
                                                           std::string_view label = "Replace in Files");

    // --- Policies ---

    void setPolicies(SavePolicies policies) noexcept { m_policies = policies; }
    // setPolicies(SavePolicies::fromSettings(snapshot)).
    void applySettings(const settings::SettingsSnapshot& snapshot);
    [[nodiscard]] const SavePolicies& policies() const noexcept { return m_policies; }

    // --- External changes ---

    // checkDisk() for every open document a change of `batch` touches; returns the changes found
    // (kind != None), also reported to the listener.
    std::vector<ExternalChange> applyChanges(const io::FileChangeBatch& batch);
    // Compares the document's file with its base and reacts (see the header comment).
    ExternalChange checkDisk(FileDocument& document);
    // Pumps `watch` into applyChanges() until it ends; completes with the watch's error (Cancelled
    // through `token`). Must resume on the owner thread. The manager and the watch outlive the task.
    [[nodiscard]] sched::Task<Result<void>> follow(io::IDirectoryWatch& watch, std::stop_token token = {});
    // Called for every change applyChanges() finds (on the owner thread); empty to remove.
    void setExternalChangeListener(std::function<void(const ExternalChange&)> listener) {
        m_listener = std::move(listener);
    }
    /*
     * Settles a document's PendingMerge. ApplyMerge needs every conflict resolved (InvalidArgument
     * otherwise, from MergeModel::editsForOurs). When the buffer changed since the merge was built,
     * the merge is rebuilt against it (resolutions are lost) and Busy is returned. Errors: NotFound
     * (no merge pending), the edit's (Busy while an IME composition is active).
     */
    [[nodiscard]] Result<void> resolveExternalChange(FileDocument& document, MergeDecision decision);

    // --- Hot exit ---

    // Journals unsaved buffers (see the header comment). Does nothing without a journal or with
    // policies().hotExit off.
    JournalReport journalDirty();
    // journalDirty(), then HotExitJournal::closeCleanly(); the journal is unusable afterwards and the
    // manager stops journaling. Errors: the journal's.
    [[nodiscard]] Result<void> closeJournal();
    // Restores the entries of `scan`'s journals (see the header comment); the scan's journals are
    // discarded unless an entry could not be read.
    RestoreReport restore(storage::RecoveryScan& scan);

    // --- Local history ---

    // Prunes local history with policies().retention now. Errors: NotFound (no history), the store's.
    [[nodiscard]] Result<storage::PruneStats> pruneHistory();

    [[nodiscard]] const LoadService& loadService() const noexcept { return m_loader; }

private:
    friend class RefactoringCoordinator;
    [[nodiscard]] Result<std::string> normalize(std::string_view path) const;
    [[nodiscard]] FileDocument& adopt(std::string path, std::string key, LoadedDocument loaded);
    [[nodiscard]] std::int64_t generationOf(FileDocument& document);
    [[nodiscard]] bool needsJournal(const FileDocument& document) const noexcept;
    [[nodiscard]] Result<SaveOutcome> saveTo(FileDocument& document, const std::string& path,
                                             const SaveOptions& options, bool saveAs);
    void putJournal(FileDocument& document, std::vector<Error>& errors);
    void recordHistory(const FileDocument& document, storage::RevisionKind kind, std::string_view bytes,
                       std::vector<Error>& warnings);
    void sweepDirectory(std::string_view directory, std::vector<Error>& warnings);
    ExternalChange markOutOfDate(FileDocument& document, Error reason);
    [[nodiscard]] Result<void> replaceText(FileDocument& document, const text::Snapshot& with);
    ExternalChange reloadClean(FileDocument& document, LoadedDocument theirs);
    // The disk's side of an external change.
    struct Theirs {
        text::Snapshot text;
        text::DocumentEncoding encoding;
        storage::DiskBase disk;
        ReadOnlyReason readOnly = ReadOnlyReason::None;
    };
    ExternalChange prepareMerge(FileDocument& document, const Theirs& theirs);
    void adoptTheirs(FileDocument& document, PendingMerge& merge);
    // Restore one entry; true when its journal must be kept (the entry is not safe in this journal).
    [[nodiscard]] bool restoreEntry(const storage::JournalEntry& entry, const std::string& content,
                                    RestoreReport& report);
    // Puts a refused entry into this process's journal unchanged; true if it landed there.
    [[nodiscard]] bool carryOver(const storage::JournalEntry& entry, std::string_view content, Error reason,
                                 RestoreReport& report);

    SaveManagerServices m_services;
    SaveManagerOptions m_options;
    SavePolicies m_policies;
    LoadService m_loader;
    std::vector<std::unique_ptr<FileDocument>> m_documents;
    std::function<void(const ExternalChange&)> m_listener;
    std::function<Result<void>(std::string_view)> m_writeGuard;
    std::function<SaveTransform(const FileDocument&, SaveReason)> m_saveTransform;
    std::set<std::string, std::less<>> m_sweptDirectories;
    std::set<std::uint64_t> m_deferredDiskChecks;
    // One composition-root coordinator may adopt a newly opened clean created file into its
    // existing global history. Callback is cleared by that coordinator before destruction.
    const void* m_refactoringOpenOwner = nullptr;
    std::function<void(FileDocument&)> m_refactoringOpened;
    TransactionId m_refactoringPending; // Source saves/close are barred until committed settlement.
    std::int64_t m_nextGeneration = 0;
    std::uint64_t m_nextUntitled = 0;
    std::optional<IClock::SteadyTime> m_lastPrune;
};

} // namespace frost::document
