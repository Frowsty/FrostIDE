// SPDX-License-Identifier: Apache-2.0
// R13 native analyzer DTO/emission seam; invocation-local, Qt-free, no source mutation.
#pragma once
#include "clang/AST/ASTContext.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <set>
#include <string>
#include <vector>
namespace frost::clang_engine {
struct AnalyzerRule {
    const char *id, *title, *category, *level, *applicability, *settingKey;
    bool cfg;
};
[[nodiscard]] const std::vector<AnalyzerRule>& analyzerRules();
[[nodiscard]] llvm::json::Array analyzerRegistry();
class AnalyzerResults {
public:
    clang::ASTContext& context;
    std::string root;
    std::set<std::string> enabled, emitted;
    llvm::json::Array findings, limitations;
    explicit AnalyzerResults(clang::ASTContext&, std::string projectRoot, std::set<std::string> rules);
    // Emits only owned file-token locations. Macro/system diagnostics never acquire invented edits.
    void add(llvm::StringRef id, clang::SourceRange range, llvm::StringRef message,
             std::optional<std::string> replacement = std::nullopt);
    void limited(const clang::FunctionDecl&, llvm::StringRef why);
};
[[nodiscard]] llvm::json::Object analyzerSarif(llvm::json::Object report);
void analyzeSemantics(AnalyzerResults&);
void analyzeDataflow(AnalyzerResults&);
[[nodiscard]] int runAnalyzerUnit(std::string_view resourceDirectory, bool sarif = false);
} // namespace frost::clang_engine
