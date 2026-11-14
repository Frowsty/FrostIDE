// SPDX-License-Identifier: Apache-2.0
// FEC-13 graph/boundary unit cases; semantic tool and real-server acceptance are separate gates.
#include "frost/engine/binding_index.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

namespace frost::engine {
namespace {
using rpc::Json;
#ifdef _WIN32
const std::string root = "C:/binding-engine";
#else
const std::string root = "/binding-engine";
#endif

std::string path(std::string_view file) { return io::joinPath(root, file).value(); }

SchemaLuaEntry entry(std::string qualified, std::uint32_t line, std::string alias = {}) {
    SchemaLuaEntry result;
    result.qualifiedName = qualified;
    result.name = qualified.substr(qualified.find_last_of(".:") + 1);
    result.kind = "Function";
    result.source = SchemaSource{"Bindings.cpp", line};
    result.sameAs = std::move(alias);
    return result;
}

NativeBindingMapping mapping(const SchemaLuaEntry& lua, std::string usr, std::string role = "callable") {
    return {lua.qualifiedName,
            *lua.source,
            std::move(usr),
            "Engine::Native",
            {path("Native.hpp"), {ByteOffset{10}, ByteOffset{16}}},
            BindingSourceRange{path("Native.cpp"), {ByteOffset{40}, ByteOffset{46}}},
            {path("Bindings.cpp"), {ByteOffset{100}, ByteOffset{150}}},
            std::move(role),
            "function",
            {path("Bindings.cpp"), {ByteOffset{125}, ByteOffset{140}}}};
}

TEST(BindingIndex, ExplicitAliasSharesUsagesIdentityButSameSpellingCreatesNoEdge) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Physics.Raycast", 3), entry("Physics.Cast", 5, "Physics.Raycast"),
                         entry("Other.Raycast", 8)};
    const std::vector mappings{mapping(schema.luaEntries[0], "c:@N@Engine@F@Raycast#")};
    auto index = BindingIndex::create(root, 7, schema, mappings);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->revision(), 7U);
    const auto bindings = index->forNative(mappings.front().usr);
    ASSERT_EQ(bindings.size(), 2U);
    EXPECT_EQ(bindings[0]->lua.qualifiedName, "Physics.Raycast");
    EXPECT_EQ(bindings[1]->lua.qualifiedName, "Physics.Cast");
    EXPECT_EQ(index->findLua("Other.Raycast")->resolution, BindingResolution::Unresolved);
    auto targets = index->nativeTargets("Physics.Cast");
    ASSERT_TRUE(targets);
    EXPECT_EQ(targets->front(), mappings.front().declaration);
    EXPECT_EQ(targets->back(), mappings.front().registrationRange);
}

TEST(BindingIndex, ConflictingUsrsAndDuplicateSchemaIdentitiesNeverPickFirst) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Physics.Cast", 3)};
    std::vector mappings{mapping(schema.luaEntries[0], "usr-one"), mapping(schema.luaEntries[0], "usr-two")};
    auto index = BindingIndex::create(root, 1, schema, mappings);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("Physics.Cast")->resolution, BindingResolution::Ambiguous);
    EXPECT_TRUE(index->forNative("usr-one").empty());
    EXPECT_FALSE(index->nativeTargets("Physics.Cast"));
    schema.luaEntries.push_back(schema.luaEntries.front());
    index = BindingIndex::create(root, 2, schema, mappings);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("Physics.Cast"), nullptr);
    EXPECT_TRUE(index->forNative("usr-one").empty());
}

TEST(BindingIndex, DistinctPropertyAccessorsAreProvenTargetsWithDistinctRoles) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Entity.Name", 3)};
    const std::vector mappings{mapping(schema.luaEntries[0], "get-name", "getter"),
                               mapping(schema.luaEntries[0], "set-name", "setter")};
    auto index = BindingIndex::create(root, 1, schema, mappings);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("Entity.Name")->resolution, BindingResolution::Resolved);
    EXPECT_EQ(index->forNative("get-name").size(), 1U);
    EXPECT_EQ(index->forNative("set-name").size(), 1U);
}

TEST(BindingIndex, AliasCycleWithoutSemanticTargetStaysUnresolvedAndConflictPropagates) {
    SchemaDocument schema;
    schema.luaEntries = {entry("A", 1, "B"), entry("B", 2, "A")};
    auto index = BindingIndex::create(root, 1, schema, {});
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("A")->resolution, BindingResolution::Unresolved);
    const std::vector mappings{mapping(schema.luaEntries[0], "first"), mapping(schema.luaEntries[1], "second")};
    index = BindingIndex::create(root, 2, schema, mappings);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("A")->resolution, BindingResolution::Ambiguous);
    EXPECT_EQ(index->findLua("B")->resolution, BindingResolution::Ambiguous);
}

