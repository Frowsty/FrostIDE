// SPDX-License-Identifier: Apache-2.0
//
// openStore (open with quarantine of corrupt files) and removeDatabaseFiles (see
// frost/storage/store.hpp).

#include "frost/storage/store.hpp"

#include "frost/core/assert.hpp"

#include "database_files.hpp"
#include "test_hooks.hpp"

#include <format>
#include <string>
#include <utility>

namespace frost::storage {

namespace {

// Opens after a copy turned out not to be corrupt any more; more means the file keeps changing.
constexpr int kQuarantineAttempts = 3;

/*
 * The verdict of Database::open on a quarantine copy, judged as the original was (same schema,
 * verification and VFS). Read-write without creation: a refusal never modifies the file, and a
 * copy that opens (and so may be migrated) or is fresh is deleted afterwards anyway.
 */
bool copyStillCorrupt(const std::string& copyPath, const OpenOptions& options) {
    OpenOptions check = options;
    check.mode = OpenMode::ReadWrite;
    check.allowFresh = false;
    check.exclusiveLock = false;
    check.checkpointOnClose = false;
    auto copy = Database::open(copyPath, check);
    return !copy && copy.error().code() == ErrorCode::Corrupt;
}

} // namespace

Result<StoreOpenOutcome> openStore(std::string_view path, const OpenOptions& options, CorruptPolicy policy) {
    FROST_PRE(options.schema != nullptr);
    IClock& clock = options.clock != nullptr ? *options.clock : systemClock();
    for (int attempt = 1;; ++attempt) {
        auto opened = Database::open(path, options);
        if (opened) {
            return StoreOpenOutcome{std::move(*opened), std::nullopt};
        }
        // Only a damaged or foreign file is moved aside; a read-only open never removes anything.
        if (opened.error().code() != ErrorCode::Corrupt || policy != CorruptPolicy::Quarantine ||
            options.mode == OpenMode::ReadOnly) {
            return std::unexpected(std::move(opened.error()));
        }
        Error reason = std::move(opened.error());
        detail::g_openStoreBeforeQuarantine.run();
        auto movedTo =
            detail::quarantineFiles(path, clock, options.busyTimeout, options.vfs,
                                    [&options](const std::string& copy) { return copyStillCorrupt(copy, options); });
        if (!movedTo) {
            return std::unexpected(std::move(movedTo.error()).withContext(reason.message()));
        }
        if (!movedTo->has_value()) {
            // Replaced since our verdict (another process quarantined it first): open it again.
            if (attempt == kQuarantineAttempts) {
                return fail(ErrorCode::Busy, std::format("open store '{}': the file changed during quarantine {} times",
                                                         path, kQuarantineAttempts));
            }
            continue;
        }
        auto fresh = Database::open(path, options);
        if (!fresh) {
            return std::unexpected(std::move(fresh.error()).withContext("open after quarantine"));
        }
        return StoreOpenOutcome{std::move(*fresh), Quarantined{std::move(movedTo->value()), std::move(reason)}};
    }
}

Result<void> removeDatabaseFiles(std::string_view path) { return detail::removeFiles(path, detail::kFileLockTimeout); }

} // namespace frost::storage
