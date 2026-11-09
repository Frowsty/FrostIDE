// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild evaluation oracle of SLN-0: this machine's real MSBuild.exe, run directly by the
// test (not through frost::project's evaluator), asked for every file item type with
// -getItem/-getProperty and for the import closure with -pp. Its answers are the expected values
// of the ProjectHost and Solution Explorer tests. Header-only; Windows test code only.
#pragma once

#include "frost/build/msbuild_locator.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/vcxproj.hpp"

#include "json.hpp"
#include "paths.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project::test {

// This machine's MSBuild.exe (vswhere), or nullopt when no Visual Studio is installed (the test
// then reports itself skipped). Any other discovery failure is a test failure.
inline std::optional<build::VsInstance> machineMsBuild() {
    auto found = build::findMsBuild(io::realProcessLauncher(), io::realFileSystem());
    if (!found) {
        if (found.error().code() != ErrorCode::NotFound) {
            ADD_FAILURE() << found.error().describe();
        }
        return std::nullopt;
    }
    return *found;
}

// One item as the oracle reports it.
struct OracleItem {
    std::string type;
    std::string fullPath; // Normalised.
    std::map<std::string, std::string, std::less<>> metadata;
};

struct OracleEvaluation {
    std::vector<OracleItem> items;
    std::map<std::string, std::string, std::less<>> properties;
    // Normalised import paths from the -pp banners (the project file itself excluded).
    std::set<std::string> imports;
};

/*
 * Runs the oracle for one project configuration. `metadata` names item metadata to collect;
 * `properties` property values. The global properties are evaluationGlobalProperties(): the
 * same input the product passes, so both sides evaluate the same project.
 */
inline std::optional<OracleEvaluation> runOracle(const std::string& msbuild, const ProjectEvaluationRequest& request,
                                                 const std::vector<std::string>& metadata = {},
                                                 const std::vector<std::string>& properties = {},
                                                 bool withImports = false) {
    std::string types;
    for (std::string_view name : fileItemTypeNames()) {
        types += (types.empty() ? "" : ";") + std::string(name);
    }
    io::ProcessSpec spec;
    spec.executable = msbuild;
    spec.arguments = {request.projectPath, "-nologo", "-nodeReuse:false", "-getItem:" + types,
                      "-getProperty:MSBuildProjectFullPath"};
    for (const std::string& name : properties) {
        spec.arguments.back() += ";" + name;
    }
    for (const auto& [name, value] : evaluationGlobalProperties(request)) {
        spec.arguments.push_back("-p:" + name + "=" + value);
    }
    spec.workingDirectory = std::string(io::parentPath(request.projectPath));
    spec.environment = {{"MSBUILDDISABLENODEREUSE", "1"}, {"MSBUILDTERMINALLOGGER", "off"}};
    auto launched = io::realProcessLauncher().launch(spec);
    if (!launched) {
        ADD_FAILURE() << launched.error().describe();
        return std::nullopt;
    }
    auto ran = (*launched)->communicate({}, std::chrono::minutes(5));
    if (!ran || ran->exitCode != 0) {
        ADD_FAILURE() << "MSBuild oracle failed for " << request.projectPath << ": "
                      << (ran ? ran->stdoutBytes + ran->stderrBytes : ran.error().describe());
        return std::nullopt;
    }
    const std::size_t brace = ran->stdoutBytes.find('{');
    auto json = detail::parseJson(brace == std::string::npos ? std::string_view{}
                                                             : std::string_view(ran->stdoutBytes).substr(brace));
    if (!json || !json->isObject()) {
        ADD_FAILURE() << "the oracle printed no JSON: " << ran->stdoutBytes;
        return std::nullopt;
    }
    OracleEvaluation out;
    if (const detail::JsonNode* props = json->find("Properties")) {
        for (std::size_t i = 0; i < props->keys.size(); ++i) {
            out.properties.emplace(props->keys[i], props->items[i].text);
        }
    }
    const detail::JsonNode* items = json->find("Items");
    if (items == nullptr || !items->isObject()) {
        ADD_FAILURE() << "the oracle printed no Items";
        return std::nullopt;
    }
    for (std::size_t g = 0; g < items->keys.size(); ++g) {
        for (const detail::JsonNode& item : items->items[g].items) {
            OracleItem parsed;
            parsed.type = items->keys[g];
            parsed.fullPath = detail::normalizeAbsolute(item.stringAt("FullPath")).value_or(item.stringAt("FullPath"));
            for (const std::string& name : metadata) {
                parsed.metadata.emplace(name, item.stringAt(name));
            }
            out.items.push_back(std::move(parsed));
        }
    }
    if (withImports) {
        frost::test::ScratchDir scratch("oracle-pp");
        EXPECT_TRUE(io::realFileSystem().createDirectories(scratch.path()).has_value());
        const std::string preprocessed = scratch.join("pp.xml");
        io::ProcessSpec pp = spec;
        pp.arguments = {request.projectPath, "-nologo", "-nodeReuse:false", "-pp:" + preprocessed};
        for (const auto& [name, value] : evaluationGlobalProperties(request)) {
            pp.arguments.push_back("-p:" + name + "=" + value);
        }
        auto ppLaunched = io::realProcessLauncher().launch(pp);
        auto ppRan = ppLaunched ? (*ppLaunched)->communicate({}, std::chrono::minutes(5))
                                : Result<io::ProcessResult>(std::unexpected(ppLaunched.error()));
        auto text = io::realFileSystem().readFile(preprocessed);
        if (!ppRan || ppRan->exitCode != 0 || !text) {
            ADD_FAILURE() << "MSBuild -pp failed for " << request.projectPath;
            return std::nullopt;
        }
        // Each import is announced by a banner: a "=====" rule, the import element, the absolute
        // path of the imported file, and another rule.
        const std::string projectKey = detail::pathKey(request.projectPath);
        bool inBanner = false;
        std::size_t start = 0;
        const std::string_view all(*text);
        while (start < all.size()) {
            const std::size_t end = std::min(all.find('\n', start), all.size());
            std::string_view line = all.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            if (line.starts_with("=====")) {
                inBanner = !inBanner;
                continue;
            }
            if (!inBanner || line.empty() || line.starts_with('<')) {
                continue;
            }
            if (io::isAbsolutePath(line)) {
                // MSBuild writes "--" in a path as "__" inside its XML comments; take the file on disk.
                std::string candidate(line);
                if (auto normalized = detail::normalizeAbsolute(candidate);
                    normalized && detail::pathKey(*normalized) != projectKey) {
                    out.imports.insert(detail::pathKey(*normalized));
                }
            }
        }
    }
    return out;
}

// The oracle's distinct item paths (path keys), sorted: what the Solution Explorer must show.
inline std::vector<std::string> oraclePathKeys(const OracleEvaluation& oracle) {
    std::set<std::string> keys;
    for (const OracleItem& item : oracle.items) {
        keys.insert(detail::pathKey(item.fullPath));
    }
    return {keys.begin(), keys.end()};
}

} // namespace frost::project::test
