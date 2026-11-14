// SPDX-License-Identifier: Apache-2.0
// REF-1 file/membership proposal assembly. No source move, project write or document mutation.
#pragma once

#include "frost/document/refactoring_transaction.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/frost_engine_assets.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace frost::app {

struct RefactoringInputSnapshot {
    std::string path;
    std::optional<std::string> disk;
    std::optional<std::string> buffer;
    [[nodiscard]] const std::optional<std::string>& logical() const noexcept { return buffer ? buffer : disk; }
};

// Owner thread: openText is a captured FileDocument UTF8 snapshot, when open. Only NotFound is
// absence; unreadable/nonregular/oversized files fail. The caller retains project/trust context.
[[nodiscard]] Result<RefactoringInputSnapshot>
captureRefactoringInput(io::IFileSystem& fs, std::string path, std::optional<std::string> openText = {});

// Appends exact disk and optional read-only buffer dependencies; conflicting duplicate receipts
// fail. These functions only assemble values; discard the aggregate plan on any failure.
[[nodiscard]] Result<void> appendRefactoringDependency(document::RefactoringPlan& plan,
                                                        const RefactoringInputSnapshot& input);
[[nodiscard]] Result<void> appendRefactoringReplacement(document::RefactoringPlan& plan,
                                                         const RefactoringInputSnapshot& input,
                                                         std::string_view before, std::string after);
// Host evaluation reads disk, not an unsaved editor overlay. Both disk and any open logical
// text must match its observed input; decode losslessly (including UTF8/UTF16 BOMs) for comparison.
[[nodiscard]] Result<void> refactoringInputMatchesEvaluation(const RefactoringInputSnapshot& input,
                                                               const std::optional<std::string>& evaluatedBytes);

// One byte-preserving file rename and optional .fmeta rename, with destination/sidecar absence
// dependencies. Folders and direct metadata operations are refused. Coordinator owns application,
// open-document rebinding, pending recovery and inverse publication; this helper never does them.
[[nodiscard]] Result<document::RefactoringPlan>
prepareSourceFileRename(io::IFileSystem& fs, const lang::FmetaMovePlan& move, std::string label);

} // namespace frost::app
