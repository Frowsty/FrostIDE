# SPDX-License-Identifier: Apache-2.0
#
# frost::storage (PLAN.md section 2 "Storage and caching", section 3.6; ADR 0014): the SQLite
# 3.54.1 WAL wrapper with a Result API, XXH3 fingerprints (xxHash 0.8.4), the hot-exit journal
# and the local-history store. Qt-free; depends on io (and core through it).
# SQLite and xxHash are linked PRIVATE, as io does with bcrypt, so no layer above storage sees
# sqlite3.h or xxhash.h; FrostLayers' deferred pass checks the $<LINK_ONLY:...> entries a static
# library propagates.
# TODO(M2): FSEG segments, the cache-key path normaliser and ProjectModelCache join here.
# TODO(M3): the cache disk budget and LRU (I11), including quarantined copies.

frost_add_library(storage LAYER storage
    SOURCES
        src/chunks.cpp
        src/database.cpp
        src/database_files.cpp
        src/fingerprint.cpp
        src/hot_exit_journal.cpp
        src/local_history.cpp
        src/schema.cpp
        src/sqlite_error.cpp
        src/sqlite_init.cpp
        src/statement.cpp
        src/store.cpp
        src/time_names.cpp
    DEPS frost::io)
target_link_libraries(frost_storage PRIVATE unofficial::sqlite3::sqlite3 xxHash::xxhash)
