// SPDX-License-Identifier: Apache-2.0
//
// The engine contract corpus tests (FEC-1 and FER-1a acceptance; labels "integration" and "corpus";
// Windows only, see tests/engine/CMakeLists.txt): FrostEditor of the pinned FrostEngine clone, built
// with `cmake --build --target FrostEditor` under the corpus build lock, run through frost::engine.
/*
 * What they check. (1) The engine's schema.export validates against schema version 1, and every
 * source location in it (components, fields, Lua bindings, events) resolves to an existing line of
 * the clone. (2) Entity ids above 2^53 survive the round trip: entity.list returns exactly the ids
 * of a scene file, and entity.get with each id written back as a JSON integer (and as the engine's
 * decimal-string form) returns that entity, while the neighbouring id (equal as a double) is not
 * found.
 *
 * Oracles. The component names come from the engine's own component.types, the command names from
 * commands.list, the ids from the scene file's text (read as digits, never through a JSON double),
 * the registration lines from the engine source text (searched for the registration statements
 * named below, never line numbers). The corpus path comes from $FROST_CORPORA_ROOT (tools\verify.ps1
 * -Corpus sets it), the pin and preset from tests/corpus/corpora.lock.json and the MSVC toolset from
 * sdk/sdk.lock.json, compiled in by CMake. A missing corpus, toolset or build fails the test.
 */

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/engine/asset_index.hpp"
#include "frost/engine/engine_cli.hpp"
#include "frost/engine/engine_lua.hpp"
#include "frost/engine/handle.hpp"
#include "frost/engine/schema.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/completion.hpp"
#include "frost/lang/lua_host.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/text/document.hpp"

#include "support/crash_dialogs.hpp"
#include "support/engine_lock.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef FROST_CORPUS_FROSTENGINE_SHA
#error "FROST_CORPUS_FROSTENGINE_SHA comes from tests/engine/CMakeLists.txt"
#endif

namespace frost::engine {
namespace {

using namespace std::chrono_literals;
using rpc::Json;

constexpr std::uint64_t k2To53 = 1ULL << 53;

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// The 1-based number of the first line after line `after` that contains `needle`; 0 when none does.
std::uint32_t lineOf(std::string_view text, std::string_view needle, std::uint32_t after = 0) {
    std::uint32_t number = 1;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        if (number > after && contains(text.substr(start, end - start), needle)) {
            return number;
        }
        start = end + 1;
        ++number;
    }
    return 0;
}

// Line `number` (1-based) of `text`, without its line break; "" past the end.
std::string lineText(std::string_view text, std::uint32_t number) {
    std::size_t start = 0;
    for (std::uint32_t n = 1; n < number && start != std::string_view::npos; ++n) {
        start = text.find('\n', start);
        start = start == std::string_view::npos ? start : start + 1;
    }
    if (start == std::string_view::npos || start >= text.size()) {
        return {};
    }
    std::string_view line = text.substr(start, text.find('\n', start) - start);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return std::string(line);
}

std::size_t occurrences(std::string_view text, std::string_view part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// $FROST_CORPORA_ROOT ("" when unset). Windows only, as this test is.
std::string corporaRoot() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "FROST_CORPORA_ROOT") != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
}

// <root>/frostengine-<sha12>, checked to be the pinned clone; "" (with a failure) otherwise, as
// tests/debug/corpus_debug_test.cpp does.
std::string checkedCorpus() {
    const std::string root = corporaRoot();
    if (root.empty()) {
        ADD_FAILURE() << "FROST_CORPORA_ROOT is not set; run the corpus tier through tools\\verify.ps1 -Corpus, "
                         "which sets it from tools/corpora.py root";
        return {};
    }
    const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
    std::string corpus = io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
    const auto head = io::realFileSystem().readFile(corpus + "/.git/HEAD");
    if (!head || head->substr(0, 40) != sha) {
        ADD_FAILURE() << corpus << " is not the pinned FrostEngine clone (python tools/corpora.py fetch, verify)";
        return {};
    }
    return corpus;
}

