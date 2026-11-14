// SPDX-License-Identifier: Apache-2.0
// Pure membership proposals for absent template files; caller joins them with REF-1 Create.
#pragma once
#include "frost/project/cmake_membership.hpp"
namespace frost::project {
// Current captured cache must still name this source/build/generator, without hidden project
// hooks. This is current-input admission, not a claim these bytes were observed by configure.
[[nodiscard]] Result<void> validateTemplateCMakeCache(const CMakeProject&, std::string_view bytes);
// Same complete input/profile contract as membership planning. Files need not exist yet: the
// caller must retain absent endpoints and create their exact bytes in the SAME transaction.
// All additions are planned from original inputs and merged, never sequential on-disk edits.
[[nodiscard]] Result<std::vector<CMakeMembershipPlan>>
planCMakeGeneratedSources(const CMakeProject&, std::size_t targetIndex, std::span<const std::string> paths,
                          std::span<const CMakeInputSnapshot> inputs);
// A top-level executable/static library template in the configured root. No configure/build is
// run or claimed. Dynamic/scoped/incomplete invocation graphs and stale target sets refuse.
[[nodiscard]] Result<CMakeMembershipPlan> planCMakeTemplateTarget(const CMakeProject&, std::string_view name,
                                                                  bool library, std::span<const std::string> paths,
                                                                  std::span<const CMakeInputSnapshot> inputs);
} // namespace frost::project