TEST(BindingIndex, ForeignRegistrationAndArgumentOutsideCallAreRefused) {
    SchemaDocument schema;
    schema.luaEntries = {entry("A", 1)};
    std::vector mappings{mapping(schema.luaEntries[0], "native")};
    mappings.front().registration.line = 2;
    EXPECT_FALSE(BindingIndex::create(root, 1, schema, mappings));
    mappings.front() = mapping(schema.luaEntries[0], "native");
    mappings.front().argumentRange.bytes.end = ByteOffset{200};
    EXPECT_FALSE(BindingIndex::create(root, 1, schema, mappings));
}

Json range(std::string file, std::uint64_t offset, std::uint64_t length) {
    return Json::object().set("path", path(file)).set("offset", offset).set("length", length);
}

Json mappedResult(const Json& request) {
    const auto& registration = request.find("registrations")->items().front();
    auto result = Json::object()
                      .set("id", registration.stringAt("id"))
                      .set("qualifiedName", registration.stringAt("qualifiedName"))
                      .set("source", *registration.find("source"))
                      .set("status", "mapped")
                      .set("registrationRange", range("Bindings.cpp", 100, 50))
                      .set("targets", Json::array().push(Json::object()
                                                             .set("role", "callable")
                                                             .set("usr", "actual-usr")
                                                             .set("kind", "function")
                                                             .set("qualifiedName", "Engine::Cast")
                                                             .set("declaration", range("Native.hpp", 10, 4))
                                                             .set("argumentRange", range("Bindings.cpp", 120, 8))));
    return Json::object()
        .set("schemaVersion", 1)
        .set("translationUnitParsed", true)
        .set("results", Json::array().push(std::move(result)));
}

TEST(BindingIndexBoundary, ExactToolResponsePreservesUsrAndByteRanges) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Physics.Cast", 7)};
    auto request = bindingMapRequest(schema, root, root, path("Bindings.cpp"));
    ASSERT_TRUE(request);
    const auto response = mappedResult(*request);
    auto report = parseBindingMapReport(*request, response);
    ASSERT_TRUE(report);
    ASSERT_EQ(report->mappings.size(), 1U);
    EXPECT_EQ(report->mappings.front().usr, "actual-usr");
    EXPECT_EQ(report->mappings.front().argumentRange.bytes, (text::ByteRange{ByteOffset{120}, ByteOffset{128}}));
    EXPECT_TRUE(BindingIndex::create(root, 1, schema, report->mappings, report->issues));
}

TEST(BindingIndexBoundary, MissingForeignDuplicateOrUnparsedMappedResultsFailClosed) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Physics.Cast", 7)};
    auto request = bindingMapRequest(schema, root, root, path("Bindings.cpp")).value();
    auto response = mappedResult(request);
    response.set("results", Json::array());
    EXPECT_FALSE(parseBindingMapReport(request, response));
    response = mappedResult(request);
    auto foreign = response.find("results")->items().front();
    foreign.set("id", "foreign");
    response.set("results", Json::array().push(foreign));
    EXPECT_FALSE(parseBindingMapReport(request, response));
    response = mappedResult(request);
    const auto original = response.find("results")->items().front();
    response.set("results", Json::array().push(original).push(original));
    EXPECT_FALSE(parseBindingMapReport(request, response));
    response = mappedResult(request);
    response.set("translationUnitParsed", false);
    EXPECT_FALSE(parseBindingMapReport(request, response));
}

TEST(BindingIndexBoundary, UnavailableAndAmbiguousToolAnswersNeverBecomeEdges) {
    SchemaDocument schema;
    schema.luaEntries = {entry("Physics.Cast", 7)};
    const auto request = bindingMapRequest(schema, root, root, path("Bindings.cpp")).value();
    auto response = mappedResult(request);
    auto unavailable = response.find("results")->items().front();
    unavailable.set("status", "ambiguous").set("reason", "Two actual registration calls");
    response.set("results", Json::array().push(unavailable));
    auto report = parseBindingMapReport(request, response);
    ASSERT_TRUE(report);
    EXPECT_TRUE(report->mappings.empty());
    ASSERT_EQ(report->issues.size(), 1U);
    auto index = BindingIndex::create(root, 1, schema, report->mappings, report->issues);
    ASSERT_TRUE(index);
    EXPECT_EQ(index->findLua("Physics.Cast")->resolution, BindingResolution::Ambiguous);
}
} // namespace
} // namespace frost::engine