// The vcvarsall environment of the pinned MSVC toolset (as tests/debug/corpus_debug_test.cpp and
// tools/corpora.py's msvc_environment).
Result<project::CMakeEnvironment> pinnedMsvcEnvironment() {
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto instances = build::listVsInstances(launcher, fs);
    if (!instances) {
        return std::unexpected(std::move(instances.error()));
    }
    const std::string pinned = FROST_PINNED_VC_TOOLS;
    for (const build::VsInstance& instance : *instances) {
        if (!fs.exists(instance.installationPath + "\\VC\\Tools\\MSVC\\" + pinned + "\\include").value_or(false)) {
            continue;
        }
        auto captured = build::captureMsvcEnvironment(launcher, fs, instance, "x64");
        if (!captured) {
            return std::unexpected(std::move(captured.error()));
        }
        const auto version = std::ranges::find_if(
            captured->changes, [](const auto& entry) { return _stricmp(entry.first.c_str(), "VCToolsVersion") == 0; });
        if (version == captured->changes.end() || !version->second || !contains(*version->second, pinned)) {
            return fail(ErrorCode::NotFound,
                        std::format("{}'s default MSVC toolset is not the pinned {}", instance.displayName, pinned));
        }
        return std::move(captured->changes);
    }
    return fail(ErrorCode::NotFound, std::format("no Visual Studio instance has the pinned MSVC toolset {}", pinned));
}

class CorpusEngineContract : public testing::Test {
protected:
    // FrostEditor of the pinned clone, built under the corpus build lock; "" (with a failure) when it
    // cannot be built. The lock is held for the whole test: the clone's build tree is shared.
    std::string builtEditor() {
        frost::test::disableCrashDialogs(); // The compilers and FrostEditor inherit the error mode.
        m_corpus = checkedCorpus();
        if (m_corpus.empty()) {
            return {};
        }
        const std::string sha = FROST_CORPUS_FROSTENGINE_SHA;
        m_lock = std::make_unique<frost::test::EngineBuildLock>(corporaRoot() + "/frostengine-" + sha.substr(0, 12) +
                                                                ".build.lock");
        if (!m_lock->held()) {
            ADD_FAILURE() << "the corpus build lock stayed taken (tools/corpora.py BuildLock)";
            return {};
        }
        auto msvc = pinnedMsvcEnvironment();
        if (!msvc) {
            ADD_FAILURE() << msvc.error().describe();
            return {};
        }
        const std::string buildDir = m_corpus + "\\build\\" + FROST_CORPUS_FROSTENGINE_PRESET;
        io::ProcessSpec buildSpec;
        buildSpec.executable = io::normalizePath(FROST_TEST_CMAKE).value_or("");
        buildSpec.arguments = {"--build", buildDir, "--target", "FrostEditor"};
        buildSpec.workingDirectory = m_corpus;
        buildSpec.environment = *msvc;
        auto built = io::runToCompletion(io::realProcessLauncher(), buildSpec, {}, 3h);
        if (!built || built->exitCode != 0) {
            ADD_FAILURE() << "cmake --build " << buildDir << " --target FrostEditor failed (configure the clone with "
                          << "python tools/corpora.py engine --only frostengine --configure): "
                          << (built ? built->stdoutBytes.substr(
                                          built->stdoutBytes.size() > 8000 ? built->stdoutBytes.size() - 8000 : 0) +
                                          built->stderrBytes
                                    : built.error().describe());
            return {};
        }
        auto program = findEngineEditorProgram(io::realFileSystem(), m_corpus, FROST_CORPUS_FROSTENGINE_PRESET);
        if (!program) {
            ADD_FAILURE() << program.error().describe();
            return {};
        }
        return std::move(*program);
    }

    std::string m_corpus;
    std::unique_ptr<frost::test::EngineBuildLock> m_lock;
};

// The names of `list`'s items' member `key`, in order.
std::vector<std::string> namesOf(const Json* list, std::string_view key = "name") {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const Json& item : list->items()) {
            out.emplace_back(item.stringAt(key));
        }
    }
    return out;
}

