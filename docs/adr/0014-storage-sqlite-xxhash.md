# ADR 0014: frost::storage on SQLite 3.54.1 and xxHash 0.8.4

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 "Storage and caching", 2.2 "Data safety" and "Privacy and network", 3.6
  (settings are JSON, never SQLite), 6.1 (fault injection, concurrency, golden tests, fuzzing); milestone M1
  "Data safety". This is the dependency ADR for sqlite3 and xxhash (CLAUDE.md "Licences").

## Context
- **No SQLite 3.54.1 in the vcpkg baseline.** `E:\sdk\vcpkg` at 9eec002 (2026.10.16, our `builtin-baseline`)
  stops at sqlite3 3.53.4#2. sqlite.org released 3.54.1 on 2026-10-16 (3.54.0: SRW-lock mutexes on Windows,
  XP/CE dropped, about 4% faster, a rewritten CLI; 3.54.1: a heap overflow on 32-bit builds only). ADR 0009
  found that `overrides` fail against the shallow vcpkg clone, and 3.54.1 is not in the versions database
  anyway. PLAN pins 3.54.1, so it needs an overlay port.
- **The port exposes features, not compile options.** Debug builds get `SQLITE_DEBUG=1`, whose `assert()`s
  fire on API misuse, so no test misuses the API on purpose and the wrapper's FROST_PRE checks run first.
  `sqlite3-vcpkg-config.h` always defines SQLITE_ENABLE_UNLOCK_NOTIFY and SQLITE_ENABLE_COLUMN_METADATA;
  nothing depends on either. Hardening is therefore done at run time.
