// SPDX-License-Identifier: Apache-2.0
//
// The bank workload of the concurrency and stress tests, over the crash store's bank table
// (crash_protocol.hpp): one writer moves money between 16 accounts in IMMEDIATE transactions,
// readers check inside deferred transactions that the total never changes, and a checkpointer
// runs Passive checkpoints. Any reader error or a wrong sum is a failure. Test code only.
#pragma once

#include "frost/storage/database.hpp"

#include "support/crash_protocol.hpp"
#include "support/storage_test_support.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace frost::storage::test {

inline OpenOptions bankOptions(OpenMode mode = OpenMode::ReadWriteCreate) {
    OpenOptions options = fastOptions();
    options.mode = mode;
    options.schema = &kCrashSchema;
    options.busyTimeout = std::chrono::seconds(10);
    return options;
}

// Creates the accounts if missing.
inline Result<void> seedBank(Database& db) {
    return db.inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
        return db.execute(std::format("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < {}) "
                                      "INSERT OR IGNORE INTO bank (id, balance) SELECT i, {} FROM n",
                                      kBankAccounts, kBankTotal / kBankAccounts));
    });
}

// One transfer of `n` (deterministic from n).
inline Result<void> transfer(Database& db, std::uint64_t n) {
    const std::uint64_t z = mix(0xBA4C, n);
    const int from = 1 + static_cast<int>(z % kBankAccounts);
    const int to = 1 + static_cast<int>((z >> 8) % kBankAccounts);
    const auto amount = static_cast<std::int64_t>((z >> 16) % 100);
    return db.inTransaction(TransactionKind::Immediate, [&]() -> Result<void> {
        if (auto debit = db.execute(std::format("UPDATE bank SET balance = balance - {} WHERE id = {}", amount, from));
            !debit) {
            return debit;
        }
        return db.execute(std::format("UPDATE bank SET balance = balance + {} WHERE id = {}", amount, to));
    });
}

// The total inside one deferred transaction; an error on failure.
inline Result<std::int64_t> bankTotal(Database& db) {
    std::int64_t total = -1;
    auto read = db.inTransaction(TransactionKind::Deferred, [&]() -> Result<void> {
        auto statement = db.prepare("SELECT coalesce(sum(balance), 0), count(*) FROM bank");
        if (!statement) {
            return std::unexpected(statement.error());
        }
        auto row = statement->step();
        if (!row) {
            return std::unexpected(row.error());
        }
        total = statement->columnInt64(1) == kBankAccounts ? statement->columnInt64(0) : -statement->columnInt64(1);
        return {};
    });
    if (!read) {
        return std::unexpected(read.error());
    }
    return total;
}

/*
 * Collects failures from worker threads (gtest assertions are not used off the main thread's
 * test body here, so every message is reported once at the end).
 */
class Failures {
public:
    void add(std::string message) {
        std::lock_guard lock(m_mutex);
        if (m_messages.size() < 20) {
            m_messages.push_back(std::move(message));
        }
        m_count.fetch_add(1);
    }
    [[nodiscard]] std::size_t count() const { return m_count.load(); }
    void report() const {
        std::lock_guard lock(m_mutex);
        for (const std::string& message : m_messages) {
            ADD_FAILURE() << message;
        }
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_messages;
    std::atomic<std::size_t> m_count{0};
};

/*
 * Runs `transfers` transfers in one writer thread (or for `duration` when transfers is 0) while
 * `readers` reader threads (the first ReadOnly) check the total and one thread checkpoints.
 * Returns the number of total reads that were checked.
 */
inline std::uint64_t runBank(const std::string& path, std::uint64_t transfers, std::chrono::milliseconds duration,
                             int readers, Failures& failures) {
    {
        auto db = Database::open(path, bankOptions());
        if (!db) {
            failures.add("open: " + db.error().describe());
            return 0;
        }
        if (auto seeded = seedBank(*db); !seeded) {
            failures.add("seed: " + seeded.error().describe());
            return 0;
        }
    }
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::jthread> threads;
    for (int r = 0; r < readers; ++r) {
        threads.emplace_back([&, r] {
            auto db = Database::open(path, bankOptions(r == 0 ? OpenMode::ReadOnly : OpenMode::ReadWrite));
            if (!db) {
                failures.add(std::format("reader {} open: {}", r, db.error().describe()));
                return;
            }
            while (!done.load()) {
                auto total = bankTotal(*db);
                if (!total) {
                    failures.add(std::format("reader {}: {}", r, total.error().describe()));
                    return;
                }
                if (*total != kBankTotal) {
                    failures.add(std::format("reader {} saw total {}", r, *total));
                    return;
                }
                reads.fetch_add(1);
            }
        });
    }
    threads.emplace_back([&] {
        auto db = Database::open(path, bankOptions(OpenMode::ReadWrite));
        if (!db) {
            failures.add("checkpointer open: " + db.error().describe());
            return;
        }
        while (!done.load()) {
            if (auto checkpointed = db->checkpoint(CheckpointMode::Passive); !checkpointed) {
                failures.add("checkpoint: " + checkpointed.error().describe());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    {
        auto db = Database::open(path, bankOptions(OpenMode::ReadWrite));
        if (!db) {
            failures.add("writer open: " + db.error().describe());
        } else {
            const auto deadline = std::chrono::steady_clock::now() + duration;
            for (std::uint64_t n = 1; transfers == 0 ? std::chrono::steady_clock::now() < deadline : n <= transfers;
                 ++n) {
                if (auto moved = transfer(*db, n); !moved) {
                    failures.add(std::format("transfer {}: {}", n, moved.error().describe()));
                    break;
                }
            }
        }
    }
    done.store(true);
    threads.clear(); // Joins.
    return reads.load();
}

} // namespace frost::storage::test
