// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/ASTUnit.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
namespace frost::clang_engine {
// Only exact compiler-reported unresolved-use errors are admitted during original recovery parsing.
class CreateUsageDiagnostics final : public clang::DiagnosticConsumer {
public:
    CreateUsageDiagnostics(std::string path, std::string root, unsigned offset);
    void HandleDiagnostic(clang::DiagnosticsEngine::Level, const clang::Diagnostic&) override;
    bool failed = false, selectedError = false, unrelated = false;

private:
    std::string path, root;
    unsigned offset, count = 0;
};
struct CreatedUsage {
    std::string path, after;
};
// Recover a real typed call, then insert a user-supplied implementation. No source writes.
[[nodiscard]] std::optional<CreatedUsage> createFromUsage(clang::ASTUnit&, const llvm::json::Object&,
                                                          std::string& error);
} // namespace frost::clang_engine
