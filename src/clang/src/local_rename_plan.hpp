// SPDX-License-Identifier: Apache-2.0
// Read-only REF-4 preparation inside the Qt-free, pinned /MT frost-clang process.
#pragma once

#include <string_view>

namespace frost::clang_engine {

/*
 * Runs --plan-local-rename and writes one provisional JSON proposal to stdout. Never applies
 * edits. Inputs and the two ASTs are invocation-local; no state survives this synchronous call.
 * Only automatic locals/parameters of ordinary free functions are supported. Non-ready results
 * have no edits and return nonzero. Resource limits and unsupported syntax are explicit refusals.
 * resourceDirectory is the pinned SDK resource directory already located by frost-clang.
 */
[[nodiscard]] int runLocalRenamePlan(int argc, char* argv[], std::string_view resourceDirectory);

// Complete compilation-database, class/global/member rename; JSON request on stdin, no writes.
[[nodiscard]] int runProjectRenamePlan(std::string_view resourceDirectory);

#ifdef FROST_CLANG_PLAN_TESTS
// Test-build-only deterministic VFS/budget diagnostics. Uses only private in-memory inputs;
// prints observed check results, returns nonzero on any failure, and never starts child tools.
[[nodiscard]] int checkLocalRenamePlanner(std::string_view resourceDirectory);
#endif

} // namespace frost::clang_engine
