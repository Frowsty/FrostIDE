// SPDX-License-Identifier: Apache-2.0
//
// LocalHistory: recording, reading and pruning revisions (see frost/storage/local_history.hpp
// for schema v1, content addressing and the retention rules).

#include "frost/storage/local_history.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/storage/schema.hpp"

#include "chunks.hpp"
#include "database_internal.hpp"
#include "sql_util.hpp"
#include "test_hooks.hpp"
#include "time_names.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace frost::storage {

namespace {

using detail::Blob;

constexpr std::uint32_t kHistoryApplicationId = 0x46724C48; // 'FrLH'

// Schema v1. tests/storage/goldens/local_history.v1.schema.txt pins it: change it only with a
// new migration.
constexpr std::string_view kHistorySchemaV1 = R"sql(
CREATE TABLE lh_blob (id INTEGER PRIMARY KEY AUTOINCREMENT, fingerprint BLOB NOT NULL CHECK (length(fingerprint) = 16),
  size INTEGER NOT NULL CHECK (size >= 0), chunk_count INTEGER NOT NULL CHECK (chunk_count >= 0)) STRICT;
CREATE INDEX lh_blob_fingerprint ON lh_blob (fingerprint, size);
CREATE TABLE lh_chunk (blob_id INTEGER NOT NULL REFERENCES lh_blob(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL CHECK (seq >= 0), data BLOB NOT NULL, PRIMARY KEY (blob_id, seq)) STRICT;
CREATE TABLE lh_revision (id INTEGER PRIMARY KEY AUTOINCREMENT, path_key TEXT NOT NULL CHECK (length(path_key) > 0),
  path TEXT NOT NULL, created_ns INTEGER NOT NULL, kind INTEGER NOT NULL CHECK (kind >= 1),
  label TEXT NOT NULL DEFAULT '', blob_id INTEGER NOT NULL REFERENCES lh_blob(id)) STRICT;
CREATE INDEX lh_revision_path ON lh_revision (path_key, created_ns, id);
CREATE INDEX lh_revision_path_id ON lh_revision (path_key, id);
CREATE INDEX lh_revision_blob ON lh_revision (blob_id);
)sql";

constexpr Migration kHistoryMigrations[] = {{1, kHistorySchemaV1, nullptr}};
constexpr SchemaSpec kHistorySchema{"local history", kHistoryApplicationId, "PRAGMA auto_vacuum=INCREMENTAL;",
                                    kHistoryMigrations};

// Content chunk size. Units: bytes.
constexpr std::size_t kChunkBytes = std::size_t{1} << 20;
// Deletes per IMMEDIATE transaction in prune.
constexpr std::size_t kPruneBatch = 500;

// readRevision's columns, and the join they come from.
constexpr std::string_view kRevisionSelect =
    "SELECT r.id, r.path_key, r.path, r.created_ns, r.kind, r.label, b.size, b.fingerprint";
constexpr std::string_view kRevisionFrom = "FROM lh_revision r JOIN lh_blob b ON b.id = r.blob_id";

bool validKind(std::int64_t kind) noexcept {
    return kind >= static_cast<std::int64_t>(RevisionKind::Save) &&
           kind <= static_cast<std::int64_t>(RevisionKind::AgentEdit);
}

// Reads the kRevisionSelect columns of a row.
Result<RevisionInfo> readRevision(const Statement& row) {
    RevisionInfo info;
    info.id = row.columnInt64(0);
    info.pathKey = row.columnText(1);
    info.path = row.columnText(2);
    info.createdAt = detail::fromUnixNanos(row.columnInt64(3));
    const std::int64_t kind = row.columnInt64(4);
    if (!validKind(kind)) {
        return fail(ErrorCode::Corrupt, std::format("revision {} has unknown kind {}", info.id, kind));
    }
    info.kind = static_cast<RevisionKind>(kind);
    info.label = row.columnText(5);
    const std::int64_t size = row.columnInt64(6);
    if (size < 0) {
        return fail(ErrorCode::Corrupt, std::format("revision {} has a negative size", info.id));
    }
    info.size = static_cast<std::uint64_t>(size);
    auto fingerprint = row.columnFingerprint(7);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    info.fingerprint = *fingerprint;
    return info;
}

Result<bool> blobEquals(Database& db, std::int64_t blobId, std::int64_t chunkCount,
                        std::span<const std::string_view> content, const std::stop_token& stop) {
    auto chunks = detail::prepareBound(db, "SELECT seq, data FROM lh_chunk WHERE blob_id = ?1 ORDER BY seq", blobId);
    if (!chunks) {
        return std::unexpected(std::move(chunks.error()));
    }
    return detail::chunksEqual(*chunks, chunkCount, content, stop);
}

Result<void> cancelled() { return fail(ErrorCode::Cancelled, "local history operation cancelled"); }

} // namespace

