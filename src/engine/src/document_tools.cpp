// SPDX-License-Identifier: Apache-2.0
// Bounded parser for authoritative engine validation issue envelopes.
#include "frost/engine/document_tools.hpp"

#include "frost/io/path.hpp"

#include <limits>
namespace frost::engine {
Result<DocumentValidation> parseDocumentValidation(const rpc::Json& reply, std::string_view expectedAsset) {
    if (!reply.isObject())
        return fail(ErrorCode::Corrupt, "Engine validation reply is not an object");
    if (!expectedAsset.empty() && (reply.stringAt("protocol") != "frostengine.document-validation" ||
                                   reply.integerAt("version") != 1 || reply.stringAt("path") != expectedAsset))
        return fail(ErrorCode::Corrupt, "Engine document validation identity/profile differs from the captured image");
    const auto* issues = reply.find("issues");
    const auto errors = reply.integerAt("errors"), warnings = reply.integerAt("warnings");
    if (!issues || !issues->isArray() || issues->size() > 10000 || !errors || !warnings || *errors < 0 || *warnings < 0)
        return fail(ErrorCode::Corrupt, "Malformed or oversized engine validation issue envelope");
    DocumentValidation result;
    std::uint64_t errorCount = 0, warningCount = 0;
    std::size_t bytes = 0;
    for (const auto& item : issues->items()) {
        DocumentIssue issue;
        issue.severity = item.stringAt("severity");
        issue.code = item.stringAt("code");
        issue.message = item.stringAt("message");
        if (expectedAsset.empty()) {
            if (issue.severity == "Error")
                issue.severity = "error";
            else if (issue.severity == "Warning")
                issue.severity = "warning";
            else if (issue.severity == "Info")
                issue.severity = "info";
        }
        const auto* location = item.find("location");
        if (!item.isObject() || !location || !location->isObject() || issue.message.empty() ||
            (issue.severity != "error" && issue.severity != "warning" && issue.severity != "info"))
            return fail(ErrorCode::Corrupt, "Malformed engine validation issue");
        issue.asset = location->stringAt("asset");
        issue.location = location->stringAt("jsonPath");
        if (const auto row = location->stringAt("rowKey"); !row.empty())
            issue.location += " row " + std::string(row);
        if (const auto field = location->stringAt("field"); !field.empty())
            issue.location += " field " + std::string(field);
        if (!expectedAsset.empty() && issue.asset != expectedAsset)
            return fail(ErrorCode::Corrupt, "Document validator returned another asset");
        if (!issue.asset.empty()) {
            if ((!expectedAsset.empty() && !issue.asset.starts_with("Assets/")) ||
                issue.asset.find('\\') != issue.asset.npos || io::isAbsolutePath(issue.asset))
                return fail(ErrorCode::Corrupt, "Engine validation asset is outside the relative project namespace");
            for (std::size_t start = 0; start <= issue.asset.size();) {
                const auto end = issue.asset.find('/', start);
                const auto part =
                    std::string_view(issue.asset).substr(start, end == issue.asset.npos ? end : end - start);
                if (part.empty() || part == "." || part == ".." || part.find(':') != part.npos)
                    return fail(ErrorCode::Corrupt, "Engine validation asset contains an unsafe path component");
                if (end == issue.asset.npos)
                    break;
                start = end + 1;
            }
        }
        const auto number = [&](std::string_view name, std::uint64_t& out) -> bool {
            if (!location->find(name))
                return true;
            const auto n = location->integerAt(name);
            if (!n || *n < 0 || *n > std::numeric_limits<int>::max())
                return false;
            out = static_cast<std::uint64_t>(*n);
            return true;
        };
        if (!number("line", issue.line) || !number("column", issue.column) || !number("byte", issue.byte))
            return fail(ErrorCode::Corrupt, "Engine validation source coordinate is invalid");
        bytes += issue.message.size() + issue.asset.size() + issue.location.size() + issue.code.size();
        if (bytes > 4 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "Engine validation issues exceed 4 MiB");
        errorCount += issue.severity == "error";
        warningCount += issue.severity == "warning";
        result.issues.push_back(std::move(issue));
    }
    if (errorCount != static_cast<std::uint64_t>(*errors) || warningCount != static_cast<std::uint64_t>(*warnings))
        return fail(ErrorCode::Corrupt, "Engine validation counters disagree with actual issues");
    result.valid = errorCount == 0;
    if (!expectedAsset.empty()) {
        const auto* valid = reply.find("valid");
        if (!valid || !valid->asBool() || *valid->asBool() != result.valid)
            return fail(ErrorCode::Corrupt, "Engine document validity disagrees with issues");
    }
    return result;
}
} // namespace frost::engine
