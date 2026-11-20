// SPDX-License-Identifier: Apache-2.0
// No IDE-authored merge answers: only bounded, hash-correlated engine reports become previews.
#include "frost/engine/scene_merge.hpp"

#include "frost/engine/handle.hpp"
#include "frost/io/spawn_log.hpp"

#include <map>
#include <set>
namespace frost::engine {
namespace {
constexpr std::size_t imageLimit = 1024 * 1024;
bool assetPath(std::string_view path) {
    if (!path.starts_with("Assets/") || (!path.ends_with(".fscene") && !path.ends_with(".fprefab")) ||
        path.find_first_of("\\:\r\n") != path.npos)
        return false;
    for (std::size_t at = 0; at <= path.size();) {
        const auto end = path.find('/', at);
        const auto part = path.substr(at, end == path.npos ? end : end - at);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (char byte : part) {
            const auto c = static_cast<unsigned char>(byte);
            if (c < 32 || c == 127)
                return false;
        }
        if (end == path.npos)
            break;
        at = end + 1;
    }
    return true;
}
Result<std::vector<SceneMergeIssue>> issues(const rpc::Json* array, bool result) {
    if (!array || !array->isArray() || array->size() > 10000)
        return fail(ErrorCode::Corrupt, "Malformed engine merge diagnostic list");
    std::vector<SceneMergeIssue> out;
    for (const auto& item : array->items()) {
        SceneMergeIssue issue{std::string(item.stringAt("phase")), std::string(item.stringAt("severity")),
                              std::string(item.stringAt("location")), std::string(item.stringAt("message"))};
        if (!item.isObject() || issue.message.empty() || (issue.severity != "error" && issue.severity != "warning") ||
            (result ? issue.phase != "result"
                    : issue.phase != "base" && issue.phase != "ours" && issue.phase != "theirs"))
            return fail(ErrorCode::Corrupt, "Malformed engine merge diagnostic");
        out.push_back(std::move(issue));
    }
    return out;
}
} // namespace
Result<rpc::Json> sceneMergeParams(const SceneMergeRequest& request) {
    const auto& source = request.images;
    if (!assetPath(source.path) || source.base.size() > imageLimit || source.ours.size() > imageLimit ||
        source.theirs.size() > imageLimit || (!source.baseExists && !source.base.empty()) ||
        request.choices.size() > 1024 || (request.candidate && request.candidate->size() > 4 * imageLimit))
        return fail(ErrorCode::OutOfRange, "Scene merge requires bounded actual scene/prefab stage images");
    auto choices = rpc::Json::object();
    for (const auto& choice : request.choices) {
        if (choice.path.size() > 32768 || choices.find(choice.path) ||
            (choice.side != "ours" && choice.side != "theirs" && choice.side != "base"))
            return fail(ErrorCode::InvalidArgument,
                        "Scene merge choices must uniquely name a reported conflict and source side");
        choices.set(choice.path, choice.side);
    }
    auto params = rpc::Json::object()
                      .set("path", source.path)
                      .set("base", source.base)
                      .set("ours", source.ours)
                      .set("theirs", source.theirs)
                      .set("baseExists", source.baseExists)
                      .set("choices", choices);
    if (request.candidate)
        params.set("candidate", *request.candidate);
    return params;
}
Result<SceneMergeReport> parseSceneMergeReport(const rpc::Json& reply, const SceneMergeRequest& request) {
    auto admitted = sceneMergeParams(request);
    if (!admitted)
        return std::unexpected(admitted.error());
    const auto& expected = request.images;
    const auto* sources = reply.find("sources");
    const auto* validation = reply.find("validation");
    const auto* result = reply.find("result");
    if (reply.dump().size() > 8 * imageLimit || reply.stringAt("protocol") != "frostengine.scene-merge" ||
        reply.integerAt("version") != 1 || reply.stringAt("path") != expected.path || !sources ||
        !sources->isObject() || sources->boolAt("baseExists") != expected.baseExists ||
        sources->stringAt("base") != (expected.baseExists ? io::sha256Hex(expected.base) : "") ||
        sources->stringAt("ours") != io::sha256Hex(expected.ours) ||
        sources->stringAt("theirs") != io::sha256Hex(expected.theirs) ||
        reply.stringAt("mode") != (request.candidate ? "edited" : "semantic") ||
        reply.stringAt("candidateSha256") != (request.candidate ? io::sha256Hex(*request.candidate) : "") || !result ||
        !result->isString() || result->asString()->size() > 4 * imageLimit || !validation || !validation->isObject() ||
        !validation->boolAt("valid") || !reply.boolAt("clean"))
        return fail(ErrorCode::Corrupt, "Engine scene merge source/profile identity differs from the retained request");
    SceneMergeReport report;
    report.result = *result->asString();
    report.valid = *validation->boolAt("valid");
    report.clean = *reply.boolAt("clean");
    report.edited = request.candidate.has_value();
    auto output = rpc::parseJson(report.result);
    const auto* entities = output ? output->find("Entities") : nullptr;
    if (!output || !output->isObject() || !entities || !entities->isArray() || entities->size() > 10000)
        return fail(ErrorCode::Corrupt, "Engine merge result is not a bounded scene entity document");
    std::set<std::uint64_t> ids;
    for (const auto& entity : entities->items()) {
        const auto id = engineHandleAt(entity, "ID");
        if (!entity.isObject() || !id || !id->value || !ids.insert(id->value).second)
            return fail(ErrorCode::Corrupt, "Engine merge result contains ambiguous entity identities");
    }
    auto parsedIssues = issues(validation->find("issues"), true), inputs = issues(reply.find("inputIssues"), false);
    if (!parsedIssues)
        return std::unexpected(parsedIssues.error());
    if (!inputs)
        return std::unexpected(inputs.error());
    report.issues = std::move(*parsedIssues);
    report.inputIssues = std::move(*inputs);
    bool hasErrors = false;
    for (const auto& issue : report.issues)
        hasErrors |= issue.severity == "error";
    if (report.valid == hasErrors)
        return fail(ErrorCode::Corrupt, "Engine merge validity disagrees with actual diagnostics");
    const auto* conflicts = reply.find("conflicts");
    const auto unresolved = reply.unsignedAt("unresolved");
    if (!conflicts || !conflicts->isArray() || conflicts->size() > 1024 || !unresolved)
        return fail(ErrorCode::Corrupt, "Malformed engine merge conflict envelope");
    std::map<std::string, std::string> choices;
    for (const auto& item : request.choices)
        choices.emplace(item.path, item.side);
    std::set<std::string> paths;
    std::uint64_t pending = 0;
    for (const auto& item : conflicts->items()) {
        SceneMergeConflict conflict;
        conflict.path = item.stringAt("path");
        conflict.kind = item.stringAt("kind");
        conflict.choice = item.stringAt("choice");
        if (!item.isObject() || conflict.path.size() > 32768 || !paths.insert(conflict.path).second ||
            (conflict.kind != "value" && conflict.kind != "delete-edit" && conflict.kind != "entity-add-collision"))
            return fail(ErrorCode::Corrupt, "Malformed engine merge conflict identity");
        const auto choice = choices.find(conflict.path);
        if (conflict.choice != (choice == choices.end() ? "" : choice->second))
            return fail(ErrorCode::Corrupt, "Engine merge did not retain the explicit conflict choice");
        const auto value = [&](const char* flag, const char* key, std::optional<rpc::Json>& out) -> bool {
            const auto present = item.boolAt(flag);
            const auto* actual = item.find(key);
            if (!present || *present != (actual != nullptr))
                return false;
            if (actual)
                out = *actual;
            return true;
        };
        if (!value("basePresent", "base", conflict.base) || !value("oursPresent", "ours", conflict.ours) ||
            !value("theirsPresent", "theirs", conflict.theirs))
            return fail(ErrorCode::Corrupt, "Engine merge conflict presence disagrees with its side values");
        pending += conflict.choice.empty();
        report.conflicts.push_back(std::move(conflict));
    }
    for (const auto& [path, choice] : choices)
        if (!paths.contains(path))
            return fail(ErrorCode::Corrupt, "Engine merge ignored a retained conflict decision");
    report.unresolved = *unresolved;
    if (report.unresolved != (report.edited ? 0 : pending) || report.clean != (report.valid && report.unresolved == 0))
        return fail(ErrorCode::Corrupt, "Engine merge resolution/validity counters disagree");
    return report;
}
} // namespace frost::engine
