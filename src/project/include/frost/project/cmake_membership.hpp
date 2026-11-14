// SPDX-License-Identifier: Apache-2.0
// Pure, conservative planning of CMake target source membership changes.
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/cmake_file_api.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

enum class CMakeMembershipOperation { Add, Remove };

struct CMakeInputSnapshot {
    std::string path;
    std::string contents;
};

struct CMakeMembershipPlan {
    std::string definitionPath; // The absolute traced input whose bytes were examined.
    std::string original;
    std::string replacement;
};

/*
 * Plan one change to the configured target. `inputs` must contain exactly one snapshot for each
 * non-CMake-owned input, including supported generated compiler/system files, captured as one immutable request
 * snapshot. The model/backtraces are checked provenance hints, not a claim that configure observed those bytes. Parsed
 * command roles and the complete static invocation graph must still agree with the selected model origin. The caller
 * retains and revalidates every snapshot, project, profile and document generation through publication. `sourcePath` is
 * an absolute existing file and `definitionPath` names the traced project input. No filesystem I/O or mutation is
 * performed. The returned replacement differs only in one literal argument or an insertion before the selected
 * command's closing parenthesis.
 *
 * This does not implement a transaction, concurrency fencing, admission or reconfiguration.
 * The caller must provide these before exposing edits. Missing/ambiguous provenance, dynamic or
 * conditional declarations, generated files, multiple configurations, unsupported relative-path
 * contexts and non-literal source roles return Unsupported. Invalid input returns InvalidArgument;
 * existing/nonexistent membership returns AlreadyExists/NotFound. Thread-safe; O(input bytes plus
 * configured sources plus the square of the input count).
 */
[[nodiscard]] Result<CMakeMembershipPlan> planCMakeMembership(const CMakeProject& model, std::size_t targetIndex,
                                                              CMakeMembershipOperation operation,
                                                              std::string_view sourcePath,
                                                              std::string_view definitionPath,
                                                              std::span<const CMakeInputSnapshot> inputs);

/*
 * Rename one source leaf in its existing directory, preserving its unique literal argument's
 * position and visibility. Uses the same complete configured-input evidence and caller-owned
 * freshness/publication preconditions as planCMakeMembership. Does not check destination disk
 * absence, resolve physical aliases, rename files, write project bytes or establish Undo.
 * Other-target membership, moves, existing destination membership and unsupported provenance
 * are refused. The returned replacement must join the source rename in one REF-1 transaction;
 * every configured input/profile/document generation remains a read dependency through commit.
 * Paths use the existing native lexical comparison (ASCII case-insensitive on Windows);
 * case-only Windows renames are refused. Physical aliases are not detected or authorized here.
 * O(argument count * definition bytes + existing membership-validation work), with temporary
 * O(input bytes + parsed arguments) storage; same thread-safety as planCMakeMembership.
 */
[[nodiscard]] Result<CMakeMembershipPlan> planCMakeSourceRename(const CMakeProject& model, std::size_t targetIndex,
                                                                std::string_view oldSourcePath,
                                                                std::string_view newSourcePath,
                                                                std::string_view definitionPath,
                                                                std::span<const CMakeInputSnapshot> inputs);

// Plan all configured target memberships from the SAME original snapshots. Shared definition
// files merge nonoverlapping proven token edits; any unsafe membership refuses the entire batch.
// Every current source command must name a published target and its old-source membership must
// agree with the model, so a stale model cannot silently omit a newly introduced membership.
// Literal unscoped membership is configuration-invariant, so this aggregate supports multi-config
// models only through that static proof. No generated-expression/configuration-specific inference.
// No intermediate model mutation, writes or partial successes. Caller retains all input receipts.
[[nodiscard]] Result<std::vector<CMakeMembershipPlan>>
planCMakeSourceRenameAllTargets(const CMakeProject& model, std::string_view oldSourcePath,
                                std::string_view newSourcePath, std::span<const CMakeInputSnapshot> inputs);

} // namespace frost::project
