// SPDX-License-Identifier: Apache-2.0
//
// storage_test_child: the helper program the storage crash, concurrency and stress tests launch
// through io::realProcessLauncher() and kill with IProcess::terminate() (kill -9). argv[1] selects
// the mode; every line it prints is complete and printed only after the operation it reports has
// returned, so the parent knows what is durable. Content comes from support/crash_protocol.hpp.
//
//   txn-loop DB ROWS SYNC SEED  transactions of ROWS rows plus a ledger row, from the store's
//                               last txn + 1; "commit <t>" after each COMMIT returns (SYNC is
//                               normal or full)
//   txn-count DB N SEED         N transactions of 20 rows (Normal), each "commit <t>", then exit 0
//   hold-mid DB SEED           5 committed transactions ("commit <t>"), then one half-written
//                               one: "mid", then blocks reading stdin
//   checkpoint-loop DB SEED     txn-loop with 20 rows, Normal, and walAutoCheckpointPages=8
//   journal-put-loop DIR SIZE SEED
//                               creates a journal and puts generation 1, 2, ... of one document;
//                               "journal <path>" first, then "put <g>" after each put returns
//   journal-create-loop DIR     creates a journal, puts one entry and closes it cleanly (hot
//                               exit), over and over; "created <path>" after each close
//   hold-journal DIR            a journal with one entry: "ready <path>", then blocks on stdin
//   exit-clean DIR              a journal with one entry, closed cleanly: "done <path>", exit 0
//   bank-writer DB SEED         transfers between the 16 bank accounts; "ready" once the
//                               accounts exist, then "transfer <n>" every 50 transfers
//   history-record-loop DB SEED records historyContent(SEED, n) for n = 1, 2, ... over 5 paths;
//                               "record <n>" after each record returns
//
// Exit status: 0 for a mode that ends, 2 for bad arguments, 3 for a storage error (printed as
// "error <description>").

#include "frost/core/clock.hpp"
#include "frost/storage/database.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"

#include "support/crash_protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using frost::storage::Database;
using frost::storage::TransactionKind;
namespace test = frost::storage::test;

