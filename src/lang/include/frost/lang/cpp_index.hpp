// SPDX-License-Identifier: Apache-2.0
// CPP-8 configuration-preserving semantic units, bounded queries and atomic SQLite publication.
#pragma once
#include "frost/lang/cpp_rename_plan.hpp"
#include "frost/storage/database.hpp"

#include <functional>
#include <span>

namespace frost::io {
class IFileSystem;
}

namespace frost::lang {
struct CppIndexUnit {
    std::string commandIdentity, configuration, source, databaseSha256;
    std::uint64_t commandIndex = 0;
    Json data; // Validated actual producer rows and immutable dependency receipts.
};
struct CppIndexLocation {
    std::string commandIdentity, configuration, usr, name, kind, path;
    std::uint64_t offset = 0, line = 0, column = 0, roles = 0;
};
struct CppIndexRequest {
    std::string projectRoot, compilationDatabase, configuration, llvmRoot;
    std::uint64_t commandIndex = 0;
    std::vector<CppRenameOverlay> overlays;
};
struct CppIndexQuery {
    std::vector<CppIndexLocation> locations;
    std::size_t staleCommands = 0;
    bool truncated = false;
    std::vector<CppIndexUnit> receipts; // Exact query-time command receipts for later UI admission.
};
struct CppCompilerInput {
    std::string canonicalPath;
    std::string bytes;
};
// Read-only compiler inputs may use aliases. Capture their resolved path around a bounded,
// no-follow read of the actual file so editor mutation paths never inherit this permission.
[[nodiscard]] Result<CppCompilerInput> readCppCompilerInput(io::IFileSystem& fs, std::string_view path);
[[nodiscard]] Result<CppIndexUnit> parseCppIndexUnit(std::string_view bytes);
[[nodiscard]] sched::Task<Result<CppIndexUnit>> requestCppIndexUnit(io::IProcessLauncher& launcher,
                                                                    io::AsyncContext context, std::string helper,
                                                                    CppIndexRequest request, IClock::Duration timeout,
                                                                    std::stop_token stop = {});
// Compare actual dependency bytes/probes to captured overlays and disk on a background worker.
// No publication occurs here. Caller additionally guards CDB/project/open-document generations.
[[nodiscard]] Result<void> validateCppIndexInputs(io::IFileSystem& fs, const CppIndexUnit& unit,
                                                  std::span<const CppRenameOverlay> overlays,
                                                  const std::stop_token& stop = {});

// One owning actor uses this move-only store on one worker at a time. All callbacks/queries are
// synchronous worker operations; no Qt or LLVM objects cross this interface. Publication is one
// transaction; cancellation/staleness must be admitted before it. Query limits are hard capped.
class CppIndexStore {
public:
    [[nodiscard]] static Result<CppIndexStore> open(std::string_view path);
    [[nodiscard]] Result<void> publish(const CppIndexUnit& unit, std::uint64_t generation, std::stop_token stop = {});
    [[nodiscard]] Result<void> invalidate(std::string_view commandIdentity);
    [[nodiscard]] Result<void> clear();
    [[nodiscard]] Result<std::size_t> storedCommands(std::string_view databaseSha256);
    [[nodiscard]] Result<CppIndexQuery> symbols(std::string_view prefix, std::string_view databaseSha256,
                                                std::string_view configuration = {}, std::size_t limit = 200,
                                                const std::stop_token& stop = {});
    [[nodiscard]] Result<CppIndexQuery> occurrences(std::string_view usr, std::string_view databaseSha256,
                                                    std::string_view configuration = {}, std::size_t limit = 1000,
                                                    const std::stop_token& stop = {});
    [[nodiscard]] Result<Json> relations(std::string_view usr, std::string_view databaseSha256,
                                         std::string_view configuration = {}, std::size_t limit = 1000,
                                         const std::stop_token& stop = {});

private:
    explicit CppIndexStore(storage::Database database) : database(std::move(database)) {}
    storage::Database database;
    [[nodiscard]] Result<CppIndexQuery> query(std::string_view value, std::string_view databaseSha256,
                                              std::string_view configuration, std::size_t limit, bool usages,
                                              const std::stop_token& stop);
};
} // namespace frost::lang