LocalHistory::LocalHistory(Database database, IClock& clock, std::optional<Quarantined> quarantined) noexcept
    : m_database(std::move(database)), m_clock(&clock), m_quarantined(std::move(quarantined)) {}

Result<LocalHistory> LocalHistory::open(std::string_view path, IClock& clock, const HistoryOptions& options) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()).withContext("open local history"));
    }
    if (auto made = io::realFileSystem().createDirectories(io::parentPath(*normalized)); !made) {
        return std::unexpected(std::move(made.error()).withContext("open local history"));
    }
    OpenOptions open;
    open.mode = OpenMode::ReadWriteCreate;
    open.synchronous = options.synchronous;
    open.verify = options.verify;
    open.cacheSizeKiB = options.cacheSizeKiB;
    open.busyTimeout = options.busyTimeout;
    open.schema = &kHistorySchema;
    open.clock = &clock;
    auto outcome = openStore(*normalized, open, CorruptPolicy::Quarantine);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    return LocalHistory(std::move(outcome->database), clock, std::move(outcome->quarantined));
}

Result<LocalHistory> LocalHistory::openInMemory(IClock& clock) {
    OpenOptions open;
    open.synchronous = Synchronous::Normal;
    open.schema = &kHistorySchema;
    auto db = Database::openInMemory(open);
    if (!db) {
        return std::unexpected(std::move(db.error()));
    }
    return LocalHistory(std::move(*db), clock, std::nullopt);
}

