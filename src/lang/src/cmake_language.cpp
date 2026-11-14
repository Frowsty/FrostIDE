// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/cmake_language.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_syntax.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
namespace frost::lang {
namespace {
bool word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.'; }
struct Symbol { std::string name, detail, path; std::size_t line = 0; int kind = 6; };
constexpr std::pair<std::string_view, std::string_view> commands[] = {
    {"add_executable", "add_executable(<name> [WIN32] [MACOSX_BUNDLE] [EXCLUDE_FROM_ALL] <sources>...)"},
    {"add_library", "add_library(<name> [STATIC | SHARED | MODULE | OBJECT | INTERFACE] <sources>...)"},
    {"add_custom_target", "add_custom_target(<name> [ALL] [COMMAND <command> [<args>...]]...)"},
    {"add_subdirectory", "add_subdirectory(<source_dir> [<binary_dir>] [EXCLUDE_FROM_ALL] [SYSTEM])"},
    {"cmake_minimum_required", "cmake_minimum_required(VERSION <min>[...<policy_max>] [FATAL_ERROR])"},
    {"project", "project(<name> [VERSION <version>] [DESCRIPTION <text>] [LANGUAGES <languages>...])"},
    {"set", "set(<variable> <value>... [PARENT_SCOPE]); set(<variable> <value>... CACHE <type> <docstring> [FORCE])"},
    {"unset", "unset(<variable> [CACHE | PARENT_SCOPE])"},
    {"option", "option(<variable> <help_text> [value])"},
    {"function", "function(<name> [<arguments>...]) ... endfunction()"},
    {"macro", "macro(<name> [<arguments>...]) ... endmacro()"},
    {"include", "include(<file|module> [OPTIONAL] [RESULT_VARIABLE <variable>] [NO_POLICY_SCOPE])"},
    {"find_package", "find_package(<PackageName> [version] [REQUIRED] [COMPONENTS <components>...])"},
    {"target_sources", "target_sources(<target> <INTERFACE|PUBLIC|PRIVATE> [items...]...)"},
    {"target_link_libraries", "target_link_libraries(<target> <PRIVATE|PUBLIC|INTERFACE> <items>...)"},
    {"target_include_directories", "target_include_directories(<target> [SYSTEM] [AFTER|BEFORE] <INTERFACE|PUBLIC|PRIVATE> <items>...)"},
    {"target_compile_definitions", "target_compile_definitions(<target> <INTERFACE|PUBLIC|PRIVATE> <items>...)"},
    {"target_compile_options", "target_compile_options(<target> [BEFORE] <INTERFACE|PUBLIC|PRIVATE> <items>...)"},
    {"target_compile_features", "target_compile_features(<target> <PRIVATE|PUBLIC|INTERFACE> <features>...)"},
    {"target_link_options", "target_link_options(<target> [BEFORE] <INTERFACE|PUBLIC|PRIVATE> <items>...)"},
    {"message", "message([<mode>] <message>...)"},
    {"if", "if(<condition>) ... [elseif(<condition>) ...] [else() ...] endif()"},
    {"foreach", "foreach(<loop_var> <items>) ... endforeach()"},
    {"while", "while(<condition>) ... endwhile()"},
    {"return", "return([PROPAGATE <variables>...])"},
    {"list", "list(<subcommand> <list> [arguments...])"},
    {"string", "string(<subcommand> [arguments...])"},
    {"file", "file(<subcommand> [arguments...])"},
    {"get_property", "get_property(<variable> <scope> PROPERTY <name> [SET | DEFINED | BRIEF_DOCS | FULL_DOCS])"},
    {"set_property", "set_property(<scope> [APPEND] [APPEND_STRING] PROPERTY <name> [values...])"},
    {"enable_testing", "enable_testing()"},
    {"add_test", "add_test(NAME <name> COMMAND <command> [<args>...] [CONFIGURATIONS <config>...] [WORKING_DIRECTORY <dir>])"},
    {"install", "install(<TARGETS|FILES|DIRECTORY|EXPORT|SCRIPT|CODE> <arguments>...)"}
};
Json location(const Symbol& s) {
    auto uri = pathToUri(s.path);
    if (!uri) return {};
    return Json::object().set("uri", *uri).set("range", toJson(LspRange{{static_cast<std::uint32_t>(s.line), 0},
                                                                {static_cast<std::uint32_t>(s.line), 0}}));
}
void declarations(std::vector<Symbol>& out, std::string_view path, std::string_view text) {
    const auto parsed = project::scanCMakeSyntaxPrefix(text);
    for (const auto& cmd : parsed) {
        if (cmd.arguments.empty() || !cmd.arguments.front().isLiteral) continue;
        const auto& name = cmd.arguments.front().literal;
        if (name.empty()) continue;
        if ((cmd.name == "function" || cmd.name == "macro") && cmd.scopeDepth == 0) {
            out.push_back({name, cmd.name + " " + name + " (source declaration)", std::string(path), cmd.line - 1, 3});
        } else if ((cmd.name == "set" || cmd.name == "option") && cmd.scopeDepth == 0) {
            // Do not claim conditional or function-local values are global values.
            std::string detail = cmd.name + " " + name + " (source declaration; not evaluated)";
            out.push_back({name, std::move(detail), std::string(path), cmd.line - 1, 6});
        } else if (cmd.name == "add_executable" || cmd.name == "add_library" || cmd.name == "add_custom_target") {
            out.push_back({name, cmd.name + " " + name + " (source declaration)", std::string(path), cmd.line - 1, 7});
        }
    }
}
} // namespace
bool isCMakeSource(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
    return name == "CMakeLists.txt" || name.ends_with(".cmake");
}
Result<Json> queryCMakeLanguage(const CMakeLanguageInput& input, std::string_view method, const Json& params,
                              std::stop_token stop) {
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "CMake analysis cancelled");
    if (method != "textDocument/completion" && method != "textDocument/definition" && method != "textDocument/hover")
        return fail(ErrorCode::Unsupported, "CMake language request is unsupported");
    const auto* pos = params.find("position");
    auto position = pos ? parsePosition(*pos) : std::nullopt;
    if (!position) return fail(ErrorCode::InvalidArgument, "CMake request requires a position");
    auto at = fromLspPosition(input.source, *position, PositionEncoding::Utf16);
    if (!at) return std::unexpected(at.error());
    const std::string source = input.source.text();
    std::size_t begin = static_cast<std::size_t>(at->value()), end = begin;
    while (begin && word(source[begin - 1])) --begin;
    while (end < source.size() && word(source[end])) ++end;
    const auto selected = std::string_view(source).substr(begin, end - begin);
    const auto prefix = std::string_view(source).substr(begin, static_cast<std::size_t>(at->value()) - begin);
    std::vector<Symbol> symbols;
    declarations(symbols, input.path, source);
    for (const auto& [path, text] : input.inputs) {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "CMake analysis cancelled");
        if (documentKey(path) != documentKey(input.path)) declarations(symbols, path, text);
    }
    if (input.model) {
        for (const auto& target : input.model->targets) {
            Symbol s{target.name, std::string(project::toString(target.type)) + " (configured target)", "", 0, 7};
            if (!target.backtrace.empty()) {
                s.path = target.backtrace.front().file;
                s.line = target.backtrace.front().line.value_or(1) - 1;
            }
            symbols.push_back(std::move(s));
        }
        for (const auto& entry : input.model->cache)
            symbols.push_back({entry.name, entry.type + " = " + entry.value + " (last configured cache)", "", 0, 6});
    }
    for (const auto& [name, signature] : commands)
        symbols.push_back({std::string(name), std::string(signature) + "\nCMake built-in command", "", 0, 3});
    Json result = Json::array();
    std::map<std::string, bool> seen;
    for (const auto& s : symbols) {
        if (method == "textDocument/completion") {
            if (!s.name.starts_with(prefix) || !seen.emplace(s.name, true).second) continue;
            result.push(Json::object().set("label", s.name).set("kind", s.kind).set("detail", s.detail));
        } else if (s.name == selected) {
            if (method == "textDocument/hover")
                return Json::object().set("contents", Json::object().set("kind", "plaintext").set("value", s.detail));
            if (!s.path.empty()) result.push(location(s));
        }
    }
    if (method == "textDocument/hover") return Json{};
    return result;
}
sched::Task<Result<Json>> CMakeLanguageService::request(std::string method, Json params, std::stop_token stop) {
    LockGuard lock(m_mutex);
    return answer(m_scheduler, m_reader, m_input, std::move(method), std::move(params), stop);
}
void CMakeLanguageService::setInput(CMakeLanguageInput input) {
    LockGuard lock(m_mutex);
    m_input = std::move(input);
}
sched::Task<Result<Json>> CMakeLanguageService::answer(sched::IScheduler& scheduler, InputReader reader,
                                                    CMakeLanguageInput input, std::string method, Json params,
                                                    std::stop_token stop) {
    co_await scheduler.schedule(sched::Lane::Background);
    const auto cancelled = [&] { return stop.stop_requested() || scheduler.stopToken().stop_requested(); };
    if (cancelled()) co_return fail(ErrorCode::Cancelled, "CMake input loading cancelled");
    std::size_t bytes = 0;
    std::size_t visited = 0;
    std::set<std::string> seen{documentKey(input.path)};
    for (const auto& [path, text] : input.inputs) {
        if (text.size() > kMaxInputBytes || text.size() > kMaxTotalInputBytes - bytes)
            co_return fail(ErrorCode::OutOfRange, "CMake input snapshot exceeds analysis budget");
        seen.insert(documentKey(path));
        bytes += text.size();
    }
    for (const auto& [path, snapshot] : input.openInputs) {
        if (cancelled()) co_return fail(ErrorCode::Cancelled, "CMake input loading cancelled");
        if (++visited > kMaxInputs || bytes >= kMaxTotalInputBytes) break;
        if (!seen.insert(documentKey(path)).second) continue;
        // Open documents remain authoritative even when too large to include in analysis.
        if (snapshot.size().value() > kMaxInputBytes || snapshot.size().value() > kMaxTotalInputBytes - bytes) continue;
        auto text = snapshot.text();
        bytes += text.size();
        input.inputs.emplace_back(path, std::move(text));
    }
    if (input.model && reader) for (const auto& file : input.model->inputs) {
        if (cancelled()) co_return fail(ErrorCode::Cancelled, "CMake input loading cancelled");
        if (file.generated || file.external) continue;
        if (++visited > kMaxInputs || bytes >= kMaxTotalInputBytes) break;
        if (!seen.insert(documentKey(file.path)).second) continue;
        const auto limit = std::min(kMaxInputBytes, kMaxTotalInputBytes - bytes);
        auto text = reader(file.path, limit);
        if (cancelled()) co_return fail(ErrorCode::Cancelled, "CMake input loading cancelled");
        if (!text || text->size() > limit) continue;
        bytes += text->size();
        input.inputs.emplace_back(file.path, std::move(*text));
    }
    co_return queryCMakeLanguage(input, method, params, stop);
}
std::optional<ServerCapabilities> CMakeLanguageService::serverCapabilities() const {
    ServerCapabilities caps;
    caps.completion = caps.definition = caps.hover = true;
    caps.serverName = "FrostIDE CMake source analysis";
    caps.completionTriggerCharacters = {"$", "{"};
    return caps;
}
} // namespace frost::lang
