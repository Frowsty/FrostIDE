// SPDX-License-Identifier: Apache-2.0
// Read-only async LibTooling project rename proposals; transaction publication belongs to document.
#pragma once
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/async_process.hpp"
#include "frost/lang/json.hpp"
#include "frost/sched/task.hpp"

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace frost::lang {
struct CppRenameOverlay {
    std::string path;
    std::string contents;
};
struct CppRenameRequest {
    std::string projectRoot;
    std::string llvmRoot;            // Optional trusted pinned SDK override for FROST_LLVM_ROOT, never project JSON.
    std::string seedTranslationUnit; // Empty: discover a command containing the header selection.
    std::string path;
    std::uint64_t offset = 0; // UTF-8 byte offset in the captured original document.
    std::string newName;
    std::string compilationDatabase;        // Complete captured compile_commands.json bytes, not a path.
    std::vector<CppRenameOverlay> overlays; // Captured dirty snapshots; no disk writes.
};
struct CppRenameFile {
    std::string path, before, after, beforeSha256, afterSha256;
    bool overlay = false;
};
struct CppRenameDependency {
    std::string path, sha256;
    std::uint64_t bytes = 0;
    bool overlay = false;
};
struct CppRenameProposal {
    std::string compilationDatabaseSha256;
    std::string status; // ready, conflict, unsupported, parse-error, invalid-input.
    std::vector<CppRenameFile> files;
    std::vector<CppRenameDependency> dependencies;
    std::vector<std::string> missingDependencies;
    std::vector<std::string> diagnostics;
    std::uint64_t translationUnitConfigurations = 0;
    [[nodiscard]] bool ready() const noexcept { return status == "ready"; }
};
[[nodiscard]] Json cppRenameRequestJson(const CppRenameRequest& request);
[[nodiscard]] Result<CppRenameProposal> parseCppRenameProposal(std::string_view bytes);
/*
 * Starts absolute pinned frost-clang --plan-project-rename, writes JSON to stdin and awaits
 * completion asynchronously. No source writes or Apply. Context/launcher outlive the task.
 * Caller binds project/profile/document generations, rejects stale replies, rechecks every
 * dependency hash/absence and submits the ENTIRE selected rename as one RefactoringPlan.
 * A fast clangd preview may precede this background semantic check; it is not its approval.
 */
[[nodiscard]] sched::Task<Result<CppRenameProposal>>
requestCppRenamePlan(io::IProcessLauncher& launcher, io::AsyncContext context, std::string helper,
                     CppRenameRequest request, IClock::Duration timeout, std::stop_token stop = {});
} // namespace frost::lang
