// SPDX-License-Identifier: Apache-2.0
// Canonical AST operation seam. Further refactorings extend this dispatcher in separate TUs.
#pragma once
#include "clang/Frontend/ASTUnit.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
namespace frost::clang_engine {
// Returns only the main file's candidate bytes. Error is actionable unsupported syntax/selection.
// All source ranges are file tokens; callers reparse every captured configuration before approval.
[[nodiscard]] std::optional<std::string> refactorAst(clang::ASTUnit& ast, const llvm::json::Object& request,
                                                     std::string& error);
[[nodiscard]] std::optional<std::string> refactorSafeDelete(clang::ASTUnit& ast, const llvm::json::Object& request,
                                                            std::string& error);
} // namespace frost::clang_engine