TEST_F(CorpusEngineContract, SchemaExportValidatesAndEverySourceIsALineOfTheClone) {
    const std::string program = builtEditor();
    ASSERT_FALSE(program.empty());
    test::ScratchDir work("engine-schema-export");
    EngineCliRequest request;
    request.command = "schema.export";
    request.workingDirectory = work.path();
    auto exported = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(exported.has_value()) << exported.error().describe();
    auto document = parseSchemaDocument(*exported);
    ASSERT_TRUE(document.has_value()) << document.error().describe();

    // The components are the engine's component.types, in its order.
    request.command = "component.types";
    auto types = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(types.has_value()) << types.error().describe();
    std::vector<std::string> exportedComponents;
    for (const SchemaComponent& component : document->components) {
        exportedComponents.push_back(component.name);
    }
    EXPECT_EQ(exportedComponents, namesOf(types->find("components")));
    EXPECT_FALSE(exportedComponents.empty());

    // The commands are the engine's commands.list (which names schema.export itself).
    request.command = "commands.list";
    auto commands = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(commands.has_value()) << commands.error().describe();
    std::vector<std::string> exportedCommands;
    for (const SchemaCommand& command : document->commands) {
        exportedCommands.push_back(command.name);
    }
    std::vector<std::string> listed = namesOf(commands->find("commands"));
    std::ranges::sort(exportedCommands);
    std::ranges::sort(listed);
    EXPECT_EQ(exportedCommands, listed);
    EXPECT_TRUE(std::ranges::contains(listed, "schema.export"));

    // Every source is an existing line of the clone.
    SourceResolver resolver(io::realFileSystem(), m_corpus);
    std::size_t resolved = 0;
    std::vector<std::string> unresolved;
    const auto check = [&](const SchemaSource& source, std::string_view what) {
        auto line = resolver.resolve(source);
        if (line) {
            ++resolved;
        } else if (unresolved.size() < 20) {
            unresolved.push_back(std::format("{} ({}:{}): {}", what, source.file, source.line, line.error().message()));
        }
    };
    for (const SchemaComponent& component : document->components) {
        check(component.source, component.name);
        for (const SchemaField& field : component.fields) {
            check(field.source, component.name + "." + field.name);
        }
    }
    std::size_t withoutSource = 0;
    for (const SchemaLuaEntry& entry : document->luaEntries) {
        if (entry.source) {
            check(*entry.source, entry.qualifiedName);
        } else {
            ++withoutSource;
            // Only Lua's own libraries have no engine C++ behind them.
            EXPECT_EQ(entry.binding, "LuaLibrary") << entry.qualifiedName << " has no source";
        }
    }
    for (const SchemaEvent& event : document->events) {
        check(event.source, event.name);
    }
    EXPECT_TRUE(unresolved.empty()) << testing::PrintToString(unresolved);
    EXPECT_GT(resolved, document->luaEntries.size() - withoutSource);
    std::cout << "[ engine ] schema.export: " << document->components.size() << " components, "
              << document->luaEntries.size() << " Lua entries (" << withoutSource << " from Lua's own libraries), "
              << document->events.size() << " events, " << document->commands.size() << " commands; " << resolved
              << " sources resolved in " << m_corpus << "\n";

    // Physics.Raycast and RaycastAll point at their registrations, whose declared costs the source states.
    auto physicsPath = io::joinPath(m_corpus, "Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp");
    ASSERT_TRUE(physicsPath.has_value()) << physicsPath.error().describe();
    auto physics = io::realFileSystem().readFile(*physicsPath);
    ASSERT_TRUE(physics.has_value()) << physics.error().describe();
    const std::uint32_t raycastName = lineOf(*physics, "\"Raycast\"");
    const std::uint32_t raycastAllName = lineOf(*physics, "\"RaycastAll\"");
    ASSERT_GT(raycastName, 0U);
    ASSERT_GT(raycastAllName, raycastName);
    const SchemaLuaEntry* raycast = document->findLuaEntry("Physics.Raycast");
    const SchemaLuaEntry* raycastAll = document->findLuaEntry("Physics.RaycastAll");
    ASSERT_NE(raycast, nullptr);
    ASSERT_NE(raycastAll, nullptr);
    ASSERT_TRUE(raycast->source && raycastAll->source);
    EXPECT_EQ(raycast->source->file, "Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp");
    // The registration call (".Function(") starts on the name's line or up to two lines before it.
    EXPECT_LE(raycast->source->line, raycastName);
    EXPECT_GE(raycast->source->line + 2, raycastName);
    EXPECT_LE(raycastAll->source->line, raycastAllName);
    EXPECT_GE(raycastAll->source->line + 2, raycastAllName);
    const std::uint32_t raycastCost = lineOf(*physics, ".Cost = ScriptCost::PhysicsQuery", raycastName);
    ASSERT_GT(raycastCost, 0U);
    EXPECT_LT(raycastCost, raycastAllName) << "Raycast's own ScriptDoc declares its cost";
    EXPECT_EQ(raycast->cost, std::vector<std::string>{"PhysicsQuery"});
    const std::uint32_t raycastAllCost = lineOf(*physics, ".Cost = ", raycastAllName);
    ASSERT_GT(raycastAllCost, 0U);
    const std::string costLine = lineText(*physics, raycastAllCost);
    for (const std::string& tag : raycastAll->cost) {
        EXPECT_TRUE(contains(costLine, "ScriptCost::" + tag)) << tag << " is not declared on: " << costLine;
    }
    EXPECT_EQ(raycastAll->cost.size(), occurrences(costLine, "ScriptCost::")) << costLine;

    // The ComponentName alias offers every component proxy of the Lua API.
    const auto alias = std::ranges::find(document->luaAliases, std::string("ComponentName"), &SchemaLuaAlias::name);
    ASSERT_NE(alias, document->luaAliases.end());
    std::vector<std::string> proxies;
    for (const SchemaLuaEntry& entry : document->luaEntries) {
        if (entry.kind == "Component") {
            proxies.push_back(entry.name);
        }
    }
    std::vector<std::string> offered = alias->values;
    std::ranges::sort(proxies);
    std::ranges::sort(offered);
    EXPECT_EQ(offered, proxies);
}

