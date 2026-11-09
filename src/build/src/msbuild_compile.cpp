// SPDX-License-Identifier: Apache-2.0
//
// The ClCompile item query of the single-file compile (see frost/build/msbuild_compile.hpp).

#include "frost/build/msbuild_compile.hpp"

#include "frost/io/path.hpp"

#include "json.hpp"

#include <algorithm>
#include <format>

namespace frost::build {

namespace {

constexpr io::PathStyle kWindows = io::PathStyle::Windows;

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// `path` normalised with Windows rules and lower-cased, for comparing two spellings of one file;
// "" when it does not normalise.
std::string pathKey(std::string_view path) {
    std::string key = io::normalizePath(path, kWindows).value_or(std::string());
    std::ranges::replace(key, '/', '\\');
    for (char& c : key) {
        c = lowerAscii(c);
    }
    return key;
}

std::string_view trimmedOutput(std::string_view text) noexcept {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    return text.size() > 2000 ? text.substr(0, 2000) : text;
}

} // namespace

Result<io::ProcessSpec> compileItemQuerySpec(const RunPropertyRequest& request) {
    // The property query's command line (same project, configuration, Solution* properties and
    // environment) with its -getProperty switches replaced by the one item query.
    auto spec = runPropertyQuerySpec(request);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    std::erase_if(spec->arguments, [](const std::string& argument) { return argument.starts_with("-getProperty:"); });
    spec->arguments.insert(spec->arguments.begin() + 1, "-getItem:ClCompile");
    spec->arguments.emplace_back("-nodeReuse:false");
    return spec;
}

Result<std::vector<CompileItem>> parseCompileItems(std::string_view json) {
    auto document = detail::parseJson(json);
    if (!document) {
        return std::unexpected(std::move(document.error()).withContext("MSBuild's -getItem output"));
    }
    const detail::JsonValue* items = document->find("Items");
    if (items == nullptr || items->kind != detail::JsonValue::Kind::Object) {
        return fail(ErrorCode::Corrupt, "MSBuild's -getItem output has no \"Items\" object");
    }
    std::vector<CompileItem> result;
    const detail::JsonValue* compile = items->find("ClCompile");
    if (compile == nullptr) {
        return result;
    }
    if (compile->kind != detail::JsonValue::Kind::Array) {
        return fail(ErrorCode::Corrupt, "MSBuild's -getItem output has a ClCompile entry that is not a list");
    }
    for (const detail::JsonValue& item : compile->items) {
        if (item.kind != detail::JsonValue::Kind::Object) {
            return fail(ErrorCode::Corrupt, "MSBuild's -getItem output lists a ClCompile item that is not an object");
        }
        result.push_back({.identity = item.stringMember("Identity"), .fullPath = item.stringMember("FullPath")});
    }
    return result;
}

Result<std::string> selectedFileIdentity(const std::vector<CompileItem>& items, std::string_view filePath) {
    const std::string wanted = pathKey(filePath);
    if (!wanted.empty()) {
        for (const CompileItem& item : items) {
            if (!item.identity.empty() && pathKey(item.fullPath) == wanted) {
                return item.identity;
            }
        }
    }
    return fail(ErrorCode::NotFound, std::format("{} is not a C/C++ source the project compiles (MSBuild lists {} "
                                                 "ClCompile item{}, and none is this file)",
                                                 filePath, items.size(), items.size() == 1 ? "" : "s"));
}

Result<std::vector<CompileItem>> queryCompileItems(io::IProcessLauncher& launcher, const RunPropertyRequest& request,
                                                   IClock::Duration timeout) {
#ifdef _WIN32
    auto spec = compileItemQuerySpec(request);
    if (!spec) {
        return std::unexpected(std::move(spec.error()));
    }
    auto ran = io::runToCompletion(launcher, *spec, {}, timeout);
    if (!ran) {
        return std::unexpected(std::move(ran.error()));
    }
    if (ran->exitCode != 0) {
        const std::string output = ran->stdoutBytes + ran->stderrBytes;
        if (output.find("MSB1001") != std::string::npos) {
            return fail(ErrorCode::Unsupported,
                        std::format("{} does not know -getItem (MSBuild 17.8 or newer is needed)", spec->executable));
        }
        return fail(ErrorCode::IoError,
                    std::format("MSBuild could not evaluate {} ({}|{}), exit code {}: {}", request.projectPath,
                                request.configuration, request.platform, ran->exitCode, trimmedOutput(output)));
    }
    return parseCompileItems(ran->stdoutBytes);
#else
    static_cast<void>(launcher);
    static_cast<void>(request);
    static_cast<void>(timeout);
    return fail(ErrorCode::Unsupported, "asking MSBuild for compile items needs Windows");
#endif
}

} // namespace frost::build
