// SPDX-License-Identifier: Apache-2.0
// Read-only AST refactoring proposals; reserved operation dispatcher for REF-6 through REF-10.
#pragma once
#include <string_view>
namespace frost::clang_engine {
// JSON stdin, atomic before/after images and immutable dependency receipts on stdout. No writes.
// Unsupported profiles produce no edits. Synchronous, invocation-local, bounded by shared snapshots.
[[nodiscard]] int runAstRefactoringPlan(std::string_view resourceDirectory);
} // namespace frost::clang_engine
