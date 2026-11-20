// SPDX-License-Identifier: Apache-2.0
// MSBuild completion and source/engine-origin navigation with bounded literal import loading.
#include "frost/lang/msbuild_language.hpp"
#include "frost/lang/uri.hpp"
#include "frost/io/path.hpp"
#include <algorithm>
#include <cctype>
#include <set>
namespace frost::lang {
namespace {
Json location(std::string_view path, std::size_t line, std::size_t column = 0) {
    auto uri = pathToUri(path);
    if (!uri) return {};
    return Json::object().set("uri", *uri).set("range", toJson(LspRange{
        {static_cast<std::uint32_t>(line),static_cast<std::uint32_t>(column)},
        {static_cast<std::uint32_t>(line),static_cast<std::uint32_t>(column)}}));
}
bool word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'; }
}
Result<Json> queryMSBuildLanguage(const MSBuildLanguageInput& input, std::string_view method,
                                 const Json& params, std::stop_token stop) {
    if (method != "textDocument/completion" && method != "textDocument/definition" && method != "textDocument/hover")
        return fail(ErrorCode::Unsupported, "MSBuild language request is unsupported");
    if (input.source.size().value()>1024U*1024U) return fail(ErrorCode::OutOfRange,"MSBuild source exceeds language budget");
    const auto* position = params.find("position");
    auto parsedPosition = position ? parsePosition(*position) : std::nullopt;
    if (!parsedPosition) return fail(ErrorCode::InvalidArgument, "MSBuild request requires a position");
    auto offset = fromLspPosition(input.source, *parsedPosition, PositionEncoding::Utf16);
    if (!offset) return std::unexpected(offset.error());
    const auto source = input.source.text();
    auto begin = static_cast<std::size_t>(offset->value()), end = begin;
    while (begin && word(source[begin-1])) --begin;
    while (end < source.size() && word(source[end])) ++end;
    const auto selected = source.substr(begin,end-begin);
    const auto prefix = source.substr(begin, static_cast<std::size_t>(offset->value())-begin);
    auto files = input.inputs;
    files.insert(files.begin(),{input.path,source});
    Json result = Json::array();
    std::set<std::string> seen;
    bool evaluatedDefinition = false;
    for (const auto& evaluation : input.evaluations) {
        const auto context = evaluation.configuration + "|" + evaluation.platform + " — last evaluated " + evaluation.projectPath;
        if (method=="textDocument/definition")
            for (const auto& imported:evaluation.importDefinitions) {
                if (documentKey(imported.sourcePath)!=documentKey(input.path) || imported.line!=parsedPosition->line+1) continue;
                auto xml=project::parseMSBuildSource(source,input.path);
                if (!xml) continue;
                for (const auto& current:xml->imports) {
                    const auto close=source.find('>',current.offset);
                    if (current.expression==imported.expression && current.offset<=begin && close!=source.npos && begin<=close &&
                        std::ranges::any_of(files,[&](const auto& file){return documentKey(file.first)==documentKey(imported.importedPath);}))
                        result.push(location(imported.importedPath,0));
                }
            }
        for (const auto& property : evaluation.propertyDefinitions) {
            if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "MSBuild query cancelled");
            const auto detail = property.value + " (" + context + ")";
            if (method == "textDocument/completion" && property.name.starts_with(prefix) && seen.insert(property.name).second)
                result.push(Json::object().set("label",property.name).set("kind",6).set("detail",detail));
            if (property.name != selected) continue;
            if (method == "textDocument/hover")
                return Json::object().set("contents",Json::object().set("kind","plaintext").set("value",detail));
            if (method != "textDocument/definition" || property.path.empty() || property.line == 0) continue;
            const auto file = std::find_if(files.begin(),files.end(),[&](const auto& row){return documentKey(row.first)==documentKey(property.path);});
            if (file == files.end()) continue;
            auto xml = project::parseMSBuildSource(file->second,file->first);
            if (!xml) continue;
            // An origin which no longer identifies that source declaration is stale, never guessed.
            if (std::ranges::any_of(xml->properties,[&](const auto& row){return row.name==property.name && row.line+1==property.line;})) {
                result.push(location(property.path,property.line-1,property.column ? property.column-1 : 0));
                evaluatedDefinition = true;
            }
        }
    }
    for (const auto& [path,bytes] : files) {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "MSBuild query cancelled");
        auto xml = project::parseMSBuildSource(bytes,path);
        if (!xml) continue;
        auto declarations = xml->properties;
        declarations.insert(declarations.end(),xml->targets.begin(),xml->targets.end());
        for (const auto& row : declarations) {
            const auto detail = row.name + " (source declaration; not evaluated)" +
                (row.condition.empty() ? "" : " Condition: " + row.condition);
            if (method == "textDocument/completion" && row.name.starts_with(prefix) && seen.insert(row.name).second)
                result.push(Json::object().set("label",row.name).set("kind",6).set("detail",detail));
            if (row.name != selected) continue;
            if (method == "textDocument/hover")
                return Json::object().set("contents",Json::object().set("kind","plaintext").set("value",detail));
            if (method == "textDocument/definition" && !evaluatedDefinition) result.push(location(path,row.line));
        }
        if (method == "textDocument/definition" && documentKey(path)==documentKey(input.path))
            for (const auto& imported : xml->imports) {
                const auto close = bytes.find('>',imported.offset);
                if (!imported.literal || begin < imported.offset || close == bytes.npos || begin > close) continue;
                auto target = io::isAbsolutePath(imported.expression) ? io::normalizePath(imported.expression) :
                    io::joinPath(io::parentPath(path), imported.expression);
                if (target && std::ranges::any_of(files,[&](const auto& file){return documentKey(file.first)==documentKey(*target);}))
                    result.push(location(*target,0));
            }
    }
    if (method == "textDocument/completion")
        for (const auto* name : {"Project","PropertyGroup","ItemGroup","Import","ImportGroup","Target","Condition","Name","ProjectReference","ClCompile","ClInclude"})
            if (std::string_view(name).starts_with(prefix) && seen.insert(name).second)
                result.push(Json::object().set("label",name).set("kind",14).set("detail","MSBuild XML element/attribute"));
    return method == "textDocument/hover" ? Json{} : result;
}
void MSBuildLanguageService::setInput(MSBuildLanguageInput input) { LockGuard lock(m_mutex); m_input=std::move(input); }
sched::Task<Result<Json>> MSBuildLanguageService::request(std::string method,Json params,std::stop_token stop) {
    LockGuard lock(m_mutex);
    return answer(m_scheduler,m_reader,m_input,std::move(method),std::move(params),stop);
}
sched::Task<Result<Json>> MSBuildLanguageService::answer(sched::IScheduler& scheduler,InputReader reader,
    MSBuildLanguageInput input,std::string method,Json params,std::stop_token stop) {
    co_await scheduler.schedule(sched::Lane::Background);
    std::set<std::string> seen{documentKey(input.path)};
    std::size_t total=input.source.size().value();
    for (const auto& [path,snapshot] : input.openInputs) {
        if (total+snapshot.size().value()>4U*1024U*1024U || snapshot.size().value()>1024U*1024U)
            co_return fail(ErrorCode::OutOfRange,"MSBuild open inputs exceed language budget");
        if (seen.insert(documentKey(path)).second) { auto bytes=snapshot.text(); total+=bytes.size(); input.inputs.emplace_back(path,std::move(bytes)); }
    }
    std::vector<std::string> pending;
    for (const auto& evaluation : input.evaluations) {
        pending.push_back(evaluation.projectPath);
        pending.insert(pending.end(),evaluation.imports.begin(),evaluation.imports.end());
    }
    const auto queueImports=[&](std::string_view path,std::string_view bytes) {
        auto xml=project::parseMSBuildSource(bytes,path);
        if (xml) for (const auto& imported : xml->imports) if (imported.literal) {
            auto target=io::isAbsolutePath(imported.expression)?io::normalizePath(imported.expression):io::joinPath(io::parentPath(path),imported.expression);
            if (target) pending.push_back(*target);
        }
    };
    queueImports(input.path,input.source.text());
    for (const auto& [path,bytes] : input.inputs) queueImports(path,bytes);
    for (std::size_t i=0;i<pending.size();++i) {
        if (stop.stop_requested() || scheduler.stopToken().stop_requested()) co_return fail(ErrorCode::Cancelled,"MSBuild input loading cancelled");
        if (!seen.insert(documentKey(pending[i])).second) continue;
        if (seen.size()>256 || total>=4U*1024U*1024U) co_return fail(ErrorCode::OutOfRange,"MSBuild import graph exceeds language budget");
        auto bytes=reader(pending[i],std::min<std::size_t>(1024U*1024U,4U*1024U*1024U-total));
        if (!bytes) continue; // Missing literal imports remain unresolved; never evaluated or invented.
        total+=bytes->size();
        input.inputs.emplace_back(pending[i],*bytes);
        queueImports(pending[i],*bytes);
    }
    co_return queryMSBuildLanguage(input,method,params,stop);
}
std::optional<ServerCapabilities> MSBuildLanguageService::serverCapabilities() const {
    ServerCapabilities caps; caps.completion=caps.definition=caps.hover=true;
    caps.serverName="FrostIDE MSBuild source/last-evaluation navigation";
    caps.completionTriggerCharacters={"$","(","<"}; return caps;
}
} // namespace frost::lang