Result<RecordOutcome> LocalHistory::record(std::string_view pathKey, std::string_view path, RevisionKind kind,
                                           std::string_view label, std::span<const std::string_view> content,
                                           std::stop_token stop) {
    if (pathKey.empty()) {
        return fail(ErrorCode::InvalidArgument, "local history: empty path key");
    }
    if (!validKind(static_cast<std::int64_t>(kind))) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("local history: unknown revision kind {}", static_cast<int>(kind)));
    }
    if (stop.stop_requested()) {
        return std::unexpected(cancelled().error());
    }
    const std::uint64_t size = detail::totalSize(content);
    const Fingerprint128 fingerprint = xxh3Hash128Concat(content);
    Database& db = m_database;
    const InterruptHandle interrupt = db.interruptHandle();
    std::stop_callback onStop(stop, [interrupt] { interrupt.interrupt(); });
    auto transaction = db.begin(TransactionKind::Immediate);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }

    // The newest revision (highest id) of the path: an unlabelled identical save adds nothing.
    if (label.empty()) {
        auto newest = detail::prepareBound(
            db,
            std::format("{}, r.blob_id, b.chunk_count {} WHERE r.path_key = ?1 ORDER BY r.id DESC LIMIT 1",
                        kRevisionSelect, kRevisionFrom),
            pathKey);
        if (!newest) {
            return std::unexpected(std::move(newest.error()));
        }
        auto row = newest->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (*row) {
            auto info = readRevision(*newest);
            if (!info) {
                return std::unexpected(std::move(info.error()));
            }
            if (info->size == size && info->fingerprint == fingerprint) {
                auto equal = blobEquals(db, newest->columnInt64(8), newest->columnInt64(9), content, stop);
                if (!equal) {
                    return std::unexpected(std::move(equal.error()));
                }
                if (*equal) {
                    newest->reset();
                    transaction->rollback();
                    return RecordOutcome{std::move(*info), false};
                }
            }
        }
    }

    // Reuse a blob with the same key only after comparing its bytes; every candidate in turn.
    std::vector<std::pair<std::int64_t, std::int64_t>> candidates; // (id, chunk_count)
    {
        auto lookup = detail::prepareBound(
            db, "SELECT id, chunk_count FROM lh_blob WHERE fingerprint = ?1 AND size = ?2 ORDER BY id", fingerprint,
            static_cast<std::int64_t>(size));
        if (!lookup) {
            return std::unexpected(std::move(lookup.error()));
        }
        while (true) {
            auto row = lookup->step();
            if (!row) {
                return std::unexpected(std::move(row.error()));
            }
            if (!*row) {
                break;
            }
            candidates.emplace_back(lookup->columnInt64(0), lookup->columnInt64(1));
        }
    }
    std::optional<std::int64_t> blobId;
    for (const auto& [candidate, chunkCount] : candidates) {
        auto equal = blobEquals(db, candidate, chunkCount, content, stop);
        if (!equal) {
            return std::unexpected(std::move(equal.error()));
        }
        if (*equal) {
            blobId = candidate;
            break;
        }
    }
    if (!blobId) {
        if (auto inserted =
                detail::runSql(db, "INSERT INTO lh_blob (fingerprint, size, chunk_count) VALUES (?1, ?2, 0)",
                               fingerprint, static_cast<std::int64_t>(size));
            !inserted) {
            return std::unexpected(std::move(inserted.error()));
        }
        blobId = db.lastInsertRowId();
        auto insert =
            detail::prepareBound(db, "INSERT INTO lh_chunk (blob_id, seq, data) VALUES (?1, ?2, ?3)", *blobId);
        if (!insert) {
            return std::unexpected(std::move(insert.error()));
        }
        auto written = detail::writeChunks(*insert, 2, 3, content, kChunkBytes, stop);
        if (!written) {
            return std::unexpected(std::move(written.error()));
        }
        // The pieces must not change during the call; a mismatch means they did.
        FROST_ASSERT(written->size == size && written->fingerprint == fingerprint);
        if (auto counted =
                detail::runSql(db, "UPDATE lh_blob SET chunk_count = ?1 WHERE id = ?2", written->chunkCount, *blobId);
            !counted) {
            return std::unexpected(std::move(counted.error()));
        }
    }
    const IClock::SystemTime now = m_clock->systemNow();
    if (auto inserted =
            detail::runSql(db,
                           "INSERT INTO lh_revision (path_key, path, created_ns, kind, label, blob_id) "
                           "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                           pathKey, path, detail::toUnixNanos(now), static_cast<std::int64_t>(kind), label, *blobId);
        !inserted) {
        return std::unexpected(std::move(inserted.error()));
    }
    const std::int64_t revisionId = db.lastInsertRowId();
    if (stop.stop_requested()) {
        return std::unexpected(cancelled().error());
    }
    if (auto committed = transaction->commit(); !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    RevisionInfo info{revisionId, std::string(pathKey), std::string(path), now, kind, std::string(label),
                      size,       fingerprint};
    return RecordOutcome{std::move(info), true};
}

Result<std::vector<RevisionInfo>> LocalHistory::revisions(std::string_view pathKey, std::size_t limit) {
    const auto bounded = static_cast<std::int64_t>(
        std::min<std::size_t>(limit, static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
    auto statement =
        detail::prepareBound(m_database,
                             std::format("{} {} WHERE r.path_key = ?1 ORDER BY r.created_ns DESC, r.id DESC LIMIT ?2",
                                         kRevisionSelect, kRevisionFrom),
                             pathKey, bounded);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<RevisionInfo> out;
    while (true) {
        auto row = statement->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            break;
        }
        auto info = readRevision(*statement);
        if (!info) {
            return std::unexpected(std::move(info.error()));
        }
        out.push_back(std::move(*info));
    }
    return out;
}

Result<void> LocalHistory::content(std::int64_t revisionId, frost::function_ref<Result<void>(std::string_view)> sink,
                                   std::stop_token stop) {
    if (stop.stop_requested()) {
        return cancelled();
    }
    Database& db = m_database;
    const InterruptHandle interrupt = db.interruptHandle();
    std::stop_callback onStop(stop, [interrupt] { interrupt.interrupt(); });
    // One read snapshot, so a concurrent prune cannot remove the chunks between the statements.
    auto transaction = db.begin(TransactionKind::Deferred);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    std::int64_t blobId = 0;
    detail::ChunkSummary expected;
    {
        auto meta = detail::prepareBound(db,
                                         "SELECT b.id, b.size, b.chunk_count, b.fingerprint FROM lh_revision r "
                                         "JOIN lh_blob b ON b.id = r.blob_id WHERE r.id = ?1",
                                         revisionId);
        if (!meta) {
            return std::unexpected(std::move(meta.error()));
        }
        auto row = meta->step();
        if (!row) {
            return std::unexpected(std::move(row.error()));
        }
        if (!*row) {
            return fail(ErrorCode::NotFound, std::format("local history has no revision {}", revisionId));
        }
        auto fingerprint = meta->columnFingerprint(3);
        if (!fingerprint) {
            return std::unexpected(std::move(fingerprint.error()));
        }
        if (meta->columnInt64(1) < 0 || meta->columnInt64(2) < 0) {
            return fail(ErrorCode::Corrupt, std::format("revision {} has a negative size", revisionId));
        }
        blobId = meta->columnInt64(0);
        expected = {static_cast<std::uint64_t>(meta->columnInt64(1)), meta->columnInt64(2), *fingerprint};
    }
    {
        auto chunks =
            detail::prepareBound(db, "SELECT seq, data FROM lh_chunk WHERE blob_id = ?1 ORDER BY seq", blobId);
        if (!chunks) {
            return std::unexpected(std::move(chunks.error()));
        }
        if (auto read = detail::readChunks(*chunks, expected, sink, stop); !read) {
            return read;
        }
    }
    return transaction->commit();
}

Result<std::string> LocalHistory::content(std::int64_t revisionId) {
    std::string out;
    auto read = content(revisionId, [&out](std::string_view bytes) -> Result<void> {
        out.append(bytes);
        return {};
    });
    if (!read) {
        return std::unexpected(std::move(read.error()));
    }
    return out;
}

namespace {

// One revision of the prune snapshot.
struct SnapshotRevision {
    std::int64_t id;
    std::int64_t createdNs;
    std::int64_t blobId;
    std::string pathKey;
};

// Deletes `ids` with `sql` (one parameter) in IMMEDIATE batches; adds changes to `removed` and
// the RETURNING size column (when present) to `bytes`.
Result<void> deleteInBatches(Database& db, std::string_view sql, const std::vector<std::int64_t>& ids, bool returnsSize,
                             std::uint64_t& removed, std::uint64_t& bytes, const std::stop_token& stop) {
    for (std::size_t start = 0; start < ids.size(); start += kPruneBatch) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        const std::size_t end = std::min(ids.size(), start + kPruneBatch);
        auto batch = db.inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
            auto statement = db.prepare(sql);
            if (!statement) {
                return std::unexpected(std::move(statement.error()));
            }
            for (std::size_t i = start; i < end; ++i) {
                if (auto bound = statement->bindInt64(1, ids[i]); !bound) {
                    return bound;
                }
                auto row = statement->step();
                if (!row) {
                    return std::unexpected(std::move(row.error()));
                }
                if (returnsSize) {
                    if (*row) {
                        ++removed;
                        bytes += static_cast<std::uint64_t>(std::max<std::int64_t>(statement->columnInt64(0), 0));
                        auto done = statement->step(); // RETURNING yields at most one row here.
                        if (!done) {
                            return std::unexpected(std::move(done.error()));
                        }
                    }
                } else {
                    removed += static_cast<std::uint64_t>(db.changes());
                }
                statement->reset();
            }
            return {};
        });
        if (!batch) {
            return batch;
        }
    }
    return {};
}

} // namespace

