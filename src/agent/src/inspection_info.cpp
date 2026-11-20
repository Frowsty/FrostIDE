// SPDX-License-Identifier: Apache-2.0
// Exact inspection descriptions reuse the existing native analyzer's single source of identities.
#include "frost/agent/ide_queries.hpp"
namespace frost::agent {
Result<rpc::Json> inspectionInfo(std::string_view requested) {
    if (requested.empty() || requested.size() > 256) return fail(ErrorCode::InvalidArgument, "inspection id is required and bounded");
#define FROST_ANALYZER_RULE(id,title,category,level,applicability,key,cfg) \
    if (requested == id) return rpc::Json::object().set("id", id).set("title", title).set("category", category) \
        .set("defaultLevel", level).set("applicability", applicability).set("settingKey", key) \
        .set("tier", cfg ? "T2-CFG" : "T2-AST").set("engine", "frost-analyzer").set("source", "source-owned-registry");
#include "../../../resources/analyzer_rules.inc"
#undef FROST_ANALYZER_RULE
    return fail(ErrorCode::NotFound, "inspection metadata is unavailable for this exact id; external catalogs are not synthesized");
}
}