// The ids of the entities of a scene file, read as digits from its text: `"ID": <digits>`.
std::vector<std::uint64_t> sceneFileIds(std::string_view text) {
    std::vector<std::uint64_t> ids;
    constexpr std::string_view kKey = "\"ID\": ";
    for (std::size_t at = text.find(kKey); at != std::string_view::npos; at = text.find(kKey, at + 1)) {
        const char* begin = text.data() + at + kKey.size();
        std::uint64_t id = 0;
        const auto [end, error] = std::from_chars(begin, text.data() + text.size(), id);
        if (error == std::errc() && end != begin) {
            ids.push_back(id);
        }
    }
    return ids;
}

// Every id of entity.list's tree.
void collectIds(const Json& entities, std::vector<std::uint64_t>& out, std::vector<std::string>& bad) {
    for (const Json& entity : entities.items()) {
        if (auto id = engineHandleAt(entity, "id")) {
            out.push_back(id->value);
        } else {
            bad.push_back(entity.find("id") != nullptr ? entity.find("id")->dump() : "missing");
        }
        if (const Json* children = entity.find("children"); children != nullptr && children->isArray()) {
            collectIds(*children, out, bad);
        }
    }
}

TEST_F(CorpusEngineContract, AssetIndexAndUsagesEqualActualEngineCliIncludingNewReference) {
    const auto program = builtEditor();
    ASSERT_FALSE(program.empty());
    test::ScratchDir work("engine-asset-index");
    std::error_code ec;
    std::filesystem::copy(std::filesystem::path(m_corpus) / "TestProject", std::filesystem::path(work.path()) / "Game",
                          std::filesystem::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto project = work.join("Game");
    auto& fs = io::realFileSystem();
    ProjectContentRequest load;
    load.program = program;
    load.project = project;
    auto content = loadProjectContent(fs, io::realProcessLauncher(), load, [] { return true; });
    ASSERT_TRUE(content) << content.error().describe();
    ASSERT_FALSE(content->assets.empty());
    const auto target = content->assets.front();
    ASSERT_TRUE(fs.writeFileAtomic(project + "/Assets/AssetUsageFixture.lua",
                                   "local asset = " + Json(target.path).dump() + "\nreturn asset\n"));
    EngineCliRequest request;
    request.project = project;
    request.command = "assets.refresh";
    ASSERT_TRUE(runEngineCliResult(io::realProcessLauncher(), program, request));
    content = loadProjectContent(fs, io::realProcessLauncher(), load, [] { return true; });
    ASSERT_TRUE(content);
    auto index = AssetIndex::create(*content);
    ASSERT_TRUE(index);
    request.command = "assets.list";
    auto listed = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(listed);
    ASSERT_NE(listed->find("assets"), nullptr);
    EXPECT_EQ(index->content().assets.size(), listed->find("assets")->size());
    for (const auto& row : listed->find("assets")->items()) {
        auto handle = engineHandleAt(row, "handle");
        ASSERT_TRUE(handle);
        const auto* actual = index->find(*handle);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->path, row.stringAt("path"));
        EXPECT_EQ(actual->type, row.stringAt("type"));
    }
    request.command = "assets.find-references";
    request.params = Json::object().set("asset", toJson(target.handle));
    auto references = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(references);
    auto usages = parseAssetUsages(*index, target.handle, *references);
    ASSERT_TRUE(usages) << usages.error().describe();
    ASSERT_NE(references->find("references"), nullptr);
    EXPECT_EQ(usages->references.size(), references->find("references")->size());
    EXPECT_TRUE(std::ranges::any_of(usages->references, [](const auto& row) {
        return row.path == "Assets/AssetUsageFixture.lua" && row.count > 0;
    }));
    std::uint64_t oracleCount{};
    for (const auto& row : references->find("references")->items()) {
        auto* count = row.find("count");
        ASSERT_NE(count, nullptr);
        ASSERT_TRUE(count->asUnsigned());
        oracleCount += *count->asUnsigned();
    }
    EXPECT_EQ(usages->occurrences, oracleCount);
    EXPECT_EQ(usages->codeVisionCount(), references->find("references")->size());
}