Result<PruneStats> LocalHistory::prune(const RetentionPolicy& policy, std::stop_token stop) {
    if (policy.maxAge <= IClock::Duration::zero() || policy.maxTotalBytes == 0) {
        return fail(ErrorCode::InvalidArgument, "local history prune: maxAge and maxTotalBytes must be positive");
    }
    if (stop.stop_requested()) {
        return std::unexpected(cancelled().error());
    }
    Database& db = m_database;
    const InterruptHandle interrupt = db.interruptHandle();
    std::stop_callback onStop(stop, [interrupt] { interrupt.interrupt(); });
    const std::int64_t now = detail::toUnixNanos(m_clock->systemNow());

    // Snapshot: every revision and every blob, in one read transaction.
    std::vector<SnapshotRevision> revisions;
    std::unordered_map<std::int64_t, std::uint64_t> blobSizes;
    {
        auto transaction = db.begin(TransactionKind::Deferred);
        if (!transaction) {
            return std::unexpected(std::move(transaction.error()));
        }
        {
            auto statement = db.prepare("SELECT id, created_ns, blob_id, path_key FROM lh_revision");
            if (!statement) {
                return std::unexpected(std::move(statement.error()));
            }
            while (true) {
                auto row = statement->step();
                if (!row) {
                    return std::unexpected(std::move(row.error()));
                }
                if (!*row) {
                    break;
                }
                revisions.push_back({statement->columnInt64(0), statement->columnInt64(1), statement->columnInt64(2),
                                     std::string(statement->columnText(3))});
            }
            auto blobs = db.prepare("SELECT id, size FROM lh_blob");
            if (!blobs) {
                return std::unexpected(std::move(blobs.error()));
            }
            while (true) {
                auto row = blobs->step();
                if (!row) {
                    return std::unexpected(std::move(row.error()));
                }
                if (!*row) {
                    break;
                }
                blobSizes.emplace(blobs->columnInt64(0),
                                  static_cast<std::uint64_t>(std::max<std::int64_t>(blobs->columnInt64(1), 0)));
            }
        }
        if (auto ended = transaction->commit(); !ended) {
            return std::unexpected(std::move(ended.error()));
        }
    }

    // Rule 1: age, with the newest revision (highest id) of each path protected.
    std::unordered_set<std::int64_t> protectedIds;
    if (policy.keepNewestPerPath) {
        std::unordered_map<std::string_view, std::int64_t> newest;
        for (const SnapshotRevision& revision : revisions) {
            auto [it, inserted] = newest.try_emplace(revision.pathKey, revision.id);
            if (!inserted) {
                it->second = std::max(it->second, revision.id);
            }
        }
        for (const auto& [_, id] : newest) {
            protectedIds.insert(id);
        }
    }
    const std::int64_t maxAge = policy.maxAge.count();
    const std::int64_t cutoff = now < std::numeric_limits<std::int64_t>::min() + maxAge
                                    ? std::numeric_limits<std::int64_t>::min()
                                    : now - maxAge;
    std::vector<std::int64_t> removeIds;
    std::vector<const SnapshotRevision*> remaining;
    for (const SnapshotRevision& revision : revisions) {
        if (revision.createdNs < cutoff && !protectedIds.contains(revision.id)) {
            removeIds.push_back(revision.id);
        } else {
            remaining.push_back(&revision);
        }
    }
    // Rule 2: size, over the distinct blobs the remaining revisions reference.
    std::unordered_map<std::int64_t, std::size_t> references;
    std::uint64_t referencedBytes = 0;
    for (const SnapshotRevision* revision : remaining) {
        if (references[revision->blobId]++ == 0) {
            const auto size = blobSizes.find(revision->blobId);
            referencedBytes += size != blobSizes.end() ? size->second : 0;
        }
    }
    std::ranges::sort(remaining, [](const SnapshotRevision* a, const SnapshotRevision* b) {
        return a->createdNs != b->createdNs ? a->createdNs < b->createdNs : a->id < b->id;
    });
    for (const SnapshotRevision* revision : remaining) {
        if (referencedBytes <= policy.maxTotalBytes) {
            break;
        }
        if (protectedIds.contains(revision->id)) {
            continue;
        }
        removeIds.push_back(revision->id);
        if (--references[revision->blobId] == 0) {
            const auto size = blobSizes.find(revision->blobId);
            referencedBytes -= size != blobSizes.end() ? size->second : 0;
        }
    }
    // Rule 3 (snapshot part): blobs no surviving revision references.
    std::vector<std::int64_t> orphanIds;
    for (const auto& [blob, _] : blobSizes) {
        if (const auto it = references.find(blob); it == references.end() || it->second == 0) {
            orphanIds.push_back(blob);
        }
    }
    detail::g_pruneAfterSnapshot.run();

    PruneStats stats;
    std::uint64_t unusedBytes = 0;
    std::ranges::sort(removeIds);
    if (auto deleted = deleteInBatches(db, "DELETE FROM lh_revision WHERE id = ?1", removeIds, false,
                                       stats.revisionsRemoved, unusedBytes, stop);
        !deleted) {
        return std::unexpected(std::move(deleted.error()));
    }
    // Rule 3 (fresh part): the anti-join also collects orphans of an interrupted prune.
    {
        auto statement =
            db.prepare("SELECT id FROM lh_blob b WHERE NOT EXISTS (SELECT 1 FROM lh_revision WHERE blob_id = b.id)");
        if (!statement) {
            return std::unexpected(std::move(statement.error()));
        }
        while (true) {
            auto row = statement->step();
            if (!row) {
                return std::unexpected(std::move(row.error()));
            }
            if (!*row) {
                break;
            }
            orphanIds.push_back(statement->columnInt64(0));
        }
    }
    std::ranges::sort(orphanIds);
    const auto duplicates = std::ranges::unique(orphanIds);
    orphanIds.erase(duplicates.begin(), duplicates.end());
    if (auto deleted = deleteInBatches(db,
                                       "DELETE FROM lh_blob WHERE id = ?1 AND NOT EXISTS "
                                       "(SELECT 1 FROM lh_revision WHERE blob_id = ?1) RETURNING size",
                                       orphanIds, true, stats.blobsRemoved, stats.contentBytesFreed, stop);
        !deleted) {
        return std::unexpected(std::move(deleted.error()));
    }

    // Give the freed pages back, then shrink the WAL (Busy there is not an error).
    detail::Connection& connection = DatabaseAccess::connection(db);
    auto before = detail::queryInt64(connection, "PRAGMA page_count");
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (auto vacuumed = db.execute("PRAGMA incremental_vacuum"); !vacuumed) {
        return std::unexpected(std::move(vacuumed.error()));
    }
    auto after = detail::queryInt64(connection, "PRAGMA page_count");
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    stats.pagesReleased = *before > *after ? static_cast<std::uint64_t>(*before - *after) : 0;
    if (!db.path().empty()) {
        if (auto checkpointed = db.checkpoint(CheckpointMode::Truncate); !checkpointed) {
            return std::unexpected(std::move(checkpointed.error()));
        }
    }
    return stats;
}