- **Long paths on Windows.** SQLite's `win32-longpath` VFS has an mxPathname of about 64 KiB and passes
  `\\?\` names through; it calls only the W file APIs. The unix VFS's MAX_PATHNAME is 512 bytes.
- **POSIX advisory locks belong to the process and the inode**: closing any descriptor of a database file
  drops every fcntl lock the process holds on it ("How To Corrupt An SQLite Database File", 2.2). Reading a
  live database with `io::readFile` would silently unlock our own connection.
- **WAL hooks and commit results.** `PRAGMA wal_autocheckpoint`'s default hook reports a failed checkpoint
  as a failed COMMIT, although the commit happened (sqlite3_wal_hook).
- **SQLite does not tie a WAL to its database.** A self-consistent -wal beside a non-empty database is
  replayed whatever wrote it; a -wal beside an empty or missing database is deleted on the first read.
- **SQLite's unix VFS reports a 1-byte file as empty** (os_unix.c, ticket #3260); the Windows VFS calls it
  "not a database". The unix VFS also asserts, in SQLITE_DEBUG builds, that one xRead is under 128 KiB.
- **SQLite's busy handler is not called for a read-to-write upgrade** (btree.c, sqlite3BtreeBeginTrans),
  which is what the first `PRAGMA journal_mode=WAL` on a rollback-mode file does.
- **Layering.** PLAN 3.2 puts SaveManager in `src/text`, the same FrostLayers rank (4) as storage, and
  equal ranks may not depend on each other (question 1 below).
- **Tooling gaps.** `tools/perf.ps1 -Smoke` (PLAN section 4) and a coverage leg (PLAN 6.1, 80%) do not exist
  yet: perf-smoke is not run and coverage is not measured.

## Decision
1. **Ports.** sqlite3 3.54.1 is an overlay at `third_party/vcpkg-ports/sqlite3/`: vcpkg's port at 9eec002
   with the version bumped, the SHA512 of `sqlite-autoconf-3540100.tar.gz` (its SHA3-256 was checked against
   sqlite.org's `b6ced585…d9c4` first), `fix-arm-uwp.patch` dropped (shell.c only; the CLI is never built),
   `add-config-include.patch` kept, `sqlite3.rc` compiled only for shared builds, MIT SPDX headers and
   `LICENSE-vcpkg.txt`. `CMakePresets.json` sets `VCPKG_OVERLAY_PORTS`. `vcpkg.json`'s `version>=` 3.54.1 is a
   tripwire for the day the overlay goes: delete it at the first quarterly baseline bump that ships ≥3.54.1.
   xxhash 0.8.4 comes from the baseline. The root CMakeLists finds both (`if(NOT FROST_FUZZ)`: fuzz trees
   install no ports); `frost_storage` links them PRIVATE, so no layer above storage sees `sqlite3.h`.
2. **Licences and features.** sqlite3 is `blessing`, xxhash `BSD-2-Clause`; both are on the
   `tools/licensecheck.py` allowlist (checked on both Windows trees and in WSL). sqlite3 uses only
   `omit-load-extension`; json1 is off (SQLITE_OMIT_JSON: nothing uses it, smaller binary and attack surface;
   re-enabling it needs an update here). The `tool` feature (the CLI) stays off. xxhash's `xxhsum` feature is
   GPL-2.0-or-later and stays off; no xxHash test vector comes from its GPL tests or CLI. xxHash's notice
   reaches binary distributions through the SBOM and notices (M5).
3. **One-time initialisation and the build check.** The first open runs `sqlite3_config(MEMSTATUS, 0)` and
   `sqlite3_initialize` once, after checking the linked library: version number 3054001, threadsafe, and
   OMIT_LOAD_EXTENSION and OMIT_JSON compiled in. Anything else is Unsupported. TODO(M1): route
   SQLITE_CONFIG_LOG to frost's log once frost::core has logging.
4. **Connection rules.** `sqlite3_open_v2` with NOMUTEX (one thread at a time per connection) and EXRESCODE;
   dbconfig DEFENSIVE=1, DQS_DML=0, DQS_DDL=0, TRUSTED_SCHEMA=0, ENABLE_LOAD_EXTENSION=0 and
   NO_CKPT_ON_CLOSE = !checkpointOnClose; `journal_mode=WAL` (anything else is Unsupported), then
   `synchronous` (Full by default, Normal for tests that are not about durability), `foreign_keys=ON`,
   `cache_size` 2 MiB (B19; recovery connections 256 KiB), `journal_size_limit` 64 MiB, `temp_store=MEMORY`,
   `cell_size_check=ON`. Writes use BEGIN IMMEDIATE. The busy handler sleeps through
   sqliteDefaultBusyCallback's delays (main.c, public domain) until a steady deadline of busyTimeout (5 s by
   default) and stops early on an interrupt; `sqlite3_busy_timeout` cannot do either. On Windows the VFS is
   `win32-longpath` with `io::extendedLengthPath`; open() reads the VFS's mxPathname and refuses longer names
   (InvalidArgument). A 1-byte file is Corrupt on every platform (Context). `PRAGMA journal_mode=WAL` is
   retried on Busy until busyTimeout: switching a new rollback-mode file upgrades a read transaction, and
   SQLite does not call the busy handler for that upgrade, so racing first opens failed at once (found by
   `Schema.ConcurrentFirstOpenMigratesOnce` under WSL ASan).
5. **Our WAL hook.** After a commit that leaves at least walAutoCheckpointPages (1000) frames it runs a
   Passive checkpoint, which never waits for readers, and always returns SQLITE_OK: a checkpoint error never
   fails a commit (kept in the connection for diagnostics). An IoError from COMMIT is outcome-unknown: the
   caller retries with the same generation, and the journal answers Stale if the first attempt landed.
6. **Errors.** One table maps result codes to frost::ErrorCode (`sqlite_error.cpp`): BUSY, LOCKED, PROTOCOL,
   SCHEMA → Busy; INTERRUPT, ABORT → Cancelled; CORRUPT, NOTADB, FORMAT, IOERR_CORRUPTFS, IOERR_DATA →
   Corrupt; FULL, IOERR, NOMEM, READONLY_DBMOVED → IoError; CANTOPEN refined by the OS error (IoError when
   unknown), CANTOPEN_ISDIR and _FULLPATH → InvalidArgument; PERM, READONLY, AUTH → PermissionDenied; TOOBIG →
   OutOfRange; CONSTRAINT_UNIQUE and _PRIMARYKEY → AlreadyExists, other CONSTRAINT, MISMATCH, ERROR →
   InvalidArgument; MISUSE, RANGE, INTERNAL, EMPTY, NOTFOUND, ABORT_ROLLBACK → Internal; NOLFS → Unsupported.
   Messages name the operation, the path, SQLite's message and code name, the OS error and the first 120
   bytes of the SQL.
7. **Schema versioning.** Each store has an application_id and counts its migrations in user_version. The
   header (both ids, page count, schema rows) is read in one read transaction, so a concurrent creation is
   seen entirely or not at all. A file
   is fresh when it has no pages, or pages with application_id 0, user_version 0 and no schema rows. Refused
   without modification: a foreign application_id, or 0 on a non-fresh file, and our id with a user_version
   of 0 or below (Corrupt); a newer version, and ReadOnly on an older one (Unsupported); a fresh file with
   allowFresh=false (NotFound); with Verify::QuickCheck a failed quick_check (Corrupt) before any write. A
   refusal closes with NO_CKPT_ON_CLOSE when a -wal existed before the open, so neither file is rewritten;
   otherwise SQLite's close only removes the empty -wal and -shm a WAL-mode read creates. Inherent limit: a
   read of a WAL file may still run WAL recovery in memory. Fresh creation (fresh pragmas, a VACUUM if the
   file has pages, then every migration and both ids in one IMMEDIATE transaction) is atomic: a crash leaves
   a fresh file or a complete store. Each migration step re-reads user_version inside its own IMMEDIATE
   transaction, so concurrent first opens and upgrades migrate once.
8. **Database files only through the VFS.** Quarantine and removal take SQLite's EXCLUSIVE lock through the
   VFS (waiting up to busyTimeout, or 5 s for removeDatabaseFiles); a held lock is Busy. Copies stream in
   64 KiB blocks through the VFS (the unix VFS asserts reads under 128 KiB in SQLITE_DEBUG builds), sized by
   `io::status` rather than xFileSize (which reports a 1-byte file as empty), and are synced before anything
   is removed. Removal order: main file, -wal,
   -shm, -journal. Residual POSIX race: a process that opens the file in the instant before the unlink keeps
   an unlinked inode (on Windows SQLite opens without FILE_SHARE_DELETE, so the race fails with Busy). In
   EXCLUSIVE locking mode SQLite leaves a zeroed -journal after the switch to WAL; open() deletes it through
   the VFS.
9. **Corrupt handling.** `openStore(..., CorruptPolicy::Quarantine)` moves a Corrupt file (and its -wal) to
   `<path>.corrupt-<UTC yyyymmddTHHMMSSZ>[-n]` and opens a fresh store; Unsupported, Busy, PermissionDenied and
   IoError are never quarantined, and a ReadOnly open removes nothing. A Corrupt error from a later call means
   the file is damaged: the owner drops the connection and reopens with Verify::QuickCheck, which quarantines
   it. The verdict is reached before the lock, and a fresh store is unlocked until its first read, so another
   process that quarantined the file first could otherwise lose its new store to our removal (POSIX unlinks
   it under its owner). Under the lock the byte-identical copy is therefore judged again with the same
   options; when it is no longer Corrupt the copies are deleted, the file stays and is opened again (3
   attempts, then Busy). Journal scans skip the re-check: journal names are never reused. TODO(M3):
   quarantined copies count in the cache budget and are pruned (I11).
10. **Hot-exit journal.** One file per process,
    `journal-<UTC yyyymmddTHHMMSSfffZ>-<16 hex from sqlite3_randomness>.db`, held with locking_mode=EXCLUSIVE
    for the process's life, so ownership is an OS lock: no PID checks and no single-instance pipe, and several
    instances keep their own journals. Until its first lock a new journal is empty and unlocked, like an
    abandoned create, and a scan in another instance may remove it (POSIX); create() therefore requires the
    name to exist once it holds the lock (SQLite also refuses the WAL switch with SQLITE_READONLY_DBMOVED) and
    otherwise starts again under a new name. Each put rewrites the whole entry in one IMMEDIATE transaction, in
    chunks of 1 MiB (4 KiB to 64 MiB configurable) with an XXH3-128 checksum, size and chunk count, so a crash
    leaves the old entry or the new one. Generations are monotonic per document key; a put at or below the
    stored generation or the document's clean mark is Stale, and markClean only raises the mark. A clean
    shutdown sets clean_shutdown under the lock and removes the files when no entry remains. The recovery scan
    matches only exact journal names, skips this process's own journal, waits up to lockWait per live
    journal (inUse), removes files left by a create that never finished (abandoned), quarantines Corrupt ones,
    leaves newer-version ones untouched (skipped), and offers damaged ones (failed quick_check) for salvage by
    checksum. discard() deletes every entry while still holding the lock, then closes and removes the files,
    so a racing scan in another instance sees an empty journal, never a second copy of the buffers.
11. **Local history.** Content is addressed by (XXH3-128, size), but a blob is reused only after a streaming
    byte comparison of every candidate inside the recording transaction (XXH3 is not collision-resistant).
    An unlabelled save equal to the path's newest revision (highest id) adds nothing. Revision and blob ids
    are AUTOINCREMENT, never reused. Retention: positive maxAge and maxTotalBytes (no defaults; settings own
    them); age (`createdAt < now − maxAge`, the newest revision of each path protected when asked), then size
    (oldest unprotected first until the distinct referenced blobs fit), then orphans (the snapshot's and a
    fresh anti-join's, deleted only while still unreferenced, so a blob reused after the snapshot survives).
    Deletes go in batches of 500 per IMMEDIATE transaction; a stop request ends after a batch. Afterwards
    `PRAGMA incremental_vacuum` (the store has auto_vacuum=INCREMENTAL) and a Truncate checkpoint.
    Future-dated revisions are never removed by age; a clock running backwards only reorders, because
    protection and dedup use the id.
12. **Fingerprints.** XXH3-64 and XXH3-128 with seed 0 through `XXH_STATIC_LINKING_ONLY` (streaming state on
    the heap, 64-byte aligned). The canonical form is 16 bytes, high word big-endian then low word, so memcmp
    order equals value order; hex is 32 lowercase digits. Not cryptographic: for non-adversarial keys, with
    verify-on-hit where identity matters. `tests/storage/goldens/xxh3_vectors.txt` pins the persisted format
    across vcpkg bumps (a regression golden from our own build, not an independent oracle).
13. **Crash guarantees and limits.** Process kill (TerminateProcess on the job, SIGKILL on the group): atomic
    and durable in both synchronous modes, tested. Power loss: Full is durable by SQLite's contract, but not
    simulated yet. SQLite does not detect a foreign or stale -wal paired with a database; the stores never
    create one. Network file systems are unsupported (WAL needs shared memory).
14. **Placement and privacy.** Local history and the hot-exit journal are user data: they live in a data
    directory outside `<cache>` that the caller chooses (for example `%LOCALAPPDATA%\FrostIDE\data\history`
    and `...\journal`, or under XDG_DATA_HOME), and are never removed by Invalidate Caches or M3's cache budget
    and LRU, only by an explicit user choice. `removeDatabaseFiles` is for caches. Everything stays local; no
    settings live in SQLite; file modes are the question below.
15. **Later slices.** FSEG segments, the cache-key path normaliser and ProjectModelCache are TODO(M2) in
    `src/storage/CMakeLists.txt`; the cache disk budget and LRU are TODO(M3). The journal fuzz target waits
    for a fuzz tree with ports; `CorruptionSweep` in the stress tests is its seed.

## Measurements
Indicative only: the machine was not isolated for them. REF-W1 (Windows 11, 12 cores), win-clangcl
Release, and WSL Ubuntu 24.04 linux-clang Release; `storage_stress_tests --gtest_also_run_disabled_tests
--gtest_filter=StorageMeasure.*` (tests/storage/measure_test.cpp).

Two runs each, as ranges; on WSL the store files live on ext4 inside the WSL disk image, whose syncs are
slower and noisier. Nothing here is a budget measurement: `tools/perf.ps1 -Smoke` does not exist yet.

| Operation | Windows | WSL |
|---|---|---|
| XXH3-64 of 64 MiB in memory | 2.5-2.8 ms (22-25 GB/s) | 3.3-3.9 ms (16-19 GB/s) |
| XXH3-128 of 64 MiB in memory | 2.5-2.7 ms (23-25 GB/s) | 3.4-3.7 ms (17-18 GB/s) |
| LocalHistory: create a fresh store, then close | 13.8-14.0 ms | 86-101 ms |
| LocalHistory: open an existing store | 1.1 ms | 0.2-0.3 ms |
| LocalHistory: record 1 MiB (synchronous=FULL) | 6.3-6.8 ms | 25-30 ms |
| Journal put of 1 MiB, NORMAL / FULL | 2.7 / 5.8-6.5 ms | 2.2-2.8 / 12.7-26.5 ms |
| Journal put of 64 MiB, NORMAL / FULL | 373-443 / 441-453 ms | 470-866 / 516-718 ms |
| Journal readContent of 64 MiB (checksum verified) | 45-46 ms | 26-88 ms |
| Close with about 1000 WAL pages (checkpoint) | 11 ms | 26-57 ms |
| Prune of 10,000 revisions and blobs | 118-124 ms | 193-513 ms |
| Quarantine of 256 MiB (copy, sync, re-judge, remove) | 266-287 ms | 762-1024 ms |

## Consequences
- Every later store (caches, session state, the project-model cache) gets the wrapper, schema versioning,
  quarantine and the crash tests for free; settings never move into SQLite.
- The overlay must be removed at the first baseline bump with ≥3.54.1 (the `version>=` tripwire).
- MSan needs instrumented ports, so there is no MSan leg; Windows ASan builds SQLite and xxhash with the asan
  triplet, and TSan sees SQLite's pthread mutexes through interception.
- Questions for the user (the conservative choice is implemented): (1) where SaveManager lives, given that
  text and storage share rank 4; (2) Linux file modes for the user-data stores (currently the umask's, as
  SQLite creates them); (3) whether disk full deserves its own ErrorCode rather than IoError; (4) who owns
  the journal fuzz target once fuzz trees install ports; (5) how long quarantined copies are kept before M3's
  budget prunes them (currently: until then); (6) whether Invalidate Caches should offer to clear local
  history (currently: never, it is user data).

## Alternatives considered
- **SQLite 3.53.4 from the baseline:** contradicts the PLAN pin.
- **LMDB:** rejected in PLAN.
- **`XXH_INLINE_ALL`, header-only xxHash:** would compile xxHash into every user; one library is simpler.
- **BLAKE3 or SHA-256 fingerprints:** slower and unnecessary for non-adversarial keys with verify-on-hit.
- **`sqlite3_busy_timeout`:** no interrupt check, and it sums planned delays instead of measuring time.
- **`PRAGMA wal_autocheckpoint`:** its hook turns checkpoint errors into commit errors.
- **Quarantine by readFile/writeFileAtomic** (drops POSIX locks, O(size) memory) **or by rename** (frost::io has
  no rename yet).
- **One shared journal with a session table plus PID checks:** racy before M2's single instance.
- **Per-buffer journal files without SQLite:** no transactional multi-row update.
- **Delta-compressed history:** needs a compression dependency and its own ADR.
- **SQLITE_OPEN_NOFOLLOW:** breaks symlinked profiles.