TEST_F(CorpusEngineContract, EntityIdsAbove2To53RoundTripExactlyThroughTheEngine) {
    const std::string program = builtEditor();
    ASSERT_FALSE(program.empty());
    // A scratch copy of the engine's TestProject: the engine may write caches into a project.
    test::ScratchDir work("engine-handles");
    std::error_code copyError;
    std::filesystem::copy(std::filesystem::path(m_corpus) / "TestProject",
                          std::filesystem::path(work.path()) / "TestProject", std::filesystem::copy_options::recursive,
                          copyError);
    ASSERT_FALSE(copyError) << copyError.message();
    const std::string project = work.join("TestProject");
    const std::string scene = "Scenes/LoadTarget.fscene";
    auto sceneText = io::realFileSystem().readFile(work.join("TestProject/Assets/Scenes/LoadTarget.fscene"));
    ASSERT_TRUE(sceneText.has_value()) << sceneText.error().describe();
    std::vector<std::uint64_t> fileIds = sceneFileIds(*sceneText);
    ASSERT_FALSE(fileIds.empty());
    std::vector<std::uint64_t> large;
    std::ranges::copy_if(fileIds, std::back_inserter(large), [](std::uint64_t id) { return id > k2To53; });
    ASSERT_FALSE(large.empty()) << "the scene file has no id above 2^53";
    // At least one is above INT64_MAX, which the IDE's JSON used to turn into a double.
    EXPECT_TRUE(
        std::ranges::any_of(large, [](std::uint64_t id) { return id > static_cast<std::uint64_t>(INT64_MAX); }));

    // entity.list returns exactly the file's ids.
    EngineCliRequest request;
    request.command = "entity.list";
    request.params = Json::object().set("scene", scene).set("components", false);
    request.project = project;
    request.workingDirectory = work.path();
    auto listed = runEngineCliResult(io::realProcessLauncher(), program, request);
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    std::vector<std::uint64_t> engineIds;
    std::vector<std::string> unreadable;
    ASSERT_NE(listed->find("entities"), nullptr);
    collectIds(*listed->find("entities"), engineIds, unreadable);
    EXPECT_TRUE(unreadable.empty()) << "ids that are no exact handle: " << testing::PrintToString(unreadable);
    std::ranges::sort(engineIds);
    std::ranges::sort(fileIds);
    EXPECT_EQ(engineIds, fileIds);

    // entity.get with each large id written back (a JSON integer, then the decimal-string form).
    const std::set<std::uint64_t> known(fileIds.begin(), fileIds.end());
    std::size_t neighboursChecked = 0;
    for (const std::uint64_t id : large) {
        SCOPED_TRACE(std::to_string(id));
        request.command = "entity.get";
        request.params = Json::object().set("scene", scene).set("entity", toJson(EngineHandle{id}));
        auto entity = runEngineCliResult(io::realProcessLauncher(), program, request);
        ASSERT_TRUE(entity.has_value()) << entity.error().describe();
        EXPECT_EQ(engineHandleAt(*entity, "id"), EngineHandle{id});
        EXPECT_TRUE(contains(entity->dump(), std::format("\"id\":{}", id))) << entity->dump().substr(0, 400);

        request.params = Json::object().set("scene", scene).set("entity", std::to_string(id));
        auto byText = runEngineCliResult(io::realProcessLauncher(), program, request);
        ASSERT_TRUE(byText.has_value()) << byText.error().describe();
        EXPECT_EQ(engineHandleAt(*byText, "id"), EngineHandle{id});

        // The neighbouring id is the same double; the engine, reading the exact integer, finds no entity.
        const std::uint64_t neighbour = id + 1;
        if (known.contains(neighbour) || static_cast<double>(neighbour) != static_cast<double>(id)) {
            continue; // Another entity, or a rounding boundary between the two doubles.
        }
        ++neighboursChecked;
        request.params = Json::object().set("scene", scene).set("entity", toJson(EngineHandle{neighbour}));
        auto missing = runEngineCliCommand(io::realProcessLauncher(), program, request);
        ASSERT_TRUE(missing.has_value()) << missing.error().describe();
        ASSERT_TRUE(missing->error.has_value()) << "the engine found " << neighbour << ": " << missing->result->dump();
        EXPECT_EQ(missing->error->code, -32001) << missing->error->message; // NotFound.
    }
    EXPECT_GT(neighboursChecked, 0U) << "no large id had a neighbour that is the same double";
}

