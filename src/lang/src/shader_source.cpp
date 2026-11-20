// SPDX-License-Identifier: Apache-2.0
// Literal include candidates with original-source provenance, never compiler search/variant guesses.
#include "frost/lang/shader_source.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/syntax/shader_source.hpp"

#include <algorithm>
#include <set>
namespace frost::lang {
namespace {
bool under(std::string_view path, std::string_view root) {
    auto prefix = documentKey(root);
    if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
        prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    return documentKey(path).starts_with(prefix);
}
struct Candidate {
    std::string path, provenance;
};
struct Resolution {
    std::vector<Candidate> candidates;
    std::string refusal;
};
Resolution resolve(io::IFileSystem& fs, const ShaderLanguageInput& input, const syntax::ShaderInclude& include) {
    Resolution out;
    if (!include.literal || include.target.empty()) {
        out.refusal = "Macro, escaped, continued or malformed include: no literal source candidate.";
        return out;
    }
    if (io::isAbsolutePath(include.target) || include.target.find_first_of("\\:\r\n") != std::string::npos) {
        out.refusal = "Only relative portable include names are navigable.";
        return out;
    }
    std::set<std::string> seen;
    auto add = [&](std::string path, std::string boundary, std::string provenance) {
        if (out.candidates.size() >= kShaderMaxCandidates)
            return;
        auto actual = fs.physicalPath(path);
        if (!actual || !under(*actual, boundary))
            return;
        auto status = fs.status(*actual);
        if (!status || status->type != io::FileType::Regular)
            return;
        if (seen.insert(documentKey(*actual)).second)
            out.candidates.push_back({std::move(*actual), std::move(provenance)});
    };
    const bool package = include.target.starts_with("Packages/");
    const bool asset = include.target.starts_with("Assets/");
    if (include.quoted && !package && !asset) {
        if (auto path = io::joinPath(io::parentPath(input.path), include.target))
            add(*path, input.physicalProjectRoot, "quoted include relative to original source directory");
    }
    if (include.quoted && (asset || package))
        if (auto path = io::joinPath(input.projectRoot, include.target))
            add(*path, input.physicalProjectRoot,
                asset ? "explicit Assets path in the current project source"
                      : "literal project Packages source candidate; active package resolution is not inferred");
    for (const auto& root : input.roots) {
        if (root.provenance.empty() || root.physicalDirectory.empty())
            continue;
        auto current = fs.physicalPath(root.directory);
        if (!current || documentKey(*current) != documentKey(root.physicalDirectory))
            continue;
        std::string_view suffix = include.target;
        if (!root.virtualPrefix.empty()) {
            if (!suffix.starts_with(root.virtualPrefix) || !root.virtualPrefix.ends_with('/'))
                continue;
            suffix.remove_prefix(root.virtualPrefix.size());
        } else if (package || asset)
            continue;
        if (auto path = io::joinPath(root.directory, suffix))
            add(*path, root.physicalDirectory, root.provenance);
    }
    if (out.candidates.empty())
        out.refusal = package           ? "No authoritative Unity package mapping is available for this source path."
                      : !include.quoted ? "Angle include requires an explicit compiler include-root mapping."
                                        : "No in-scope literal source candidate exists in the retained roots.";
    return out;
}
Json lspRange(const text::Snapshot& source, text::ByteRange range) {
    return toJson(LspRange{toLspPosition(source, range.begin, PositionEncoding::Utf16),
                           toLspPosition(source, range.end, PositionEncoding::Utf16)});
}
std::string describe(const syntax::ShaderInclude& include, const Resolution& result) {
    std::string text =
        "Shader source include candidate (not compiler-resolved)\n#" + include.directive + " " + include.target;
    if (include.conditional)
        text += "\nInside a conditional preprocessor region; activity and shader variants are not evaluated.";
    for (const auto& candidate : result.candidates)
        text += "\n" + candidate.path + " — " + candidate.provenance;
    if (!result.refusal.empty())
        text += "\n" + result.refusal;
    if (result.candidates.size() > 1)
        text += "\nMultiple source candidates; compiler include precedence is not inferred.";
    return text;
}
} // namespace
Result<Json> queryShaderSource(io::IFileSystem& fs, const ShaderLanguageInput& input, std::string_view method,
                               const Json& params, std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Shader source navigation cancelled");
    if (method != "textDocument/hover" && method != "textDocument/definition" && method != "textDocument/declaration" &&
        method != "textDocument/documentLink")
        return fail(ErrorCode::Unsupported, "Shader source provider supports literal include navigation only; "
                                            "configure a semantic registry provider for this request");
    if (input.source.size().value() > syntax::kShaderMaxSourceBytes || input.roots.size() > kShaderMaxIncludeRoots)
        return fail(ErrorCode::OutOfRange, "Shader source navigation exceeds its named source/root limits");
    auto actualRoot = fs.physicalPath(input.projectRoot);
    auto actualSource = fs.physicalPath(input.path);
    if (!actualRoot || documentKey(*actualRoot) != documentKey(input.physicalProjectRoot) || !actualSource ||
        !under(*actualSource, input.physicalProjectRoot))
        return fail(ErrorCode::PermissionDenied, "Shader source project identity or physical containment changed");
    const auto* document = params.find("textDocument");
    auto requested = document
                         ? uriToPath(document->stringAt("uri"))
                         : Result<std::string>(fail(ErrorCode::InvalidArgument, "Shader request needs a document URI"));
    if (!requested || documentKey(*requested) != documentKey(input.path))
        return fail(ErrorCode::Busy, "Shader source snapshot does not match the requested document");
    const auto source = input.source.text();
    auto scanned = syntax::scanShaderSource(source, syntax::shaderSourceKind(input.path), stop);
    if (!scanned.complete)
        return fail(ErrorCode::OutOfRange, "Shader source token/include limit exceeded");
    std::optional<ByteOffset> offset;
    if (const auto* position = params.find("position"))
        if (auto parsed = parsePosition(*position)) {
            auto byte = fromLspPosition(input.source, *parsed, PositionEncoding::Utf16);
            if (byte)
                offset = *byte;
        }
    Json result = method == "textDocument/hover" ? Json{} : Json::array();
    for (const auto& include : scanned.includes) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Shader source navigation cancelled");
        if (method != "textDocument/documentLink" &&
            (!offset || *offset < include.range.begin || *offset > include.range.end))
            continue;
        auto resolved = resolve(fs, input, include);
        const auto provenance = describe(include, resolved);
        if (method == "textDocument/hover") {
            result = Json::object()
                         .set("contents", Json::object().set("kind", "plaintext").set("value", provenance))
                         .set("range", lspRange(input.source, include.range));
            break;
        }
        if (method == "textDocument/definition" || method == "textDocument/declaration") {
            for (const auto& candidate : resolved.candidates)
                if (auto uri = pathToUri(candidate.path))
                    result.push(Json::object()
                                    .set("uri", *uri)
                                    .set("range", toJson(LspRange{}))
                                    .set("frostSourceProvenance", candidate.provenance));
        } else if (method == "textDocument/documentLink") {
            auto link = Json::object().set("range", lspRange(input.source, include.range)).set("tooltip", provenance);
            if (resolved.candidates.size() == 1)
                if (auto uri = pathToUri(resolved.candidates.front().path))
                    link.set("target", *uri);
            result.push(std::move(link));
        }
    }
    auto current = fs.physicalPath(input.projectRoot);
    if (!current || documentKey(*current) != documentKey(input.physicalProjectRoot))
        return fail(ErrorCode::Busy, "Shader source project changed during navigation");
    return result;
}
void ShaderSourceService::setInput(ShaderLanguageInput input) {
    LockGuard lock(m_mutex);
    m_input = std::move(input);
}
sched::Task<Result<Json>> ShaderSourceService::request(std::string method, Json params, std::stop_token stop) {
    LockGuard lock(m_mutex);
    return answer(m_fs, m_scheduler, m_input, std::move(method), std::move(params), stop);
}
sched::Task<Result<Json>> ShaderSourceService::answer(io::IFileSystem& fs, sched::IScheduler& scheduler,
                                                      ShaderLanguageInput input, std::string method, Json params,
                                                      std::stop_token stop) {
    co_await scheduler.schedule(sched::Lane::Background);
    co_return queryShaderSource(fs, input, method, params, stop);
}
std::optional<ServerCapabilities> ShaderSourceService::serverCapabilities() const {
    ServerCapabilities caps;
    caps.definition = caps.declaration = caps.hover = caps.documentLink = true;
    caps.serverName = "FrostIDE shader literal-source navigation";
    return caps;
}
} // namespace frost::lang
