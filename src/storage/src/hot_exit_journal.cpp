// SPDX-License-Identifier: Apache-2.0
//
// HotExitJournal, RecoveredJournal and findRecoverableJournals (see
// frost/storage/hot_exit_journal.hpp for the design and schema v1).

#include "frost/storage/hot_exit_journal.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/storage/schema.hpp"

#include "chunks.hpp"
#include "database_files.hpp"
#include "database_internal.hpp"
#include "sql_util.hpp"
#include "sqlite_init.hpp"
#include "test_hooks.hpp"
#include "time_names.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace frost::storage {

namespace {

using detail::Blob;
using detail::Null;

constexpr std::uint32_t kJournalApplicationId = 0x4672484A; // 'FrHJ'

// Schema v1. tests/storage/goldens/hot_exit_journal.v1.schema.txt pins it: change it only with a
// new migration.
constexpr std::string_view kJournalSchemaV1 = R"sql(
CREATE TABLE journal_state (id INTEGER PRIMARY KEY CHECK (id = 1), created_ns INTEGER NOT NULL,
  clean_shutdown INTEGER NOT NULL CHECK (clean_shutdown IN (0, 1)), updated_ns INTEGER NOT NULL) STRICT;
INSERT INTO journal_state (id, created_ns, clean_shutdown, updated_ns) VALUES (1, 0, 0, 0);
CREATE TABLE journal_entry (id INTEGER PRIMARY KEY,
  document_key TEXT NOT NULL UNIQUE CHECK (length(document_key) > 0), path TEXT NOT NULL,
  generation INTEGER NOT NULL CHECK (generation >= 0), updated_ns INTEGER NOT NULL,
  encoding TEXT NOT NULL, has_bom INTEGER NOT NULL CHECK (has_bom IN (0, 1)), line_ending TEXT NOT NULL,
  base_size INTEGER, base_mtime_ns INTEGER,
  base_fingerprint BLOB CHECK (base_fingerprint IS NULL OR length(base_fingerprint) = 16),
  content_size INTEGER NOT NULL CHECK (content_size >= 0),
  content_fingerprint BLOB NOT NULL CHECK (length(content_fingerprint) = 16),
  chunk_count INTEGER NOT NULL CHECK (chunk_count >= 0),
  CHECK ((base_size IS NULL) = (base_fingerprint IS NULL) AND (base_size IS NULL) = (base_mtime_ns IS NULL))) STRICT;
CREATE TABLE journal_chunk (entry_id INTEGER NOT NULL REFERENCES journal_entry(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL CHECK (seq >= 0), data BLOB NOT NULL, PRIMARY KEY (entry_id, seq)) STRICT;
CREATE TABLE journal_clean (document_key TEXT PRIMARY KEY CHECK (length(document_key) > 0),
  generation INTEGER NOT NULL CHECK (generation >= 0)) STRICT, WITHOUT ROWID;
)sql";

constexpr Migration kJournalMigrations[] = {{1, kJournalSchemaV1, nullptr}};
constexpr SchemaSpec kJournalSchema{"hot-exit journal", kJournalApplicationId, {}, kJournalMigrations};

constexpr std::size_t kMinChunkBytes = std::size_t{4} << 10;
constexpr std::size_t kMaxChunkBytes = std::size_t{64} << 20;
constexpr int kCreateAttempts = 3;
// Recovery connections are short-lived. Budget: B19.
constexpr std::int64_t kRecoveryCacheKiB = 256;
// closeCleanly's removal waits at most this long in all, for the lock and (Windows) the delete:
// our own lock is gone at once, so a longer wait only means another instance's scan has the file
// open (it then removes it). Budget: B20.
constexpr IClock::Duration kCloseRemovalWait = std::chrono::milliseconds(200);

constexpr std::string_view kEntryColumns =
    "SELECT id, document_key, path, generation, updated_ns, encoding, has_bom, line_ending, base_size, "
    "base_mtime_ns, base_fingerprint, content_size, content_fingerprint FROM journal_entry";

Result<void> checkKey(std::string_view documentKey, std::int64_t generation) {
    if (documentKey.empty()) {
        return fail(ErrorCode::InvalidArgument, "hot-exit journal: empty document key");
    }
    if (generation < 0) {
        return fail(ErrorCode::InvalidArgument, std::format("hot-exit journal: negative generation {}", generation));
    }
    return {};
}

// Reads one entry row of kEntryColumns.
Result<JournalEntry> readEntry(const Statement& row) {
    JournalEntry entry;
    entry.id = row.columnInt64(0);
    entry.state.documentKey = row.columnText(1);
    entry.state.path = row.columnText(2);
    entry.state.generation = row.columnInt64(3);
    entry.updatedAt = detail::fromUnixNanos(row.columnInt64(4));
    entry.state.encoding = row.columnText(5);
    entry.state.hasBom = row.columnInt64(6) != 0;
    entry.state.lineEnding = row.columnText(7);
    if (!row.columnIsNull(8)) {
        auto fingerprint = row.columnFingerprint(10);
        if (!fingerprint) {
            return std::unexpected(std::move(fingerprint.error()));
        }
        entry.state.base = DiskBase{static_cast<std::uint64_t>(row.columnInt64(8)),
                                    detail::fromUnixNanos(row.columnInt64(9)), *fingerprint};
    }
    const std::int64_t size = row.columnInt64(11);
    if (size < 0) {
        return fail(ErrorCode::Corrupt, std::format("journal entry {} has a negative size", entry.id));
    }
    entry.contentSize = static_cast<std::uint64_t>(size);
    auto fingerprint = row.columnFingerprint(12);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    entry.contentFingerprint = *fingerprint;
    return entry;
}

Result<std::vector<JournalEntry>> listEntries(Database& db) {
    auto statement = db.prepare(std::format("{} ORDER BY document_key", kEntryColumns));
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<JournalEntry> entries;
    while (true) {
        auto row = statement->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        auto entry = readEntry(*statement);
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        entries.push_back(std::move(*entry));
    }
    return entries;
}

// The file-name pattern journal-<yyyymmddTHHMMSSfffZ>-<16 lowercase hex>.db.
bool isJournalName(std::string_view name) noexcept {
    constexpr std::string_view kPrefix = "journal-";
    constexpr std::string_view kSuffix = ".db";
    constexpr std::size_t kLength = kPrefix.size() + 19 + 1 + 16 + kSuffix.size();
    if (name.size() != kLength || !name.starts_with(kPrefix) || !name.ends_with(kSuffix)) {
        return false;
    }
    const std::string_view middle = name.substr(kPrefix.size(), 19 + 1 + 16);
    for (std::size_t i = 0; i < middle.size(); ++i) {
        const char c = middle[i];
        bool ok = false;
        if (i == 8) {
            ok = c == 'T';
        } else if (i == 18) {
            ok = c == 'Z';
        } else if (i == 19) {
            ok = c == '-';
        } else if (i < 18) {
            ok = c >= '0' && c <= '9';
        } else {
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool fileExists(const std::string& path) { return io::realFileSystem().status(path).has_value(); }

} // namespace

namespace detail {

// Friend of HotExitJournal and RecoveredJournal: constructs them.
struct JournalAccess {
    static HotExitJournal makeJournal(Database database, IClock& clock, const JournalOptions& options) noexcept {
        return HotExitJournal(std::move(database), clock, options);
    }
    static RecoveredJournal makeRecovered(Database database, bool endedCleanly, bool damaged) noexcept {
        return RecoveredJournal(std::move(database), endedCleanly, damaged);
    }
};

} // namespace detail

// ---------------------------------------------------------------------------------------------
// HotExitJournal

HotExitJournal::HotExitJournal(Database database, IClock& clock, const JournalOptions& options) noexcept
    : m_database(std::move(database)), m_clock(&clock), m_chunkBytes(options.chunkBytes) {}

Result<HotExitJournal> HotExitJournal::create(std::string_view directory, IClock& clock,
                                              const JournalOptions& options) {
    if (options.chunkBytes < kMinChunkBytes || options.chunkBytes > kMaxChunkBytes) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("hot-exit journal: chunkBytes {} outside [4 KiB, 64 MiB]", options.chunkBytes));
    }
    if (auto initialized = detail::initializeSqlite(); !initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    auto dir = io::normalizePath(directory);
    if (!dir) {
        return std::unexpected(std::move(dir.error()).withContext("create hot-exit journal"));
    }
    if (auto made = io::realFileSystem().createDirectories(*dir); !made) {
        return std::unexpected(std::move(made.error()).withContext("create hot-exit journal"));
    }
    OpenOptions open;
    open.mode = OpenMode::ReadWriteCreate;
    open.synchronous = options.synchronous;
    open.exclusiveLock = true;
    open.schema = &kJournalSchema;
    open.vfs = options.vfs;
    open.clock = &clock;
    std::optional<Error> lastError;
    for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
        std::uint64_t random = 0;
        sqlite3_randomness(sizeof(random), &random);
        const std::string name =
            std::format("journal-{}-{:016x}.db", detail::utcStamp(clock.systemNow(), true), random);
        auto path = io::joinPath(*dir, name);
        if (!path) {
            return std::unexpected(std::move(path.error()).withContext("create hot-exit journal"));
        }
        // A journal that a scan could not quarantine (its copy's name too long) is never created.
        if (auto room = detail::checkQuarantineRoom(*path, options.vfs); !room) {
            return std::unexpected(std::move(room.error()).withContext("create hot-exit journal"));
        }
        if (fileExists(*path)) {
            lastError = Error(ErrorCode::AlreadyExists, std::format("'{}' exists", *path));
            continue;
        }
        /*
         * Until its first lock, the new file is empty and unlocked, exactly like an abandoned
         * create, so a recovery scan in another instance may remove it (POSIX lets the unlink
         * through; on Windows our open handle makes it fail). SQLite then refuses the switch to
         * WAL (SQLITE_READONLY_DBMOVED, an IoError); a VFS without that check would let this
         * connection write to an unlinked inode and lose everything at exit. Either way the name
         * is gone, and since nobody else ever creates it, its existence once we hold the lock
         * proves the file is ours. When it is gone, drop the companions SQLite created by name
         * and start again under a new name.
         */
        auto db = Database::open(*path, open);
        if (!db && !fileExists(*path)) {
            static_cast<void>(detail::removeFiles(*path, kCloseRemovalWait, options.vfs));
            lastError = std::move(db.error());
            continue;
        }
        if (!db) {
            if (db.error().code() == ErrorCode::Busy || db.error().code() == ErrorCode::Corrupt) {
                lastError = std::move(db.error()); // Someone else's file appeared under this name.
                continue;
            }
            return std::unexpected(std::move(db.error()).withContext("create hot-exit journal"));
        }
        // The file is ours only if this connection initialised it: created_ns is still 0.
        const std::int64_t now = detail::toUnixNanos(clock.systemNow());
        auto claimed = detail::runSql(*db,
                                      "UPDATE journal_state SET created_ns = ?1, updated_ns = ?1 "
                                      "WHERE id = 1 AND created_ns = 0",
                                      now);
        if (!claimed) {
            return std::unexpected(std::move(claimed.error()).withContext("create hot-exit journal"));
        }
        if (*claimed != 1) {
            lastError = Error(ErrorCode::AlreadyExists, std::format("'{}' was not fresh", *path));
            continue; // Not ours: close without touching it further.
        }
        // The lock is held from here on, so the name cannot vanish any more (see above).
        if (!fileExists(*path)) {
            static_cast<void>(db->close());
            static_cast<void>(detail::removeFiles(*path, kCloseRemovalWait, options.vfs));
            lastError = Error(ErrorCode::Busy, std::format("'{}' was removed by a recovery scan", *path));
            continue;
        }
        return detail::JournalAccess::makeJournal(std::move(*db), clock, options);
    }
    return fail(ErrorCode::AlreadyExists,
                std::format("create hot-exit journal in '{}': no fresh name after {} attempts ({})", *dir,
                            kCreateAttempts, lastError ? lastError->message() : std::string()));
}

Result<PutOutcome> HotExitJournal::put(const DocumentState& state, std::span<const std::string_view> content,
                                       std::stop_token stop) {
    if (auto valid = checkKey(state.documentKey, state.generation); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "hot-exit journal put cancelled");
    }
    Database& db = m_database;
    const InterruptHandle interrupt = db.interruptHandle();
    std::stop_callback onStop(stop, [interrupt] { interrupt.interrupt(); });
    auto transaction = db.begin(TransactionKind::Immediate);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    auto stored = detail::queryOptionalInt(db, "SELECT generation FROM journal_entry WHERE document_key = ?1",
                                           std::string_view(state.documentKey));
    if (!stored) {
        return std::unexpected(std::move(stored.error()));
    }
    auto clean = detail::queryOptionalInt(db, "SELECT generation FROM journal_clean WHERE document_key = ?1",
                                          std::string_view(state.documentKey));
    if (!clean) {
        return std::unexpected(std::move(clean.error()));
    }
    if ((*stored && state.generation <= **stored) || (*clean && state.generation <= **clean)) {
        transaction->rollback();
        return PutOutcome::Stale;
    }
    const std::int64_t now = detail::toUnixNanos(m_clock->systemNow());
    std::optional<std::int64_t> baseSize;
    std::optional<std::int64_t> baseMtime;
    std::optional<Fingerprint128> baseFingerprint;
    if (state.base) {
        baseSize = static_cast<std::int64_t>(state.base->size);
        baseMtime = detail::toUnixNanos(state.base->mtime);
        baseFingerprint = state.base->fingerprint;
    }
    // Upsert the metadata with a placeholder content description, then replace the chunks.
    auto upsert = detail::prepareBound(
        db,
        "INSERT INTO journal_entry (document_key, path, generation, updated_ns, encoding, has_bom, line_ending, "
        "base_size, base_mtime_ns, base_fingerprint, content_size, content_fingerprint, chunk_count) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 0, zeroblob(16), 0) "
        "ON CONFLICT (document_key) DO UPDATE SET path = excluded.path, generation = excluded.generation, "
        "updated_ns = excluded.updated_ns, encoding = excluded.encoding, has_bom = excluded.has_bom, "
        "line_ending = excluded.line_ending, base_size = excluded.base_size, "
        "base_mtime_ns = excluded.base_mtime_ns, base_fingerprint = excluded.base_fingerprint "
        "RETURNING id",
        std::string_view(state.documentKey), std::string_view(state.path), state.generation, now,
        std::string_view(state.encoding), std::int64_t{state.hasBom ? 1 : 0}, std::string_view(state.lineEnding),
        baseSize, baseMtime, baseFingerprint);
    if (!upsert) {
        return std::unexpected(std::move(upsert.error()));
    }
    auto returned = upsert->step();
    if (!returned) {
        return std::unexpected(std::move(returned.error()));
    }
    if (!*returned) {
        return fail(ErrorCode::Internal, "journal upsert returned no id");
    }
    const std::int64_t entryId = upsert->columnInt64(0);
    upsert->reset();
    if (auto removed = detail::runSql(db, "DELETE FROM journal_chunk WHERE entry_id = ?1", entryId); !removed) {
        return std::unexpected(std::move(removed.error()));
    }
    auto insert =
        detail::prepareBound(db, "INSERT INTO journal_chunk (entry_id, seq, data) VALUES (?1, ?2, ?3)", entryId);
    if (!insert) {
        return std::unexpected(std::move(insert.error()));
    }
    auto written = detail::writeChunks(*insert, 2, 3, content, m_chunkBytes, stop);
    if (!written) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto described = detail::runSql(db,
                                        "UPDATE journal_entry SET content_size = ?1, content_fingerprint = ?2, "
                                        "chunk_count = ?3 WHERE id = ?4",
                                        static_cast<std::int64_t>(written->size), written->fingerprint,
                                        written->chunkCount, entryId);
        !described) {
        return std::unexpected(std::move(described.error()));
    }
    if (auto touched = detail::runSql(db, "UPDATE journal_state SET updated_ns = ?1 WHERE id = 1", now); !touched) {
        return std::unexpected(std::move(touched.error()));
    }
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "hot-exit journal put cancelled");
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return PutOutcome::Written;
}