TEST_F(CorpusEngineContract, LuaCompletionAndNavigationMatchSchemaExportOnPinnedClone) {
    const std::string program = builtEditor();
    ASSERT_FALSE(program.empty());
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();

    auto luaLs = lang::findLuaLanguageServer(fs, lang::defaultLuaSearch());
    ASSERT_TRUE(luaLs.has_value()) << "Pinned LuaLS is required: " << luaLs.error().describe();

    test::ScratchDir work("engine-lua-corpus");
    EngineCliRequest request;
    request.command = "schema.export";
    request.workingDirectory = work.path();
    auto exported = runEngineCliResult(launcher, program, request);
    ASSERT_TRUE(exported.has_value()) << exported.error().describe();
    auto schema = parseSchemaDocument(*exported);
    ASSERT_TRUE(schema.has_value()) << schema.error().describe();

    // 1. Supply `schema.export` meta stubs through LUA-13's `LuaHostRegistry` & `EngineLuaHostProvider`.
    lang::LuaHostRegistry hosts;
    hosts.add(std::make_shared<EngineLuaHostProvider>(*schema, m_corpus));
    const std::string workspaceDir = work.join("workspace");
    ASSERT_TRUE(fs.createDirectories(workspaceDir).has_value());
    const auto contributions = hosts.contributions(fs, workspaceDir);
    ASSERT_EQ(contributions.size(), 1U);
    ASSERT_EQ(contributions[0].metaFiles.size(), 1U);

    const std::string stateDir = work.join("state");
    auto metaDir = lang::luaHostMetaDirectory(stateDir, contributions[0].provider, workspaceDir);
    ASSERT_TRUE(metaDir.has_value()) << metaDir.error().describe();
    ASSERT_TRUE(lang::writeLuaMetaFiles(fs, *metaDir, contributions[0].metaFiles).has_value());
    const std::string stubPath = *io::joinPath(*metaDir, kEngineLuaStubFileName);
    ASSERT_TRUE(fs.exists(stubPath).value_or(false));

    const std::array<std::string, 1> metaLibs{*metaDir};
    lang::LuaServiceOptions luaOpts;
    luaOpts.serverPath = *luaLs;
    luaOpts.workspaceRoot = workspaceDir;
    auto args = lang::luaLanguageServerArguments(stateDir);
    ASSERT_TRUE(args.has_value());
    luaOpts.arguments = std::move(*args);
    luaOpts.trusted = true;
    luaOpts.configuration = lang::composeLuaConfiguration("Lua 5.4", contributions, metaLibs);

    auto reactor = io::Reactor::create();
    ASSERT_TRUE(reactor.has_value());
    sched::ThreadPool pool(sched::ThreadPoolOptions{.workerCount = 2});

    const std::string scriptPath = *io::joinPath(workspaceDir, "Probe.lua");
    const std::string scriptText = "---@class ProbeScript : FrostScript\n"
                                   "local ProbeScript = {}\n"
                                   "function ProbeScript:OnUpdate(dt)\n"
                                   "    local t = self.Entity:GetComponent(\"Transform\")\n"
                                   "    local p = t.\n"
                                   "    local rb = self.Entity:GetComponent(\"Rigidbody2D\")\n"
                                   "    local v = rb.\n"
                                   "    local hit = Physics.Raycast(t.Translation, t.Rotation)\n"
                                   "    local comp = self.Entity:GetComponent(\"\")\n"
                                   "end\n"
                                   "function ProbeScript:On\n";

    {
        lang::LuaService service(launcher, {**reactor, pool}, std::move(luaOpts));
        struct StopGuard {
            sched::ThreadPool& pool;
            lang::LuaService& service;
            ~StopGuard() { pool.spawn(sched::Lane::UserVisible, service.stop()).join(); }
        } stopGuard{pool, service};
        auto doc = text::Document::fromUtf8(scriptText);
        ASSERT_TRUE(doc.has_value());
        ASSERT_TRUE(service.openDocument(scriptPath, doc->text()).has_value());
        ASSERT_TRUE(service.start().has_value());

        const auto waitUntil = [](auto pred, std::chrono::seconds timeout) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (pred()) {
                    return true;
                }
                std::this_thread::sleep_for(50ms);
            }
            return false;
        };
        ASSERT_TRUE(waitUntil([&] { return service.state() == lang::LuaState::Running; }, 30s));

        const auto queryCompletionLabels = [&](ByteOffset caret) {
            std::vector<std::string> labels;
            waitUntil(
                [&] {
                    const lang::LspPosition pos = lang::toLspPosition(doc->text(), caret, service.encoding());
                    auto res =
                        pool.spawn(sched::Lane::UserVisible, lang::requestCompletion(service, scriptPath, pos, {}))
                            .join();
                    if (!res || res->items.empty()) {
                        return false;
                    }
                    labels.clear();
                    std::set<std::string, std::less<>> seen;
                    for (const lang::CompletionItem& item : res->items) {
                        std::string bare(item.raw.stringAt("label"));
                        if (bare.empty()) {
                            bare = item.label;
                        }
                        if (const auto paren = bare.find('('); paren != std::string::npos) {
                            bare.resize(paren);
                        }
                        if (!bare.empty() && seen.insert(bare).second) {
                            labels.push_back(std::move(bare));
                        }
                    }
                    return !labels.empty();
                },
                60s);
            return labels;
        };

        // 2. Component completion items equal the entries `schema.export` lists for that component.
        const auto transformDotOffset = ByteOffset{scriptText.find("local p = t.") + 12};
        std::vector<std::string> transformActual = queryCompletionLabels(transformDotOffset);
        std::vector<std::string> transformExpected = schemaComponentEntryNames(*schema, "Transform");
        std::ranges::sort(transformActual);
        std::ranges::sort(transformExpected);
        EXPECT_FALSE(transformExpected.empty());
        EXPECT_EQ(transformActual, transformExpected);

        // Also check a component that has both properties and methods in `schema.export` (`Rigidbody2D`).
        const auto rbDotOffset = ByteOffset{scriptText.find("local v = rb.") + 13};
        std::vector<std::string> rbActual = queryCompletionLabels(rbDotOffset);
        std::vector<std::string> rbExpected = schemaComponentEntryNames(*schema, "Rigidbody2D");
        std::ranges::sort(rbActual);
        std::ranges::sort(rbExpected);
        EXPECT_FALSE(rbExpected.empty());
        EXPECT_EQ(rbActual, rbExpected);

        // 3. Completion for component names (`GetComponent("")`) and event names (`ProbeScript:On`).
        const auto compQuoteOffset = ByteOffset{scriptText.find("GetComponent(\"\")") + 14};
        const std::vector<std::string> compCompletions = queryCompletionLabels(compQuoteOffset);
        EXPECT_TRUE(std::ranges::any_of(compCompletions, [](const std::string& s) {
            return s.find("Transform") != std::string::npos;
        })) << testing::PrintToString(compCompletions);

        const auto eventPrefixOffset = ByteOffset{scriptText.find("function ProbeScript:On") + 23};
        const std::vector<std::string> eventCompletions = queryCompletionLabels(eventPrefixOffset);
        EXPECT_TRUE(std::ranges::any_of(eventCompletions, [](const std::string& s) {
            return s.find("OnCreate") != std::string::npos;
        })) << testing::PrintToString(eventCompletions);
        EXPECT_TRUE(std::ranges::any_of(eventCompletions, [](const std::string& s) {
            return s.find("OnDestroy") != std::string::npos;
        })) << testing::PrintToString(eventCompletions);

        // 4. Go to Definition on `Physics.Raycast` opens the generated stub (`frost_schema.lua`).
        const auto raycastOffset = ByteOffset{scriptText.find("Physics.Raycast") + 10};
        const lang::LspPosition raycastPos = lang::toLspPosition(doc->text(), raycastOffset, service.encoding());
        auto nav =
            pool.spawn(sched::Lane::UserVisible,
                       lang::requestLocations(service, lang::NavigationTarget::Definition, scriptPath, raycastPos))
                .join();
        ASSERT_TRUE(nav.has_value()) << nav.error().describe();
        ASSERT_FALSE(nav->empty());
        EXPECT_EQ(lang::documentKey((*nav)[0].path), lang::documentKey(stubPath));

        // 5. Go to C++ Binding opens the exact C++ `file:line` from `source_location` in the clone.
        auto raycastCpp = resolveLuaCppBindingSource(fs, m_corpus, *schema, scriptText, raycastOffset);
        ASSERT_TRUE(raycastCpp.has_value()) << raycastCpp.error().describe();
        const SchemaLuaEntry* raycastSchema = schema->findLuaEntry("Physics.Raycast");
        ASSERT_NE(raycastSchema, nullptr);
        ASSERT_TRUE(raycastSchema->source.has_value());
        EXPECT_EQ(raycastCpp->line, raycastSchema->source->line);
        EXPECT_NE(raycastCpp->path.find("PhysicsScriptModule.cpp"), std::string::npos);

        const auto translationOffset = ByteOffset{scriptText.find("t.Translation") + 4};
        auto translationCpp = resolveLuaCppBindingSource(fs, m_corpus, *schema, scriptText, translationOffset);
        ASSERT_TRUE(translationCpp.has_value()) << translationCpp.error().describe();
        const SchemaComponent* transformSchema = schema->findComponent("Transform");
        ASSERT_NE(transformSchema, nullptr);
        ASSERT_FALSE(transformSchema->fields.empty());
        EXPECT_EQ(translationCpp->line, transformSchema->fields[0].source.line);

        const auto onUpdateOffset = ByteOffset{scriptText.find("OnUpdate") + 3};
        auto onUpdateCpp = resolveLuaCppBindingSource(fs, m_corpus, *schema, scriptText, onUpdateOffset);
        ASSERT_TRUE(onUpdateCpp.has_value()) << onUpdateCpp.error().describe();
        EXPECT_GT(onUpdateCpp->line, 0U);
    }
    (*reactor)->shutdown();
    pool.shutdown();
}

} // namespace
} // namespace frost::engine
