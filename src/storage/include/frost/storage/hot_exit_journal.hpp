// SPDX-License-Identifier: Apache-2.0
//
// HotExitJournal: the SQLite dirty-buffer journal for hot exit and crash recovery (PLAN.md
// section 2.2 "Data safety"; ADR 0014), and the recovery scan that finds journals of FrostIDE
// processes that have exited or crashed.
//
/*
 * Design. Each FrostIDE process owns one journal file,
 * journal-<UTC yyyymmddTHHMMSSfffZ>-<16 hex from sqlite3_randomness>.db, in a directory the
 * caller chooses (user data, outside <cache>: Invalidate Caches never removes it). The journal is
 * opened with OpenOptions::exclusiveLock, so its process holds SQLite's write lock for its whole
 * life. Ownership is therefore detected by OS locks: a journal whose lock can be taken belongs to
 * a process that has exited or crashed; no PID checks and no single-instance pipe are needed, and
 * several FrostIDE instances each keep their own journal. The OS releases the lock when a
 * process dies; on Windows that can lag, hence findRecoverableJournals' lockWait.
 *
 * Content is opaque bytes, normally the buffer's UTF-8 text. Encoding, BOM and line ending are
 * labels for the later save; the journal never converts anything. Each put rewrites the whole
 * entry in chunks of JournalOptions::chunkBytes with an XXH3-128 checksum, in one transaction.
 *
 * Schema v1 (application_id 0x4672484A 'FrHJ', STRICT tables): journal_state (one row: created,
 * clean_shutdown, updated), journal_entry (one row per documentKey), journal_chunk (the content,
 * in seq order, cascading from its entry) and journal_clean (the clean marks). Timestamps are
 * nanoseconds since the Unix epoch, UTC. tests/storage/goldens/hot_exit_journal.v1.schema.txt
 * pins the DDL.
 *
 * Every call blocks on disk I/O; never call one on the UI thread.
 */
#pragma once

#include "frost/compat/function_ref.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/storage/database.hpp"
#include "frost/storage/fingerprint.hpp"
#include "frost/storage/store.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::storage {

namespace detail {
struct JournalAccess;
} // namespace detail

// The file a buffer derives from, as it was when the buffer was loaded or last saved.
struct DiskBase {
    std::uint64_t size = 0;
    IClock::SystemTime mtime{};
    Fingerprint128 fingerprint{};
};

struct DocumentState {
    // Non-empty opaque identity: a normalised path or "untitled:<n>". TODO(M2): the cache-key
    // path normaliser (PLAN.md section 2, "Storage and caching") will produce it.
    std::string documentKey;
    std::string path; // Absolute path, or "" for an untitled buffer.
    /*
     * >= 0, and monotonically increasing per documentKey for the journal's whole life, also
     * across buffers that reuse a key (one process-wide counter is simplest). A put whose
     * generation is <= the stored one, or <= the document's clean mark, is Stale (a no-op).
     */
    std::int64_t generation = 0;
    std::string encoding; // Opaque label (m1/encodings defines the names).
    bool hasBom = false;
    std::string lineEnding; // Opaque label.
    std::optional<DiskBase> base;
};

// One journal entry's metadata (content is read separately).
struct JournalEntry {
    std::int64_t id = 0;
    DocumentState state;
    IClock::SystemTime updatedAt{};
    std::uint64_t contentSize = 0;
    Fingerprint128 contentFingerprint{};
};

enum class PutOutcome : unsigned char { Written, Stale };

struct JournalOptions {
    // Content chunk size, in [4 KiB, 64 MiB]; otherwise create() fails with InvalidArgument.
    std::size_t chunkBytes = std::size_t{1} << 20;
    Synchronous synchronous = Synchronous::Full;
    // SQLite VFS for tests (fault injection); empty means the default (database.hpp).
    std::string vfs{};
};

/*
 * This process's journal. Move-only, with one owner (the SaveManager actor); not thread-safe.
 * Destroying it closes the connection WITHOUT marking a clean shutdown, so the next start
 * reports a crash and offers the entries.
 */
