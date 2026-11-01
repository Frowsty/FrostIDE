// SPDX-License-Identifier: Apache-2.0
//
// What storage_test_child and the crash, concurrency and stress tests agree on: the schema of the
// transaction and bank stores, and the deterministic content of every row, journal generation and
// history record, so the parent can recompute what the child wrote. Header-only, no gtest, so the
// child can use it too. Test code only.
#pragma once

#include "frost/storage/fingerprint.hpp"
#include "frost/storage/schema.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>

namespace frost::storage::test {

/*
 * The crash store ('FrCT'): txn-loop transactions write `rows` rows into crash_data and one
 * crash_ledger row with the row count and the XXH3-64 of the rows' values in order, all in one
 * transaction; bank-writer moves money between 16 accounts that always sum to kBankTotal.
 */
inline constexpr std::uint32_t kCrashStoreId = 0x46724354;
inline constexpr Migration kCrashMigrations[] = {
    {1,
     "CREATE TABLE crash_data (txn INTEGER NOT NULL, row INTEGER NOT NULL, value BLOB NOT NULL, "
     "PRIMARY KEY (txn, row)) STRICT;"
     "CREATE TABLE crash_ledger (txn INTEGER PRIMARY KEY, rows INTEGER NOT NULL, checksum INTEGER NOT NULL) STRICT;"
     "CREATE TABLE bank (id INTEGER PRIMARY KEY, balance INTEGER NOT NULL) STRICT;",
     nullptr},
};
inline constexpr SchemaSpec kCrashSchema{"crash test store", kCrashStoreId, {}, kCrashMigrations};

inline constexpr int kBankAccounts = 16;
inline constexpr std::int64_t kBankTotal = kBankAccounts * 1000;

// Bytes from a seeded generator; equal arguments give equal bytes.
inline std::string seededBytes(std::uint64_t seed, std::size_t size) {
    std::mt19937_64 rng(seed);
    std::string out(size, '\0');
    std::size_t i = 0;
    while (i < size) {
        std::uint64_t word = rng();
        for (int b = 0; b < 8 && i < size; ++b, ++i) {
            out[i] = static_cast<char>(word & 0xFF);
            word >>= 8;
        }
    }
    return out;
}

inline std::uint64_t mix(std::uint64_t seed, std::uint64_t a, std::uint64_t b = 0) {
    // splitmix64 finaliser (public domain, Sebastiano Vigna) over the combined inputs.
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (a + 1) + 0xBF58476D1CE4E5B9ULL * (b + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// The value of row `row` of transaction `txn`: 40 to 167 bytes.
inline std::string rowValue(std::uint64_t seed, std::int64_t txn, int row) {
    const std::uint64_t key = mix(seed, static_cast<std::uint64_t>(txn), static_cast<std::uint64_t>(row));
    return seededBytes(key, 40 + static_cast<std::size_t>(key % 128));
}

// The ledger checksum of a transaction: XXH3-64 over its row values in row order.
inline std::int64_t ledgerChecksum(std::uint64_t seed, std::int64_t txn, int rows) {
    Xxh3Stream64 hash;
    for (int row = 0; row < rows; ++row) {
        hash.update(rowValue(seed, txn, row));
    }
    return static_cast<std::int64_t>(hash.digest());
}

// The content of journal generation `generation` (journal-put-loop).
inline std::string journalContent(std::uint64_t seed, std::int64_t generation, std::size_t size) {
    return seededBytes(mix(seed, static_cast<std::uint64_t>(generation), 0x4A), size);
}

// The content of history record `n` (history-record-loop): 0 to 300 KiB, so some span chunks.
inline std::string historyContent(std::uint64_t seed, std::int64_t n) {
    const std::uint64_t key = mix(seed, static_cast<std::uint64_t>(n), 0x48);
    return seededBytes(key, static_cast<std::size_t>(key % (300 * 1024)));
}

// The path key of history record `n`: five paths in turn.
inline std::string historyKey(std::int64_t n) { return std::format("src/file{}.cpp", n % 5); }

} // namespace frost::storage::test