Result<HistoryStats> LocalHistory::stats() {
    Database& db = m_database;
    auto transaction = db.begin(TransactionKind::Deferred);
    if (!transaction) {
        return std::unexpected(std::move(transaction.error()));
    }
    detail::Connection& connection = DatabaseAccess::connection(db);
    HistoryStats stats;
    struct Query {
        std::uint64_t* out;
        std::string_view sql;
    };
    std::uint64_t pages = 0;
    std::uint64_t bytesPerPage = 0;
    const Query queries[] = {
        {&stats.revisions, "SELECT count(*) FROM lh_revision"},
        {&stats.blobs, "SELECT count(*) FROM lh_blob"},
        {&stats.contentBytes, "SELECT coalesce(sum(size), 0) FROM lh_blob"},
        {&pages, "PRAGMA page_count"},
        {&bytesPerPage, "PRAGMA page_size"},
    };
    for (const Query& query : queries) {
        auto value = detail::queryInt64(connection, query.sql);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        *query.out = static_cast<std::uint64_t>(std::max<std::int64_t>(*value, 0));
    }
    stats.fileBytes = pages * bytesPerPage;
    if (auto ended = transaction->commit(); !ended) {
        return std::unexpected(std::move(ended.error()));
    }
    return stats;
}

} // namespace frost::storage
