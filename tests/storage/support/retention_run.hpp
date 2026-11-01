// SPDX-License-Identifier: Apache-2.0
//
// The retention property driver shared by retention_property_test and the stress shards: one
// seed runs random record and prune operations against a LocalHistory and the independent
// RetentionOracle, and compares them after every operation. Test code only.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/storage/local_history.hpp"

#include "retention_oracle.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <vector>

namespace frost::storage::test {

namespace retention_detail {

inline std::int64_t unixNanos(IClock::SystemTime time) { return time.time_since_epoch().count(); }

// Content number `n` of the pool: deterministic, so equal numbers are equal bytes. Sizes run
// from 0 to about 3 KiB; the pool is small, so dedup across paths and repeats happen often.
inline std::string poolContent(std::uint32_t n) {
    std::mt19937 rng(n * 2654435761u + 1);
    const std::size_t size = n == 0 ? 0 : rng() % 3000;
    std::string out(size, '\0');
    for (char& byte : out) {
        byte = static_cast<char>(rng() & 0xFF);
    }
    return out;
}

// Every surviving revision id, ascending, through the wrapper.
inline std::vector<std::int64_t> storedIds(LocalHistory& history, std::string& error) {
    std::vector<std::int64_t> ids;
    auto statement = history.database().prepare("SELECT id FROM lh_revision ORDER BY id");
    if (!statement) {
        error = statement.error().describe();
        return ids;
    }
    while (true) {
        auto row = statement->step();
        if (!row) {
            error = row.error().describe();
            return ids;
        }
        if (!*row) {
            return ids;
        }
        ids.push_back(statement->columnInt64(0));
    }
}

inline std::uint64_t storedReferencedBytes(LocalHistory& history, std::string& error) {
    auto statement = history.database().prepare(
        "SELECT coalesce(sum(size), 0) FROM lh_blob WHERE id IN (SELECT blob_id FROM lh_revision)");
    if (!statement) {
        error = statement.error().describe();
        return 0;
    }
    auto row = statement->step();
    if (!row || !*row) {
        error = row ? "no row" : row.error().describe();
        return 0;
    }
    return static_cast<std::uint64_t>(statement->columnInt64(0));
}

} // namespace retention_detail

/*
 * Runs `operations` random operations for `seed` over 10 paths: about 70% records (sometimes
 * labelled), 30% prunes with random valid policies, and clock steps of up to 10 minutes forward
 * or 5 minutes back. `clock` must be the clock `history` was opened with. Returns "" when the
 * store matched the oracle throughout, else a description of the first difference.
 */
inline std::string runRetentionSeed(std::uint64_t seed, int operations, LocalHistory& history, ManualClock& clock) {
    using namespace retention_detail;
    std::mt19937_64 rng(seed);
    RetentionOracle oracle;
    for (int op = 0; op < operations; ++op) {
        const std::uint64_t step = rng() % 100;
        if (step < 10) {
            const auto back = std::chrono::seconds(static_cast<std::int64_t>(rng() % 300));
            clock.setSystemTime(clock.systemNow() - back);
        } else {
            clock.advance(std::chrono::seconds(static_cast<std::int64_t>(rng() % 600)));
        }
        const std::int64_t now = unixNanos(clock.systemNow());
        const std::string where = std::format("seed {} op {}", seed, op);
        if (rng() % 10 < 7) {
            const std::string key = std::format("src/file{}.cpp", rng() % 10);
            const std::string content = poolContent(static_cast<std::uint32_t>(rng() % 24));
            const bool labelled = rng() % 8 == 0;
            const std::string_view pieces[] = {content};
            auto outcome = history.record(key, "/" + key, labelled ? RevisionKind::Labelled : RevisionKind::Save,
                                          labelled ? "label" : "", pieces);
            if (!outcome) {
                return std::format("{}: record failed: {}", where, outcome.error().describe());
            }
            const auto expected = oracle.record(key, content, labelled, now);
            if (outcome->added != expected.added || outcome->revision.id != expected.id) {
                return std::format("{}: record gave id {} added {}, the oracle id {} added {}", where,
                                   outcome->revision.id, outcome->added, expected.id, expected.added);
            }
            continue;
        }
        const auto maxAge = std::chrono::seconds(1 + static_cast<std::int64_t>(rng() % 3600));
        const std::uint64_t maxBytes = 1 + rng() % 20000;
        const bool keepNewest = rng() % 2 == 0;
        auto pruned = history.prune(RetentionPolicy(maxAge, maxBytes, keepNewest));
        if (!pruned) {
            return std::format("{}: prune failed: {}", where, pruned.error().describe());
        }
        const std::size_t expectedRemoved =
            oracle.prune(now, std::chrono::nanoseconds(maxAge).count(), maxBytes, keepNewest);
        std::string error;
        const std::vector<std::int64_t> ids = storedIds(history, error);
        const std::uint64_t bytes = storedReferencedBytes(history, error);
        auto stats = history.stats();
        if (!error.empty() || !stats) {
            return std::format("{}: reading back failed: {}", where, error.empty() ? stats.error().describe() : error);
        }
        if (ids != oracle.ids()) {
            return std::format("{}: survivors differ after prune(maxAge {} s, maxBytes {}, keepNewest {}): {} "
                               "stored, {} expected",
                               where, maxAge.count(), maxBytes, keepNewest, ids.size(), oracle.ids().size());
        }
        if (pruned->revisionsRemoved != expectedRemoved) {
            return std::format("{}: removed {}, the oracle {}", where, pruned->revisionsRemoved, expectedRemoved);
        }
        if (bytes != oracle.referencedBytes() || stats->contentBytes != bytes) {
            return std::format("{}: referenced bytes {} (all blobs {}), the oracle {}", where, bytes,
                               stats->contentBytes, oracle.referencedBytes());
        }
    }
    return {};
}

} // namespace frost::storage::test
