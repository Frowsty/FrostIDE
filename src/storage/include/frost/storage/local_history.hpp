// SPDX-License-Identifier: Apache-2.0
//
// LocalHistory: the local-history store (snapshots of saved files, content-addressed and
// checksum-verified, with retention), PLAN.md section 2 "Storage and caching" and M1 "Data
// safety"; ADR 0014.
//
/*
 * Schema v1 (application_id 0x46724C48 'FrLH', auto_vacuum=INCREMENTAL, STRICT tables):
 * lh_blob (fingerprint, size, chunk count), lh_chunk (content in seq order, cascading from its
 * blob) and lh_revision (path key, path, time, kind, label, blob). Blob and revision ids are
 * AUTOINCREMENT, so an id is never reused, even after prune deletes the newest row: an id a
 * caller holds never names other content. tests/storage/goldens/local_history.v1.schema.txt
 * pins the DDL.
 *
 * Content addressing: a blob is keyed by (XXH3-128, size), but XXH3 is not collision-resistant,
 * so an existing blob is reused only after a streaming byte comparison inside the recording
 * transaction; on a collision a new blob is stored.
 *
 * Retention (prune), over metadata loaded in one read transaction:
 *   0. maxAge and maxTotalBytes must be positive (InvalidArgument otherwise).
 *   1. Age: revisions with createdAt < now - maxAge go; with keepNewestPerPath the newest
 *      revision of each pathKey (the highest id) is protected. Revisions dated in the future are
 *      never removed by age.
 *   2. Size: while the distinct blobs that the remaining revisions reference hold more than
 *      maxTotalBytes of content, the oldest unprotected revision (createdAt, then id) goes.
 *   3. Orphans: every blob no revision references, in the snapshot or by a fresh anti-join
 *      (which also collects the orphans of an interrupted prune), is deleted with its chunks,
 *      but only while still unreferenced, so a blob another connection reused meanwhile stays.
 * Deletes run in batches of 500 per IMMEDIATE transaction; a stop request ends the prune after
 * the current batch with Cancelled (committed batches stay). Revisions recorded by other
 * connections after the snapshot are never removed by that prune. Afterwards the prune runs
 * PRAGMA incremental_vacuum and a Truncate checkpoint (Busy there is not an error).
 *
 * The store is user data and lives outside <cache>: Invalidate Caches never removes it, only an
 * explicit user choice does (ADR 0014). Every call blocks; never call one on the UI thread.
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

// Why a revision exists. Values are stored; never renumber them.
enum class RevisionKind : std::uint8_t { Save = 1, ExternalChange = 2, Labelled = 3, AgentEdit = 4 };

struct RevisionInfo {
    std::int64_t id = 0;
    std::string pathKey;
    std::string path;
    IClock::SystemTime createdAt{};
    RevisionKind kind = RevisionKind::Save;
    std::string label;
    std::uint64_t size = 0;
    Fingerprint128 fingerprint{};
};

// added = false: the content equals the newest revision of the path key and no label was given,
// so no revision was added and `revision` is that newest one.
struct RecordOutcome {
    RevisionInfo revision;
    bool added = false;
};

/*
 * Retention limits. There are no defaults on purpose: the caller passes values from settings
 * (m1/settings owns the keys; SQLite never holds settings, PLAN.md 3.6). It is not an aggregate,
 * so RetentionPolicy{} does not compile (a zero policy would mean "delete all history").
 * maxTotalBytes counts the content bytes of the distinct blobs that surviving revisions
 * reference, not the file size.
 */
struct RetentionPolicy {
    RetentionPolicy() = delete ("retention values come from settings; a zero policy would delete all history");
    constexpr RetentionPolicy(IClock::Duration maxAgeValue, std::uint64_t maxTotalBytesValue,
                              bool keepNewestPerPathValue) noexcept
        : maxAge(maxAgeValue), maxTotalBytes(maxTotalBytesValue), keepNewestPerPath(keepNewestPerPathValue) {}

    IClock::Duration maxAge;     // > 0, else prune() is InvalidArgument.
    std::uint64_t maxTotalBytes; // > 0, else prune() is InvalidArgument.
    bool keepNewestPerPath;
};

struct PruneStats {
    std::uint64_t revisionsRemoved = 0;
    std::uint64_t blobsRemoved = 0;
    std::uint64_t contentBytesFreed = 0;
    std::uint64_t pagesReleased = 0; // Pages incremental_vacuum gave back (the logical file shrank by these).
};

