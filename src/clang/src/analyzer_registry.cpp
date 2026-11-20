// SPDX-License-Identifier: Apache-2.0
// Source-owned rule identities, not aliases for external tool check counts.
#include "analyzer.hpp"
namespace frost::clang_engine {
const std::vector<AnalyzerRule>& analyzerRules() {
    static const std::vector<AnalyzerRule> rules{
#define FROST_ANALYZER_RULE(id,title,category,level,applicability,key,cfg) {id,title,category,level,applicability,key,cfg},
#include "../../../resources/analyzer_rules.inc"
#undef FROST_ANALYZER_RULE
    };
    return rules;
}
llvm::json::Array analyzerRegistry() {
    llvm::json::Array result;
    for (const auto& rule : analyzerRules()) {
        result.push_back(llvm::json::Object{{"id",rule.id},{"title",rule.title},{"category",rule.category},
            {"defaultLevel",rule.level},{"tier",rule.cfg ? "T2-CFG" : "T2-AST"},
            {"settingKey",rule.settingKey},{"applicability",rule.applicability}});
    }
    return result;
}
}
