// SPDX-License-Identifier: Apache-2.0
// Pure, conservative planning of CMake target source membership changes.
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/cmake_file_api.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

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
 * non-CMake-owned input, including supported generated compiler/system files, with the same bytes
 * as the active configure observed. The caller retains
 * and revalidates every snapshot, project, profile and document generation through publication.
 * `sourcePath` is an absolute existing file and `definitionPath` names the traced project input.
 * No filesystem I/O or mutation is performed. The returned replacement differs only in one
 * literal argument or an insertion before the selected command's closing parenthesis.
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

} // namespace frost::project
