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
#include "frost/engine/engine_cli.hpp"
#include "frost/engine/handle.hpp"
#include "frost/engine/schema.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

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
    const std::string corpus = io::normalizePath(root + "/frostengine-" + sha.substr(0, 12)).value_or("");
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
    auto physics =
        io::realFileSystem().readFile(m_corpus + "\\Frost\\src\\Frost\\Scripting\\Modules\\PhysicsScriptModule.cpp");
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

} // namespace
} // namespace frost::engine