// fileBytes = page_count * page_size of the main file (the WAL is not counted).
struct HistoryStats {
    std::uint64_t revisions = 0;
    std::uint64_t blobs = 0;
    std::uint64_t contentBytes = 0;
    std::uint64_t fileBytes = 0;
};

struct HistoryOptions {
    Synchronous synchronous = Synchronous::Full; // Tests that are not about durability pass Normal.
    Verify verify = Verify::Header;              // QuickCheck after a runtime Corrupt (see open()).
    std::int64_t cacheSizeKiB = 2048;            // Budget: B19.
    IClock::Duration busyTimeout = std::chrono::seconds(5);
};

/*
 * One connection to a local-history store. Move-only; not thread-safe. Several LocalHistory
 * objects on one file (threads or processes) are safe: WAL, one writer at a time, readers never
 * block, and concurrent first opens and upgrades migrate exactly once.
 */
class LocalHistory {
public:
    /*
     * Opens or creates the store (and its parent directories) with
     * openStore(..., CorruptPolicy::Quarantine); a quarantined file is reported through
     * quarantined(). A Corrupt error from any later call means the file is damaged: destroy this
     * object and reopen with Verify::QuickCheck, which quarantines it. Errors: as openStore
     * (Busy when quarantine is needed but the file is open elsewhere), and those of creating the
     * directory.
     */
    [[nodiscard]] static Result<LocalHistory> open(std::string_view path, IClock& clock = systemClock(),
                                                   const HistoryOptions& options = {});
    // An in-memory store for tests (the retention property test). Errors: as Database::openInMemory.
    [[nodiscard]] static Result<LocalHistory> openInMemory(IClock& clock);

    LocalHistory(LocalHistory&&) noexcept = default;
    LocalHistory& operator=(LocalHistory&&) noexcept = default;
    LocalHistory(const LocalHistory&) = delete ("a connection has one owner");
    LocalHistory& operator=(const LocalHistory&) = delete ("a connection has one owner");
    ~LocalHistory() = default;

    [[nodiscard]] const std::optional<Quarantined>& quarantined() const noexcept { return m_quarantined; }

    /*
     * Snapshots the bytes of a file as saved (encoded, BOM and CRLF included, so a revert is
     * byte-exact), the concatenation of `content`. Runs in one IMMEDIATE transaction; blobs are
     * shared by content as the header comment describes. "Newest revision" means the highest id
     * of pathKey. The time comes from clock.systemNow(). O(size). Cancelled on a stop request.
     * Budget: B21. Errors: InvalidArgument (empty pathKey, unknown kind), Busy, IoError, Corrupt,
     * Cancelled.
     */
    [[nodiscard]] Result<RecordOutcome> record(std::string_view pathKey, std::string_view path, RevisionKind kind,
                                               std::string_view label, std::span<const std::string_view> content,
                                               std::stop_token stop = {});

    // Newest first (createdAt descending, then id descending), at most `limit`. O(log n + k).
    // Errors: Corrupt (an unknown kind was read), Busy, IoError.
    [[nodiscard]] Result<std::vector<RevisionInfo>> revisions(std::string_view pathKey, std::size_t limit = SIZE_MAX);

    /*
     * Streams a revision's content to `sink` in O(chunk) memory and verifies size, chunk count
     * and checksum at the end (the sink must discard what it received on Corrupt). Errors:
     * NotFound, Corrupt, Cancelled, a sink error, IoError.
     */
    [[nodiscard]] Result<void> content(std::int64_t revisionId,
                                       frost::function_ref<Result<void>(std::string_view)> sink,
                                       std::stop_token stop = {});
    // The whole content in memory; as above.
    [[nodiscard]] Result<std::string> content(std::int64_t revisionId);

    // Applies `policy` with now = clock.systemNow(), as the header comment describes.
    // Errors: InvalidArgument (zero limits), Cancelled, Busy, IoError, Corrupt.
    [[nodiscard]] Result<PruneStats> prune(const RetentionPolicy& policy, std::stop_token stop = {});
    [[nodiscard]] Result<HistoryStats> stats();

    // For tests (checkpoints, white-box corruption).
    [[nodiscard]] Database& database() noexcept { return m_database; }

private:
    LocalHistory(Database database, IClock& clock, std::optional<Quarantined> quarantined) noexcept;

    Database m_database;
    IClock* m_clock;
    std::optional<Quarantined> m_quarantined;
};

} // namespace frost::storage