class HotExitJournal {
public:
    /*
     * Creates this process's journal in `directory` (created if missing). The new file must be
     * fresh, and must still exist once it is locked (a recovery scan in another instance may
     * remove a new, still unlocked file as abandoned); otherwise create() retries with a new
     * random suffix (3 attempts). Creation is atomic (schema.hpp). Errors: as Database::open;
     * IoError; InvalidArgument (chunkBytes); AlreadyExists after 3 failed attempts.
     */
    [[nodiscard]] static Result<HotExitJournal> create(std::string_view directory, IClock& clock = systemClock(),
                                                       const JournalOptions& options = {});

    HotExitJournal(HotExitJournal&&) noexcept = default;
    HotExitJournal& operator=(HotExitJournal&&) noexcept = default;
    HotExitJournal(const HotExitJournal&) = delete ("a journal has one owner");
    HotExitJournal& operator=(const HotExitJournal&) = delete ("a journal has one owner");
    ~HotExitJournal() = default;

    /*
     * Replaces the entry for state.documentKey atomically, in one IMMEDIATE transaction: a
     * crash leaves the old entry or the new one, never a mixture. The content is the
     * concatenation of `content`; it is re-chunked into chunkBytes rows through an
     * O(chunkBytes) staging buffer and XXH3-128 checksummed. O(content size). A stop request
     * rolls back and returns Cancelled (the old entry stays). An IoError leaves the outcome
     * unknown (Transaction::commit); a retry with the same generation is safe (Stale if the
     * first one landed). Never call it on the UI thread. Budget: B21.
     * Errors: InvalidArgument (empty key, negative generation), IoError (disk full), Busy,
     * Cancelled, Corrupt.
     */
    [[nodiscard]] Result<PutOutcome> put(const DocumentState& state, std::span<const std::string_view> content,
                                         std::stop_token stop = {});

    /*
     * The document was saved (or its changes discarded) as of `generation`: deletes the entry if
     * its generation is <= `generation` and returns true (also when there was no entry); keeps a
     * newer entry and returns false (newer edits are still dirty). Also records `generation` as
     * the document's clean mark (the maximum of the old and new marks), so a put queued before
     * the save stays Stale. Errors: InvalidArgument (empty key, negative generation), IoError, Busy.
     */
    [[nodiscard]] Result<bool> markClean(std::string_view documentKey, std::int64_t generation);
    // The buffer closed: drops the entry and the clean mark. True if either existed.
    [[nodiscard]] Result<bool> forget(std::string_view documentKey);
    // Metadata of every entry, sorted by documentKey (bytewise). O(entries).
    [[nodiscard]] Result<std::vector<JournalEntry>> entries();

    /*
     * Marks a clean shutdown (clean_shutdown = 1, committed while the lock is held) and closes.
     * Without entries (clean marks do not count) the files are then removed; otherwise they
     * stay for the next start (hot exit). The object is unusable afterwards, whatever the
     * result. Errors: those of the update and close; Busy when the removal finds the file open
     * again (another instance's scan, which then removes it). Budget: B20.
     */
    [[nodiscard]] Result<void> closeCleanly();

    [[nodiscard]] const std::string& path() const noexcept { return m_database.path(); }
    // For tests (limits, white-box corruption).
    [[nodiscard]] Database& database() noexcept { return m_database; }

private:
    friend struct detail::JournalAccess;
    HotExitJournal(Database database, IClock& clock, const JournalOptions& options) noexcept;

    Database m_database;
    IClock* m_clock;
    std::size_t m_chunkBytes;
};

/*
 * A journal of an exited process, opened (and locked) by this process for recovery. Move-only;
 * not thread-safe. Destroying it without discard() leaves the files for the next start.
 */
class RecoveredJournal {
public:
    RecoveredJournal(RecoveredJournal&&) noexcept = default;
    RecoveredJournal& operator=(RecoveredJournal&&) noexcept = default;
    RecoveredJournal(const RecoveredJournal&) = delete ("a journal has one owner");
    RecoveredJournal& operator=(const RecoveredJournal&) = delete ("a journal has one owner");
    ~RecoveredJournal() = default;

