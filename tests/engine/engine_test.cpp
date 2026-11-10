// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::engine (FEC-1, FER-1a; label "unit"): exact handles in every form the
// engine prints or accepts, the schema.export reader's checks (each fault named by its JSON path),
// source resolution against a scratch checkout, and the engine-program lookup of an unbuilt tree.
// The documents here are inputs to the validator, written to hit one rule each; what the real
// engine prints is checked by engine_corpus_tests against the pinned FrostEngine.

#include "frost/engine/engine_cli.hpp"
#include "frost/engine/engine_lua.hpp"
#include "frost/engine/handle.hpp"
#include "frost/engine/schema.hpp"
#include "frost/io/file_system.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>

namespace frost::engine {
namespace {

using rpc::Json;
using rpc::parseJson;

Json parsed(std::string_view text) {
    auto json = parseJson(text);
    EXPECT_TRUE(json.has_value()) << text;
    return json.value_or(Json());
}

TEST(EngineHandle, ReadsEveryFormExactly) {
    // 2^53 + 1 and a handle above INT64_MAX: both lost their low bits as doubles before FEC-1.
    EXPECT_EQ(engineHandleFrom(parsed("9007199254740993")), EngineHandle{9007199254740993ULL});
    EXPECT_EQ(engineHandleFrom(parsed("16273909378015378282")), EngineHandle{16273909378015378282ULL});
    EXPECT_EQ(engineHandleFrom(parsed("18446744073709551615")), EngineHandle{18446744073709551615ULL});
    EXPECT_EQ(engineHandleFrom(Json("16273909378015378282")), EngineHandle{16273909378015378282ULL});
    EXPECT_EQ(engineHandleFrom(Json("0xE1D6A7F1C3B2A96A")), EngineHandle{0xE1D6A7F1C3B2A96AULL});
    EXPECT_EQ(engineHandleFrom(parsed("0")), EngineHandle{0});
    EXPECT_EQ(engineHandleAt(parsed(R"({"id":9787777966128738328})"), "id"), EngineHandle{9787777966128738328ULL});
    EXPECT_EQ(engineHandleAt(parsed(R"({"id":1})"), "other"), std::nullopt);
}

TEST(EngineHandle, RefusesWhatIsNotAnExactHandle) {
    EXPECT_EQ(engineHandleFrom(parsed("-1")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(parsed("1.5")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(parsed("18446744073709551616")), std::nullopt); // Beyond 2^64 - 1: a double.
    EXPECT_EQ(engineHandleFrom(Json(1e19)), std::nullopt);                     // Doubles never carry handles.
    EXPECT_EQ(engineHandleFrom(Json("18446744073709551616")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(Json("-5")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(Json("12a")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(Json("")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(Json("0x")), std::nullopt);
    EXPECT_EQ(engineHandleFrom(Json(true)), std::nullopt);
}

TEST(EngineHandle, WritesTheIntegerTheEngineReadsBack) {
    const EngineHandle handle{16273909378015378282ULL};
    EXPECT_EQ(toJson(handle).dump(), "16273909378015378282");
    const Json params = Json::object().set("entity", toJson(handle));
    EXPECT_EQ(params.dump(), R"({"entity":16273909378015378282})");
    EXPECT_EQ(engineHandleAt(parsed(params.dump()), "entity"), handle);
}

// The smallest document schema version 1 accepts, which each test below breaks in one place.
Json minimalDocument() {
    return parsed(R"json({
        "schema": "frostengine.schema-export", "schemaVersion": 1, "engine": {"version": "0.1.0"},
        "costTags": ["Allocates", "PhysicsQuery"], "bindingKinds": ["Wrapped", "Raw", "LuaLibrary"],
        "components": [{"name": "Transform", "source": {"file": "Frost/src/Frost/Scene/Components.cpp", "line": 12},
                        "fields": [{"name": "Translation", "type": "Vector3",
                                    "source": {"file": "Frost/src/Frost/Scene/Components.cpp", "line": 13}}]}],
        "lua": {"version": "Lua 5.4", "scriptsRoot": "Scripts", "modulePathPattern": "Scripts/?.lua",
                "entries": [
                    {"kind": "Function", "scope": "Static", "owner": "Physics", "name": "Raycast", "fullName": "Physics.Raycast",
                     "qualifiedName": "Physics.Raycast", "signature": "Physics.Raycast(origin, direction)", "description": "",
                     "parameters": [], "returns": {"type": "Hit", "description": ""}, "module": "Physics", "readOnly": false,
                     "testModeOnly": false, "binding": "Wrapped", "cost": ["PhysicsQuery"],
                     "source": {"file": "Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", "line": 40}},
                    {"kind": "Function", "scope": "Static", "owner": "string", "name": "format", "fullName": "string.format",
                     "qualifiedName": "string.format", "signature": "", "description": "", "parameters": [],
                     "returns": {"type": "", "description": ""}, "module": "Lua", "readOnly": false, "testModeOnly": false,
                     "binding": "LuaLibrary", "cost": [], "source": null},
                    {"kind": "Function", "scope": "Static", "owner": "Physics", "name": "Cast", "fullName": "Physics.Cast",
                     "qualifiedName": "Physics.Cast", "signature": "", "description": "", "parameters": [],
                     "returns": {"type": "", "description": ""}, "module": "Physics", "readOnly": false, "testModeOnly": true,
                     "binding": "Wrapped", "cost": ["PhysicsQuery"], "sameAs": "Physics.Raycast",
                     "source": {"file": "Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", "line": 44}}],
                "aliases": [{"name": "ComponentName", "description": "", "values": [{"value": "Transform", "description": ""}]}]},
        "events": [{"name": "OnUpdate", "signature": "OnUpdate(dt)", "description": "", "parameters": [], "module": "Core",
                    "source": {"file": "Frost/src/Frost/Scripting/ScriptCallbacks.cpp", "line": 7}}],
        "commands": [{"name": "schema.export", "category": "Tools", "description": "", "flags": ["Headless"]}]
    })json");
}

TEST(EngineSchema, ReadsAValidDocument) {
    auto document = parseSchemaDocument(minimalDocument());
    ASSERT_TRUE(document.has_value()) << document.error().message();
    EXPECT_EQ(document->engineVersion, "0.1.0");
    ASSERT_EQ(document->components.size(), 1U);
    EXPECT_EQ(document->components[0].fields[0].type, "Vector3");
    EXPECT_EQ(document->components[0].fields[0].source, (SchemaSource{"Frost/src/Frost/Scene/Components.cpp", 13}));
    ASSERT_EQ(document->luaEntries.size(), 3U);
    const SchemaLuaEntry* raycast = document->findLuaEntry("Physics.Raycast");
    ASSERT_NE(raycast, nullptr);
    EXPECT_EQ(raycast->cost, std::vector<std::string>{"PhysicsQuery"});
    EXPECT_EQ(raycast->binding, "Wrapped");
    EXPECT_FALSE(document->findLuaEntry("string.format")->source.has_value());
    EXPECT_EQ(document->findLuaEntry("Physics.Cast")->sameAs, "Physics.Raycast");
    EXPECT_TRUE(document->findLuaEntry("Physics.Cast")->testModeOnly);
    EXPECT_EQ(document->luaAliases[0].values, std::vector<std::string>{"Transform"});
    EXPECT_EQ(document->events[0].source.line, 7U);
    EXPECT_EQ(document->commands[0].flags, std::vector<std::string>{"Headless"});
    EXPECT_NE(document->findComponent("Transform"), nullptr);
    EXPECT_EQ(document->findComponent("Missing"), nullptr);
}

// Applies `edit` to the minimal document and expects `code` with `fragment` in the message.
void expectRefused(const std::function<void(Json&)>& edit, ErrorCode code, std::string_view fragment) {
    Json document = minimalDocument();
    edit(document);
    auto read = parseSchemaDocument(document);
    ASSERT_FALSE(read.has_value()) << "accepted: " << document.dump();
    EXPECT_EQ(read.error().code(), code) << read.error().message();
    EXPECT_NE(read.error().message().find(fragment), std::string::npos) << read.error().message();
}

Json& luaEntry(Json& document, std::size_t index) {
    return const_cast<Json&>(document.find("lua")->find("entries")->items()[index]);
}

TEST(EngineSchema, RefusesAnotherSchemaOrVersion) {
    expectRefused([](Json& d) { d.set("schema", "something-else"); }, ErrorCode::Unsupported, "something-else");
    expectRefused([](Json& d) { d.set("schemaVersion", 2); }, ErrorCode::Unsupported, "version 2");
    expectRefused([](Json& d) { d.set("schemaVersion", "1"); }, ErrorCode::Unsupported, "version");
    EXPECT_EQ(parseSchemaDocument(Json::array()).error().code(), ErrorCode::Corrupt);
}

TEST(EngineSchema, NamesTheJsonPathOfTheFirstFault) {
    expectRefused([](Json& d) { d.set("components", Json::object()); }, ErrorCode::Corrupt,
                  "components: is an object, expected an array");
    expectRefused([](Json& d) { luaEntry(d, 0).set("cost", Json::array({Json("Teleports")})); }, ErrorCode::Corrupt,
                  "lua.entries[0].cost[0]: \"Teleports\"");
    expectRefused([](Json& d) { luaEntry(d, 0).set("binding", "Magic"); }, ErrorCode::Corrupt,
                  "lua.entries[0].binding: \"Magic\"");
    expectRefused([](Json& d) { luaEntry(d, 2).set("sameAs", "Physics.Nothing"); }, ErrorCode::Corrupt,
                  "lua.entries[2].sameAs: \"Physics.Nothing\" names no entry");
    expectRefused([](Json& d) { luaEntry(d, 0).set("qualifiedName", ""); }, ErrorCode::Corrupt,
                  "lua.entries[0].qualifiedName: is empty");
    expectRefused([](Json& d) { luaEntry(d, 0).set("testModeOnly", 1); }, ErrorCode::Corrupt,
                  "lua.entries[0].testModeOnly: is an integer, expected a boolean");
    expectRefused(
        [](Json& d) { d.set("events", Json::array({Json::object().set("name", "OnUpdate").set("signature", "")})); },
        ErrorCode::Corrupt, "events[0].source: missing");
}

TEST(EngineSchema, RefusesSourcesThatAreNotLinesOfTheCheckout) {
    const auto withSource = [](Json source) { return [source](Json& d) { luaEntry(d, 0).set("source", source); }; };
    const auto at = [](std::string_view file, std::int64_t line) {
        return Json::object().set("file", std::string(file)).set("line", line);
    };
    expectRefused(withSource(at("Frost/a.cpp", 0)), ErrorCode::Corrupt, "lua.entries[0].source.line: 0");
    expectRefused(withSource(at("Frost/a.cpp", -3)), ErrorCode::Corrupt, "is not a line number");
    expectRefused(withSource(at("Frost\\a.cpp", 3)), ErrorCode::Corrupt, "not a repository-relative path");
    expectRefused(withSource(at("/abs/a.cpp", 3)), ErrorCode::Corrupt, "not a repository-relative path");
    expectRefused(withSource(at("C:/abs/a.cpp", 3)), ErrorCode::Corrupt, "not a repository-relative path");
    expectRefused(withSource(at("Frost/../../a.cpp", 3)), ErrorCode::Corrupt, "\"..\" component");
    expectRefused(withSource(at("Frost//a.cpp", 3)), ErrorCode::Corrupt, "empty");
    expectRefused(withSource(at("", 3)), ErrorCode::Corrupt, "source.file: is empty");
    expectRefused(withSource(Json("Frost/a.cpp:3")), ErrorCode::Corrupt, "expected an object or null");
    // Only Lua's own library entries may lack a source; a component may not.
    expectRefused([](Json& d) { const_cast<Json&>(d.find("components")->items()[0]).set("source", Json()); },
                  ErrorCode::Corrupt, "components[0].source: is null");
}

TEST(EngineSchema, ResolvesSourcesToLinesOfTheCheckout) {
    test::ScratchDir root("engine-schema-sources");
    auto& fs = io::realFileSystem();
    ASSERT_TRUE(fs.createDirectories(root.join("Frost/src")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(root.join("Frost/src/A.cpp"), "first\r\nsecond line\nthird").has_value());

    auto resolved = resolveSchemaSource(fs, root.path(), {"Frost/src/A.cpp", 2});
    ASSERT_TRUE(resolved.has_value()) << resolved.error().message();
    EXPECT_EQ(resolved->path, root.join("Frost/src/A.cpp"));
    EXPECT_EQ(resolved->text, "second line");

    SourceResolver resolver(fs, root.path());
    EXPECT_EQ(resolver.resolve({"Frost/src/A.cpp", 3})->text, "third");
    EXPECT_EQ(resolver.resolve({"Frost/src/A.cpp", 4}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(resolver.resolve({"Frost/src/Missing.cpp", 1}).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(resolver.resolve({"../outside.cpp", 1}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(resolveSchemaSource(fs, "relative/root", {"Frost/src/A.cpp", 1}).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(EngineCli, AnUnbuiltTreeHasNoProgramAndSaysWhere) {
    test::ScratchDir root("engine-cli-unbuilt");
    auto& fs = io::realFileSystem();
    auto program = findEngineEditorProgram(fs, root.path(), "windows-debug");
    ASSERT_FALSE(program.has_value());
    EXPECT_EQ(program.error().code(), ErrorCode::NotFound);
    EXPECT_NE(program.error().message().find("windows-debug"), std::string::npos) << program.error().message();
    EXPECT_EQ(findEngineEditorProgram(fs, "relative", "windows-debug").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(findEngineEditorProgram(fs, root.path(), "").error().code(), ErrorCode::InvalidArgument);
    EngineCliRequest request;
    request.command = "";
    EXPECT_EQ(runEngineCliCommand(io::realProcessLauncher(), root.join("FrostEditor"), request).error().code(),
              ErrorCode::InvalidArgument);
}

TEST(EngineLua, GeneratesMetaStubsGlobalsAndComponentEntryNamesFromSchema) {
    auto doc = parseSchemaDocument(minimalDocument());
    ASSERT_TRUE(doc.has_value()) << doc.error().message();

    const std::string stubs = generateEngineLuaStubs(*doc);
    EXPECT_NE(stubs.find("---@meta frost_schema"), std::string::npos);
    EXPECT_NE(stubs.find("---@alias ComponentName"), std::string::npos);
    EXPECT_NE(stubs.find("---| '\"Transform\"'"), std::string::npos);
    EXPECT_NE(stubs.find("---@class Transform"), std::string::npos);
    EXPECT_NE(stubs.find("---@field Translation Vec3"), std::string::npos);
    EXPECT_NE(stubs.find("---@overload fun(self: Entity, name: \"Transform\"): Transform"), std::string::npos);
    EXPECT_NE(stubs.find("function Physics.Raycast("), std::string::npos);
    EXPECT_NE(stubs.find("--- Cost: PhysicsQuery"), std::string::npos);
    EXPECT_NE(stubs.find("--- Source: Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp:40"),
              std::string::npos);
    EXPECT_NE(stubs.find("---@class FrostScript"), std::string::npos);
    EXPECT_NE(stubs.find("function FrostScript:OnUpdate("), std::string::npos);
    EXPECT_NE(stubs.find("function OnUpdate("), std::string::npos);
    // Lua's own standard library entries (`binding == "LuaLibrary"`) are not re-emitted as engine globals.
    EXPECT_EQ(stubs.find("function string.format"), std::string::npos);

    EXPECT_EQ(schemaComponentEntryNames(*doc, "Transform"), std::vector<std::string>{"Translation"});
    const std::vector<std::string> globals = schemaLuaGlobals(*doc);
    EXPECT_TRUE(std::ranges::contains(globals, "Physics"));
    EXPECT_TRUE(std::ranges::contains(globals, "FrostScript"));
    EXPECT_TRUE(std::ranges::contains(globals, "OnUpdate"));
    EXPECT_FALSE(std::ranges::contains(globals, "string"));
}

TEST(EngineLua, ResolvesLuaSymbolsToSchemaCppBindingSources) {
    auto doc = parseSchemaDocument(minimalDocument());
    ASSERT_TRUE(doc.has_value()) << doc.error().message();

    const std::string script = "local t = entity:GetComponent(\"Transform\")\n"
                               "local pos = t.Translation\n"
                               "local hit = Physics.Raycast(pos, pos)\n"
                               "local cast = Physics.Cast(pos, pos)\n"
                               "function Player:OnUpdate(dt)\n"
                               "    local direct = Transform.Translation\n"
                               "end\n";

    // Component name literal inside `GetComponent("Transform")`.
    const auto compLiteralPos = script.find("\"Transform\"") + 3;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{compLiteralPos}),
              (SchemaSource{"Frost/src/Frost/Scene/Components.cpp", 12}));

    // Component field via inferred variable `t.Translation`.
    const auto inferredFieldPos = script.find("t.Translation") + 4;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{inferredFieldPos}),
              (SchemaSource{"Frost/src/Frost/Scene/Components.cpp", 13}));

    // Direct `Transform.Translation` with caret on `Translation`.
    const auto directFieldPos = script.find("Transform.Translation") + 12;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{directFieldPos}),
              (SchemaSource{"Frost/src/Frost/Scene/Components.cpp", 13}));

    // `Physics.Raycast` with caret on `Raycast` and with caret on `Physics`.
    const auto raycastPos = script.find("Physics.Raycast") + 10;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{raycastPos}),
              (SchemaSource{"Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", 40}));
    const auto physicsPrefixPos = script.find("Physics.Raycast") + 2;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{physicsPrefixPos}),
              (SchemaSource{"Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", 40}));

    // `Physics.Cast` (`sameAs: Physics.Raycast`, with its own registration line 44).
    const auto castPos = script.find("Physics.Cast") + 9;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{castPos}),
              (SchemaSource{"Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", 44}));

    // Event callback `Player:OnUpdate`.
    const auto eventPos = script.find("OnUpdate") + 3;
    EXPECT_EQ(resolveLuaCppBinding(*doc, script, ByteOffset{eventPos}),
              (SchemaSource{"Frost/src/Frost/Scripting/ScriptCallbacks.cpp", 7}));

    // Caret inside the generated meta stub itself (`frost_schema.lua`) resolves via `Source: <file>:<line>`.
    const std::string stubs = generateEngineLuaStubs(*doc);
    const auto stubFnPos = stubs.find("function Physics.Raycast") + 10;
    EXPECT_EQ(resolveLuaCppBinding(*doc, stubs, ByteOffset{stubFnPos}),
              (SchemaSource{"Frost/src/Frost/Scripting/Modules/PhysicsScriptModule.cpp", 40}));
}

TEST(EngineLua, HostProviderContributesSchemaMetaFilesThroughLuaHostApi) {
    auto doc = parseSchemaDocument(minimalDocument());
    ASSERT_TRUE(doc.has_value());
    test::ScratchDir dir("engine-lua-host");
    auto& fs = io::realFileSystem();

    lang::LuaHostRegistry hosts;
    hosts.add(std::make_shared<EngineLuaHostProvider>(*doc, dir.path()));
    const auto contributions = hosts.contributions(fs, dir.path());
    ASSERT_EQ(contributions.size(), 1U);
    EXPECT_EQ(contributions[0].provider, "FrostEngine");
    EXPECT_EQ(contributions[0].runtimeVersion, "Lua 5.4");
    ASSERT_EQ(contributions[0].metaFiles.size(), 1U);
    EXPECT_EQ(contributions[0].metaFiles[0].name, kEngineLuaStubFileName);

    const auto metaDir = lang::luaHostMetaDirectory(dir.join("state"), contributions[0].provider, dir.path());
    ASSERT_TRUE(metaDir.has_value());
    ASSERT_TRUE(lang::writeLuaMetaFiles(fs, *metaDir, contributions[0].metaFiles).has_value());
    EXPECT_TRUE(fs.exists(*metaDir + "/" + std::string(kEngineLuaStubFileName)).value_or(false));
}

} // namespace
} // namespace frost::engine