Result<bool> HotExitJournal::markClean(std::string_view documentKey, std::int64_t generation) {
    if (auto valid = checkKey(documentKey, generation); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    Database& db = m_database;
    auto transaction = db.begin(TransactionKind::Immediate);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    auto stored =
        detail::queryOptionalInt(db, "SELECT generation FROM journal_entry WHERE document_key = ?1", documentKey);
    if (!stored) {
        return std::unexpected(std::move(stored.error()));
    }
    bool cleared = true;
    if (*stored) {
        if (**stored <= generation) {
            if (auto removed = detail::runSql(db, "DELETE FROM journal_entry WHERE document_key = ?1", documentKey);
                !removed) {
                return std::unexpected(std::move(removed.error()));
            }
        } else {
            cleared = false;
        }
    }
    if (auto marked = detail::runSql(db,
                                     "INSERT INTO journal_clean (document_key, generation) VALUES (?1, ?2) "
                                     "ON CONFLICT (document_key) DO UPDATE SET "
                                     "generation = max(generation, excluded.generation)",
                                     documentKey, generation);
        !marked) {
        return std::unexpected(std::move(marked.error()));
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return cleared;
}

Result<bool> HotExitJournal::forget(std::string_view documentKey) {
    if (auto valid = checkKey(documentKey, 0); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    Database& db = m_database;
    auto transaction = db.begin(TransactionKind::Immediate);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    auto entries = detail::runSql(db, "DELETE FROM journal_entry WHERE document_key = ?1", documentKey);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    auto marks = detail::runSql(db, "DELETE FROM journal_clean WHERE document_key = ?1", documentKey);
    if (!marks) {
        return std::unexpected(std::move(marks.error()));
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    return *entries + *marks > 0;
}

Result<std::vector<JournalEntry>> HotExitJournal::entries() { return listEntries(m_database); }

Result<void> HotExitJournal::closeCleanly() {
    FROST_PRE(m_database.isOpen());
    Database& db = m_database;
    const std::string path = db.path();
    const std::string vfs = DatabaseAccess::connection(db).vfsName;
    auto marked = detail::runSql(db, "UPDATE journal_state SET clean_shutdown = 1, updated_ns = ?1 WHERE id = 1",
                                 detail::toUnixNanos(m_clock->systemNow()));
    auto remaining = marked ? detail::queryInt64(DatabaseAccess::connection(db), "SELECT count(*) FROM journal_entry")
                            : Result<std::int64_t>(std::unexpected(marked.error()));
    // Close whatever happened above: the object is unusable afterwards.
    auto closed = db.close();
    if (!marked) {
        return std::unexpected(std::move(marked.error()).withContext("close hot-exit journal"));
    }
    if (!remaining) {
        return std::unexpected(std::move(remaining.error()).withContext("close hot-exit journal"));
    }
    if (!closed) {
        return std::unexpected(std::move(closed.error()).withContext("close hot-exit journal"));
    }
    if (*remaining == 0) {
        return detail::removeFiles(path, kCloseRemovalWait, vfs);
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// RecoveredJournal

RecoveredJournal::RecoveredJournal(Database database, bool endedCleanly, bool damaged) noexcept
    : m_database(std::move(database)), m_path(m_database.path()), m_endedCleanly(endedCleanly), m_damaged(damaged) {}

Result<std::vector<JournalEntry>> RecoveredJournal::entries() { return listEntries(m_database); }

Result<void> RecoveredJournal::readContent(std::int64_t entryId,
                                           frost::function_ref<Result<void>(std::string_view)> sink,
                                           std::stop_token stop) {
    if (stop.stop_requested()) {
        return fail(ErrorCode::Cancelled, "journal read cancelled");
    }
    Database& db = m_database;
    const InterruptHandle interrupt = db.interruptHandle();
    std::stop_callback onStop(stop, [interrupt] { interrupt.interrupt(); });
    auto transaction = db.begin(TransactionKind::Deferred);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    detail::ChunkSummary expected;
    {
        auto meta = detail::prepareBound(
            db, "SELECT content_size, content_fingerprint, chunk_count FROM journal_entry WHERE id = ?1", entryId);
        if (!meta) {
            return std::unexpected(std::move(meta.error()));
        }
        auto row = meta->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::NotFound, std::format("journal '{}' has no entry {}", m_path, entryId));
        }
        auto fingerprint = meta->columnFingerprint(1);
        if (!fingerprint) {
            return std::unexpected(std::move(fingerprint.error()));
        }
        if (meta->columnInt64(0) < 0 || meta->columnInt64(2) < 0) {
            return fail(ErrorCode::Corrupt, std::format("journal entry {} has a negative size", entryId));
        }
        expected = {static_cast<std::uint64_t>(meta->columnInt64(0)), meta->columnInt64(2), *fingerprint};
    }
    {
        auto chunks =
            detail::prepareBound(db, "SELECT seq, data FROM journal_chunk WHERE entry_id = ?1 ORDER BY seq", entryId);
        if (!chunks) {
            return std::unexpected(std::move(chunks.error()));
        }
        if (auto read = detail::readChunks(*chunks, expected, sink, stop); !read) {
            return read;
        }
    }
    // A read transaction: committing only releases the snapshot.
    return transaction->commit();
}

Result<std::string> RecoveredJournal::readContent(std::int64_t entryId) {
    std::string out;
    auto read = readContent(entryId, [&out](std::string_view bytes) -> Result<void> {
        out.append(bytes);
        return {};
    });
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    return out;
}

Result<void> RecoveredJournal::discard() && {
    FROST_PRE(m_database.isOpen());
    Database& db = m_database;
    const std::string vfs = DatabaseAccess::connection(db).vfsName;
    auto emptied = db.inTransaction(TransactionKind::Immediate, [&db]() -> Result<void> {
        return db.execute("DELETE FROM journal_entry; DELETE FROM journal_clean;");
    });
    const bool corrupt = !emptied && emptied.error().code() == ErrorCode::Corrupt;
    if (!emptied && !corrupt) {
        return std::unexpected(std::move(emptied.error()).withContext("discard hot-exit journal"));
    }
    detail::g_discardAfterCommit.run();
    if (auto closed = db.close(); !closed && !corrupt && !m_damaged) {
        return std::unexpected(std::move(closed.error()).withContext("discard hot-exit journal"));
    }
    if (m_damaged || corrupt) {
        auto moved = detail::quarantineFiles(m_path, systemClock(), detail::kFileLockTimeout, vfs);
        if (!moved) {
            return std::unexpected(std::move(moved.error()).withContext("discard hot-exit journal"));
        }
        return {};
    }
    return detail::removeFiles(m_path, detail::kFileLockTimeout, vfs);
}

// ---------------------------------------------------------------------------------------------
// findRecoverableJournals

Result<RecoveryScan> findRecoverableJournals(std::string_view directory, IClock::Duration lockWait,
                                             std::string_view skipPath) {
    RecoveryScan scan;
    auto dir = io::normalizePath(directory);
    if (!dir) {
        return std::unexpected(std::move(dir.error()).withContext("scan hot-exit journals"));
    }
    std::string skip;
    if (!skipPath.empty()) {
        auto normalized = io::normalizePath(skipPath);
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()).withContext("scan hot-exit journals"));
        }
        skip = std::move(*normalized);
    }
    auto listing = io::realFileSystem().listDirectory(*dir);
    if (!listing) {
        if (listing.error().code() == ErrorCode::NotFound) {
            return scan;
        }
        return std::unexpected(std::move(listing.error()).withContext("scan hot-exit journals"));
    }
    OpenOptions open;
    open.mode = OpenMode::ReadWrite;
    open.exclusiveLock = true;
    open.allowFresh = false;
    open.cacheSizeKiB = kRecoveryCacheKiB;
    open.busyTimeout = lockWait;
    open.schema = &kJournalSchema;
    for (const io::DirEntry& entry : *listing) {
        if (entry.type != io::FileType::Regular || !isJournalName(entry.name)) {
            continue;
        }
        auto joined = io::joinPath(*dir, entry.name);
        if (!joined) {
            continue; // Unrepresentable name; cannot be one of ours.
        }
        const std::string& path = *joined;
        if (path == skip) {
            continue;
        }
        auto db = Database::open(path, open);
        if (!db) {
            Error error = std::move(db.error());
            if (!fileExists(path)) {
                continue; // Discarded by another instance meanwhile.
            }
            switch (error.code()) {
            case ErrorCode::Busy: ++scan.inUse; break;
            case ErrorCode::NotFound:
                // allowFresh=false: a create() that never finished. Remove it unless it came alive.
                if (auto removed = detail::removeFiles(path, lockWait); removed) {
                    ++scan.abandoned;
                } else if (removed.error().code() == ErrorCode::Busy) {
                    ++scan.inUse;
                } else {
                    scan.skipped.push_back({path, std::move(removed.error())});
                }
                break;
            case ErrorCode::Corrupt: {
                // Journal names are never reused (create() starts again under a new name), so
                // the file cannot have been replaced since the verdict: no re-check needed. A
                // nullopt means it vanished before our lock (another instance's scan moved it).
                auto moved = detail::quarantineFiles(path, systemClock(), lockWait);
                if (moved) {
                    if (moved->has_value()) {
                        scan.unreadable.push_back({std::move(moved->value()), std::move(error)});
                    }
                } else if (moved.error().code() == ErrorCode::Busy) {
                    ++scan.inUse;
                } else {
                    scan.skipped.push_back({path, std::move(moved.error())});
                }
                break;
            }
            default: scan.skipped.push_back({path, std::move(error)}); break;
            }
            continue;
        }
        const bool damaged = !db->quickCheck();
        bool endedCleanly = false;
        if (auto flag = detail::queryOptionalInt(*db, "SELECT clean_shutdown FROM journal_state WHERE id = 1"); flag) {
            endedCleanly = flag->value_or(0) == 1;
        }
        scan.journals.push_back(detail::JournalAccess::makeRecovered(std::move(*db), endedCleanly, damaged));
    }
    return scan;
}

} // namespace frost::storage