    // Hot exit (true: closeCleanly ran) or crash (false).
    [[nodiscard]] bool endedCleanly() const noexcept { return m_endedCleanly; }
    // quick_check failed; entries are still offered where their checksums verify.
    [[nodiscard]] bool damaged() const noexcept { return m_damaged; }
    // Entry metadata, as HotExitJournal::entries. On a damaged journal this may fail with
    // Corrupt (the entry table is unreadable); discard() then quarantines the file.
    [[nodiscard]] Result<std::vector<JournalEntry>> entries();
    /*
     * Streams the entry's chunks in order to `sink`, then checks the size, the chunk count and
     * the XXH3-128 checksum. Corrupt on a mismatch: the sink must then discard what it received.
     * A sink error stops the read and is returned. O(size) time, O(chunk) memory. Errors:
     * NotFound (no such entry), Corrupt, Cancelled (stop request), IoError.
     */
    [[nodiscard]] Result<void> readContent(std::int64_t entryId,
                                           frost::function_ref<Result<void>(std::string_view)> sink,
                                           std::stop_token stop = {});
    // The whole content in memory; as above.
    [[nodiscard]] Result<std::string> readContent(std::int64_t entryId);
    /*
     * After the caller restored or declined the entries: deletes every entry in one transaction
     * while still holding the lock (so a scan in another instance that wins the race sees an
     * empty journal, never a second copy of the buffers), closes, then removes the files with
     * removeDatabaseFiles, or quarantines them if damaged() (or the delete found them corrupt).
     * Errors: those of the delete, the close, the removal and the quarantine.
     */
    [[nodiscard]] Result<void> discard() &&;

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    // For tests.
    [[nodiscard]] Database& database() noexcept { return m_database; }

private:
    friend struct detail::JournalAccess; // findRecoverableJournals constructs it.
    RecoveredJournal(Database database, bool endedCleanly, bool damaged) noexcept;

    Database m_database;
    std::string m_path;
    bool m_endedCleanly = false;
    bool m_damaged = false;
};

// A journal the scan left alone, and why.
struct SkippedJournal {
    std::string path;
    Error reason;
};

struct RecoveryScan {
    std::vector<RecoveredJournal> journals; // Locked by this process until discarded or destroyed.
    std::size_t inUse = 0;                  // Busy after lockWait: a live owner.
    std::vector<Quarantined> unreadable;    // Corrupt (not a database, foreign): moved aside.
    std::vector<SkippedJournal>
        skipped;               // Unsupported (a newer FrostIDE's journal), PermissionDenied, IoError: untouched.
    std::size_t abandoned = 0; // Fresh files left by a create() that never finished: removed.
};

/*
 * Lists the regular files in `directory` named exactly
 * journal-<yyyymmddTHHMMSSfffZ>-<16 lowercase hex>.db (so -wal, -shm, -journal and .corrupt-*
 * names never match, and links and other types are ignored) and skips `skipPath` (this
 * process's own journal, if it already exists). Each is opened with exclusiveLock,
 * allowFresh=false, a 256 KiB cache and busyTimeout = lockWait, then quick-checked (damaged()
 * rather than quarantine, so entries can be salvaged by checksum). Busy counts as inUse;
 * NotFound from allowFresh is an abandoned create and is removed; Corrupt is quarantined;
 * everything else goes to `skipped` and the scan continues. Journals that vanish while scanning
 * (another instance discarded them) are ignored. When several recovered journals hold the same
 * documentKey, the entry with the latest updatedAt is the newest.
 * Errors: only those of listing the directory; a missing directory gives an empty scan. Never
 * run it before the first frame; it waits up to lockWait per live journal. Budget: B1.
 */
[[nodiscard]] Result<RecoveryScan> findRecoverableJournals(std::string_view directory,
                                                           IClock::Duration lockWait = std::chrono::milliseconds(500),
                                                           std::string_view skipPath = {});

} // namespace frost::storage
