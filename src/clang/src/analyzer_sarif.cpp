// SPDX-License-Identifier: Apache-2.0
// Native SARIF 2.1 output of actual analyzer regions/fixes; emitted columns are Unicode codepoints.
#include "analyzer.hpp"
#include "llvm/Config/llvm-config.h"
namespace frost::clang_engine {
namespace {
std::string fileUri(llvm::StringRef original) {
    std::string path=original.str();
#ifdef _WIN32
    for(auto& ch:path) if(ch=='\\') ch='/';
#endif
    std::string uri=path.starts_with("//") ? "file:" : path.starts_with('/') ? "file://" : "file:///";
    constexpr char hex[]="0123456789ABCDEF";
    for(unsigned char ch:path) {
        if((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='/'||ch==':'||ch=='-'||ch=='_'||ch=='.'||ch=='~') uri+=static_cast<char>(ch);
        else {uri+='%';uri+=hex[ch>>4];uri+=hex[ch&15];}
    }
    return uri;
}
}
llvm::json::Object analyzerSarif(llvm::json::Object report) {
    llvm::json::Array rules,results;
    for(const auto& rule:analyzerRules()) {
        llvm::json::Object metadata{{"id",rule.id},{"shortDescription",llvm::json::Object{{"text",rule.title}}}};
        metadata["defaultConfiguration"]=llvm::json::Object{{"level",rule.level}};
        metadata["properties"]=llvm::json::Object{{"category",rule.category},{"applicability",rule.applicability}};
        rules.push_back(std::move(metadata));
    }
    if(const auto* values=report.getArray("findings")) for(const auto& value:*values) if(const auto* item=value.getAsObject()) {
        llvm::StringRef level="warning";
        for(const auto& rule:analyzerRules()) if(item->getString("ruleId")==rule.id) {level=rule.level;break;}
        const auto uri=fileUri(item->getString("path").value_or(""));
        llvm::json::Object artifact{{"uri",uri}};
        llvm::json::Object region{{"startLine",item->getInteger("line").value_or(1)},{"startColumn",item->getInteger("column").value_or(1)},
            {"endLine",item->getInteger("endLine").value_or(1)},{"endColumn",item->getInteger("endColumn").value_or(1)}};
        llvm::json::Object physical{{"artifactLocation",std::move(artifact)},{"region",std::move(region)}};
        llvm::json::Object result{{"ruleId",item->getString("ruleId").value_or("")},{"level",level},
            {"message",llvm::json::Object{{"text",item->getString("message").value_or("")}}}};
        result["locations"]=llvm::json::Array{llvm::json::Object{{"physicalLocation",std::move(physical)}}};
        if(auto replacement=item->getString("replacement")) {
            llvm::json::Object deleted{{"byteOffset",item->getInteger("offset").value_or(0)},{"byteLength",item->getInteger("length").value_or(0)}};
            llvm::json::Object edit{{"deletedRegion",std::move(deleted)},{"insertedContent",llvm::json::Object{{"text",*replacement}}}};
            llvm::json::Object change{{"artifactLocation",llvm::json::Object{{"uri",uri}}},{"replacements",llvm::json::Array{std::move(edit)}}};
            llvm::json::Object fix{{"description",llvm::json::Object{{"text","Apply the exact AST-token replacement"}}},{"artifactChanges",llvm::json::Array{std::move(change)}}};
            result["fixes"]=llvm::json::Array{std::move(fix)};
        }
        results.push_back(std::move(result));
    }
    const bool complete=report.getString("status")=="ready";
    llvm::json::Object driver{{"name","frost-analyzer"},{"version",LLVM_VERSION_STRING},{"rules",std::move(rules)}};
    llvm::json::Object run{{"tool",llvm::json::Object{{"driver",std::move(driver)}}},{"columnKind","unicodeCodePoints"}};
    run["invocations"]=llvm::json::Array{llvm::json::Object{{"executionSuccessful",complete}}};
    run["properties"]=std::move(report);run["results"]=std::move(results);
    return {{"version","2.1.0"},{"$schema","https://docs.oasis-open.org/sarif/sarif/v2.1.0/errata01/os/schemas/sarif-schema-2.1.0.json"},{"runs",llvm::json::Array{std::move(run)}}};
}
}