void line(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

int storageError(const frost::Error& error) {
    line("error " + error.describe());
    return 3;
}

// Blocks until stdin reaches EOF (the parent never writes; it kills the child).
void blockOnStdin() {
    char buffer[64];
    while (std::fread(buffer, 1, sizeof buffer, stdin) > 0) {
    }
}

bool parseInt(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

frost::Result<Database> openCrashStore(const std::string& path, frost::storage::Synchronous sync,
                                       std::uint32_t autoCheckpointPages = 1000) {
    frost::storage::OpenOptions options;
    options.synchronous = sync;
    options.schema = &test::kCrashSchema;
    options.walAutoCheckpointPages = autoCheckpointPages;
    options.busyTimeout = std::chrono::seconds(10);
    return Database::open(path, options);
}

frost::Result<std::int64_t> lastTxn(Database& db) {
    auto statement = db.prepare("SELECT coalesce(max(txn), 0) FROM crash_ledger");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    auto row = statement->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    return statement->columnInt64(0);
}

// Writes rows [0, upTo) of transaction `txn`; with `ledger`, also its ledger row.
frost::Result<void> writeTxnRows(Database& db, std::uint64_t seed, std::int64_t txn, int rows, int upTo, bool ledger) {
    auto insert = db.prepare("INSERT INTO crash_data (txn, row, value) VALUES (?1, ?2, ?3)");
    if (!insert) {
        return std::unexpected(insert.error());
    }
    for (int row = 0; row < upTo; ++row) {
        const std::string value = test::rowValue(seed, txn, row);
        if (auto bound = insert->bindInt64(1, txn); !bound) {
            return bound;
        }
        if (auto bound = insert->bindInt64(2, row); !bound) {
            return bound;
        }
        if (auto bound = insert->bindBlob(3, value); !bound) {
            return bound;
        }
        if (auto ran = insert->run(); !ran) {
            return ran;
        }
    }
    if (!ledger) {
        return {};
    }
    auto entry = db.prepare("INSERT INTO crash_ledger (txn, rows, checksum) VALUES (?1, ?2, ?3)");
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (auto bound = entry->bindInt64(1, txn); !bound) {
        return bound;
    }
    if (auto bound = entry->bindInt64(2, rows); !bound) {
        return bound;
    }
    if (auto bound = entry->bindInt64(3, test::ledgerChecksum(seed, txn, rows)); !bound) {
        return bound;
    }
    return entry->run();
}

frost::Result<void> commitTxn(Database& db, std::uint64_t seed, std::int64_t txn, int rows) {
    return db.inTransaction(TransactionKind::Immediate,
                            [&]() -> frost::Result<void> { return writeTxnRows(db, seed, txn, rows, rows, true); });
}

int txnLoop(const std::string& path, int rows, frost::storage::Synchronous sync, std::uint64_t seed,
            std::uint32_t autoCheckpointPages, std::int64_t stopAfter) {
    auto db = openCrashStore(path, sync, autoCheckpointPages);
    if (!db) {
        return storageError(db.error());
    }
    auto last = lastTxn(*db);
    if (!last) {
        return storageError(last.error());
    }
    for (std::int64_t txn = *last + 1; stopAfter < 0 || txn <= *last + stopAfter; ++txn) {
        if (auto committed = commitTxn(*db, seed, txn, rows); !committed) {
            return storageError(committed.error());
        }
        line(std::format("commit {}", txn));
    }
    return 0;
}

int holdMid(const std::string& path, std::uint64_t seed) {
    auto db = openCrashStore(path, frost::storage::Synchronous::Full);
    if (!db) {
        return storageError(db.error());
    }
    auto last = lastTxn(*db);
    if (!last) {
        return storageError(last.error());
    }
    constexpr int kRows = 20;
    std::int64_t txn = *last + 1;
    for (int i = 0; i < 5; ++i, ++txn) {
        if (auto committed = commitTxn(*db, seed, txn, kRows); !committed) {
            return storageError(committed.error());
        }
        line(std::format("commit {}", txn));
    }
    auto transaction = db->begin(TransactionKind::Immediate);
    if (!transaction) {
        return storageError(transaction.error());
    }
    if (auto half = writeTxnRows(*db, seed, txn, kRows, kRows / 2, false); !half) {
        return storageError(half.error());
    }
    line("mid");
    blockOnStdin();
    return 0;
}

frost::storage::DocumentState journalState(std::int64_t generation) {
    frost::storage::DocumentState state;
    state.documentKey = "crash-doc";
    state.path = "/work/crash-doc.txt";
    state.generation = generation;
    state.encoding = "utf-8";
    state.lineEnding = "lf";
    return state;
}

int journalPutLoop(const std::string& dir, std::size_t size, std::uint64_t seed) {
    auto journal = frost::storage::HotExitJournal::create(dir, frost::systemClock(), {});
    if (!journal) {
        return storageError(journal.error());
    }
    line("journal " + journal->path());
    for (std::int64_t generation = 1;; ++generation) {
        const std::string content = test::journalContent(seed, generation, size);
        const std::string_view pieces[] = {content};
        auto put = journal->put(journalState(generation), pieces);
        if (!put) {
            return storageError(put.error());
        }
        line(std::format("put {}", generation));
    }
}

frost::Result<frost::storage::HotExitJournal> journalWithEntry(const std::string& dir) {
    auto journal = frost::storage::HotExitJournal::create(dir, frost::systemClock(), {});
    if (!journal) {
        return std::unexpected(journal.error());
    }
    const std::string content = test::journalContent(7, 1, 5000);
    const std::string_view pieces[] = {content};
    if (auto put = journal->put(journalState(1), pieces); !put) {
        return std::unexpected(put.error());
    }
    return journal;
}

int journalCreateLoop(const std::string& dir) {
    while (true) {
        auto journal = journalWithEntry(dir);
        if (!journal) {
            return storageError(journal.error());
        }
        const std::string path = journal->path();
        if (auto closed = journal->closeCleanly(); !closed) {
            return storageError(closed.error());
        }
        line("created " + path);
    }
}

int holdJournal(const std::string& dir) {
    auto journal = journalWithEntry(dir);
    if (!journal) {
        return storageError(journal.error());
    }
    line("ready " + journal->path());
    blockOnStdin();
    return 0;
}

int exitClean(const std::string& dir) {
    auto journal = journalWithEntry(dir);
    if (!journal) {
        return storageError(journal.error());
    }
    const std::string path = journal->path();
    if (auto closed = journal->closeCleanly(); !closed) {
        return storageError(closed.error());
    }
    line("done " + path);
    return 0;
}

int bankWriter(const std::string& path, std::uint64_t seed) {
    auto db = openCrashStore(path, frost::storage::Synchronous::Normal);
    if (!db) {
        return storageError(db.error());
    }
    auto seeded = db->inTransaction(TransactionKind::Immediate, [&]() -> frost::Result<void> {
        return db->execute(std::format("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < {}) "
                                       "INSERT OR IGNORE INTO bank (id, balance) SELECT i, {} FROM n",
                                       test::kBankAccounts, test::kBankTotal / test::kBankAccounts));
    });
    if (!seeded) {
        return storageError(seeded.error());
    }
    line("ready");
    std::uint64_t state = seed;
    for (std::int64_t n = 1;; ++n) {
        state = test::mix(state, static_cast<std::uint64_t>(n));
        const int from = 1 + static_cast<int>(state % test::kBankAccounts);
        const int to = 1 + static_cast<int>((state >> 8) % test::kBankAccounts);
        const std::int64_t amount = static_cast<std::int64_t>((state >> 16) % 50);
        auto moved = db->inTransaction(TransactionKind::Immediate, [&]() -> frost::Result<void> {
            if (auto debit =
                    db->execute(std::format("UPDATE bank SET balance = balance - {} WHERE id = {}", amount, from));
                !debit) {
                return debit;
            }
            return db->execute(std::format("UPDATE bank SET balance = balance + {} WHERE id = {}", amount, to));
        });
        if (!moved) {
            return storageError(moved.error());
        }
        if (n % 50 == 0) {
            line(std::format("transfer {}", n));
        }
    }
}

int historyRecordLoop(const std::string& path, std::uint64_t seed) {
    frost::storage::HistoryOptions options;
    options.busyTimeout = std::chrono::seconds(10);
    auto history = frost::storage::LocalHistory::open(path, frost::systemClock(), options);
    if (!history) {
        return storageError(history.error());
    }
    for (std::int64_t n = 1;; ++n) {
        const std::string content = test::historyContent(seed, n);
        const std::string_view pieces[] = {content};
        const std::string key = std::format("src/file{}.cpp", n % 5);
        auto recorded = history->record(key, "/work/" + key, frost::storage::RevisionKind::Save, {}, pieces);
        if (!recorded) {
            return storageError(recorded.error());
        }
        line(std::format("record {}", n));
    }
}

int run(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        line("usage: storage_test_child MODE ARGS...");
        return 2;
    }
    const std::string& mode = args[1];
    const std::string& target = args[2];
    std::int64_t a = 0;
    std::int64_t b = 0;
    auto syncOf = [](std::string_view text) {
        return text == "full" ? frost::storage::Synchronous::Full : frost::storage::Synchronous::Normal;
    };
    if (mode == "txn-loop" && args.size() == 6 && parseInt(args[3], a) && parseInt(args[5], b)) {
        return txnLoop(target, static_cast<int>(a), syncOf(args[4]), static_cast<std::uint64_t>(b), 1000, -1);
    }
    if (mode == "txn-count" && args.size() == 5 && parseInt(args[3], a) && parseInt(args[4], b)) {
        // Not killed: `a` transactions of 20 rows with seed `b`, then exit (setup and stress).
        return txnLoop(target, 20, frost::storage::Synchronous::Normal, static_cast<std::uint64_t>(b), 1000, a);
    }
    if (mode == "hold-mid" && args.size() == 4 && parseInt(args[3], a)) {
        return holdMid(target, static_cast<std::uint64_t>(a));
    }
    if (mode == "checkpoint-loop" && args.size() == 4 && parseInt(args[3], a)) {
        return txnLoop(target, 20, frost::storage::Synchronous::Normal, static_cast<std::uint64_t>(a), 8, -1);
    }
    if (mode == "journal-put-loop" && args.size() == 5 && parseInt(args[3], a) && parseInt(args[4], b)) {
        return journalPutLoop(target, static_cast<std::size_t>(a), static_cast<std::uint64_t>(b));
    }
    if (mode == "journal-create-loop" && args.size() == 3) {
        return journalCreateLoop(target);
    }
    if (mode == "hold-journal" && args.size() == 3) {
        return holdJournal(target);
    }
    if (mode == "exit-clean" && args.size() == 3) {
        return exitClean(target);
    }
    if (mode == "bank-writer" && args.size() == 4 && parseInt(args[3], a)) {
        return bankWriter(target, static_cast<std::uint64_t>(a));
    }
    if (mode == "history-record-loop" && args.size() == 4 && parseInt(args[3], a)) {
        return historyRecordLoop(target, static_cast<std::uint64_t>(a));
    }
    line("bad arguments for mode " + mode);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Never a dialog on the desktop: crashes, aborts and Debug CRT reports go to stderr (merged
    // into stdout by the parent) and end the process.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    for ([[maybe_unused]] const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) { // Unused in Release.
        static_cast<void>(_CrtSetReportMode(type, _CRTDBG_MODE_FILE));
        static_cast<void>(_CrtSetReportFile(type, _CRTDBG_FILE_STDERR));
    }
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Unbuffered: a line is in the pipe before the next operation starts.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // argv is UTF-8: the embedded manifest sets the UTF-8 code page on Windows.
    const std::vector<std::string> args(argv, argv + argc);
    const int status = run(args);
    std::fflush(stdout);
    return status;
}
