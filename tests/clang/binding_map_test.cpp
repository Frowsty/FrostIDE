// SPDX-License-Identifier: Apache-2.0
// FEC-13 real LibTooling acceptance: exact AST identities and read-only workspace analysis.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/rpc/json.hpp"

#include "binding_map_oracle.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using frost::rpc::Json;
using namespace std::chrono_literals;

struct CapturedFile {
    std::string bytes;
    std::filesystem::file_time_type modified;
    friend bool operator==(const CapturedFile&, const CapturedFile&) = default;
};

struct WorkspaceSnapshot {
    std::map<std::string, CapturedFile> files;
    std::vector<std::string> directories;
    friend bool operator==(const WorkspaceSnapshot&, const WorkspaceSnapshot&) = default;
};

std::optional<WorkspaceSnapshot> snapshot(std::string_view root) {
    const auto path = frost::io::toStdPath(root);
    if (!path) {
        ADD_FAILURE() << path.error().describe();
        return std::nullopt;
    }
    WorkspaceSnapshot result;
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(*path, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        auto name = frost::io::fromStdPath(it->path());
        if (!name) {
            ADD_FAILURE() << name.error().describe();
            return std::nullopt;
        }
        if (it->is_directory(error)) {
            result.directories.push_back(*name);
        } else if (!error && it->is_regular_file(error)) {
            auto bytes = frost::io::realFileSystem().readFile(*name);
            if (!bytes) {
                ADD_FAILURE() << bytes.error().describe();
                return std::nullopt;
            }
            const auto modified = it->last_write_time(error);
            result.files.emplace(*name, CapturedFile{std::move(*bytes), modified});
        }
    }
    if (error) {
        ADD_FAILURE() << error.message();
        return std::nullopt;
    }
    std::ranges::sort(result.directories);
    return result;
}

const Json& row(const Json& report, std::string_view id) {
    const Json* results = report.find("results");
    if (results)
        for (const auto& candidate : results->items())
            if (candidate.stringAt("id") == id)
                return candidate;
    ADD_FAILURE() << "missing registration " << id << " in " << report.dump();
    static const Json missing;
    return missing;
}

const std::vector<Json>& targets(const Json& entry) {
    const Json* value = entry.find("targets");
    if (value && value->isArray())
        return value->items();
    ADD_FAILURE() << "missing targets in " << entry.dump();
    static const std::vector<Json> empty;
    return empty;
}

class BindingMapTest : public testing::Test {
protected:
    void SetUp() override {
        auto fixture = frost::io::joinPath(FROST_BINDING_MAP_FIXTURE_DIR, "registrations.cpp");
        ASSERT_TRUE(fixture.has_value());
        auto bytes = frost::io::realFileSystem().readFile(*fixture);
        ASSERT_TRUE(bytes.has_value()) << bytes.error().describe();
        source = std::move(*bytes);
        ASSERT_TRUE(write("fixture.cpp", source));
        ASSERT_TRUE(writeDatabase());
    }

    bool write(std::string_view file, std::string_view bytes) {
        auto written = frost::io::realFileSystem().writeFileAtomic(scratch.join(file), bytes);
        EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().describe());
        return written.has_value();
    }

    Json command(std::string_view extraFlag = {}) const {
        // The CDB selects the pinned compiler's GNU driver explicitly on both supported hosts.
        // Deliberate output flags prove analysis strips object/dependency generation.
        Json arguments =
            Json::array({FROST_TEST_CXX_COMPILER, "--driver-mode=g++", "-std=c++20", "-c", scratch.join("fixture.cpp"),
                         "-o", scratch.join("forbidden.o"), "-MMD", "-MF", scratch.join("forbidden.d")});
        if (!extraFlag.empty())
            arguments.push(extraFlag);
        return Json::object()
            .set("directory", scratch.path())
            .set("file", scratch.join("fixture.cpp"))
            .set("arguments", std::move(arguments));
    }

    bool writeDatabase(std::string_view extraFlag = {}, bool multiple = false) {
        Json commands = Json::array({command(extraFlag)});
        if (multiple)
            commands.push(command("-DSECOND_COMPILATION_CONTEXT=1"));
        return write("compile_commands.json", commands.dump());
    }

    Json registration(std::string_view id, std::string_view name, std::string_view kind = "Function") const {
        const std::string marker = "// BIND " + std::string(id) + "\n";
        const auto offset = source.find(marker);
        EXPECT_NE(offset, std::string::npos) << marker;
        const auto line =
            offset == std::string::npos
                ? 1
                : 1 + std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(offset), '\n');
        return Json::object()
            .set("id", id)
            .set("qualifiedName", "Lua." + std::string(name))
            .set("name", name)
            .set("kind", kind)
            .set("source", Json::object().set("file", "fixture.cpp").set("line", line));
    }

    Json request(Json registrations) const {
        return Json::object()
            .set("schemaVersion", 1)
            .set("workspaceRoot", scratch.path())
            .set("compileCommandsDirectory", scratch.path())
            .set("translationUnit", scratch.join("fixture.cpp"))
            .set("registrations", std::move(registrations));
    }

    std::optional<Json> run(Json input, int expectedExit = 0) {
        if (!write("request.json", input.dump()))
            return std::nullopt;
        auto before = snapshot(scratch.path());
        if (!before)
            return std::nullopt;
        frost::io::ProcessSpec spec;
        spec.executable = FROST_CLANG_BINARY;
        spec.workingDirectory = scratch.path();
        spec.arguments = {"--binding-map", scratch.join("request.json")};
        auto output = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", 60s);
        auto after = snapshot(scratch.path());
        EXPECT_TRUE(after.has_value());
        if (after)
            EXPECT_EQ(*before, *after) << "read-only analysis changed workspace files, timestamps or directories";
        if (!output) {
            ADD_FAILURE() << output.error().describe();
            return std::nullopt;
        }
        EXPECT_EQ(output->exitCode, expectedExit) << output->stderrBytes << '\n' << output->stdoutBytes;
        auto parsed = frost::rpc::parseJson(output->stdoutBytes);
        if (!parsed) {
            ADD_FAILURE() << parsed.error().describe() << '\n' << output->stdoutBytes;
            return std::nullopt;
        }
        return std::move(*parsed);
    }

    std::string rangeBytes(const Json& range) const {
        const auto offset = range.unsignedAt("offset");
        const auto length = range.unsignedAt("length");
        EXPECT_EQ(range.stringAt("path"), scratch.join("fixture.cpp"));
        if (!offset || !length || *offset > source.size() || *length > source.size() - *offset) {
            ADD_FAILURE() << "not a fixture byte range: " << range.dump();
            return {};
        }
        return source.substr(static_cast<std::size_t>(*offset), static_cast<std::size_t>(*length));
    }

    frost::test::ScratchDir scratch{"clang-binding-map"};
    std::string source;
};

TEST_F(BindingMapTest, NativeArgumentIdentityOverridesLuaSpellingAndPreservesImmutableAliases) {
    auto output =
        run(request(Json::array({registration("direct", "visibleLuaName"), registration("alias", "alias"),
                                 registration("declared", "declared"), registration("multiline", "multiline")})));
    ASSERT_TRUE(output.has_value());
    EXPECT_EQ(output->boolAt("translationUnitParsed"), true);
    const auto& direct = row(*output, "direct");
    const auto& alias = row(*output, "alias");
    ASSERT_EQ(direct.stringAt("status"), "mapped") << direct.dump();
    ASSERT_EQ(alias.stringAt("status"), "mapped") << alias.dump();
    ASSERT_EQ(targets(direct).size(), 1U);
    ASSERT_EQ(targets(alias).size(), 1U);
    const auto& actual = targets(direct).front();
    EXPECT_EQ(actual.stringAt("qualifiedName"), "native::actualTarget");
    EXPECT_FALSE(actual.stringAt("usr").empty());
    EXPECT_EQ(actual.stringAt("usr"), targets(alias).front().stringAt("usr"));
    ASSERT_EQ(row(*output, "multiline").stringAt("status"), "mapped");
    ASSERT_EQ(targets(row(*output, "multiline")).size(), 1U);
    EXPECT_EQ(actual.stringAt("usr"), targets(row(*output, "multiline")).front().stringAt("usr"));
    ASSERT_NE(actual.find("declaration"), nullptr);
    EXPECT_EQ(rangeBytes(*actual.find("declaration")), "actualTarget");
    EXPECT_EQ(actual.find("declaration")->unsignedAt("offset"), source.find("int actualTarget") + 4);
    ASSERT_NE(actual.find("argumentRange"), nullptr);
    EXPECT_EQ(rangeBytes(*actual.find("argumentRange")), "&native::actualTarget");
    ASSERT_NE(targets(alias).front().find("argumentRange"), nullptr);
    EXPECT_EQ(rangeBytes(*targets(alias).front().find("argumentRange")), "aliasOfAlias");
    ASSERT_NE(direct.find("registrationRange"), nullptr);
    EXPECT_NE(rangeBytes(*direct.find("registrationRange")).find("api.Function(\"visibleLuaName\""), std::string::npos);
    const auto& declared = row(*output, "declared");
    ASSERT_EQ(declared.stringAt("status"), "mapped") << declared.dump();
    ASSERT_EQ(targets(declared).size(), 1U);
    const auto& target = targets(declared).front();
    ASSERT_NE(target.find("declaration"), nullptr);
    ASSERT_NE(target.find("definition"), nullptr);
    EXPECT_EQ(target.find("declaration")->unsignedAt("offset"), source.find("int declaredTarget(int);") + 4);
    EXPECT_EQ(target.find("definition")->unsignedAt("offset"), source.find("int declaredTarget(int input)") + 4);
}

TEST_F(BindingMapTest, SourceManifestDigestsActualTuAndHeaderBuffersEvenWhenUsrsStayTheSame) {
    const std::string header = "#pragma once\ninline constexpr int headerValue = 1;\n";
    ASSERT_TRUE(write("native_header.hpp", header));
    source = "#include \"native_header.hpp\"\n" + source;
    ASSERT_TRUE(write("fixture.cpp", source));
    auto first = run(request(Json::array({registration("direct", "visibleLuaName")})));
    ASSERT_TRUE(first);
    const auto digest = [&](const Json& report, std::string_view file) {
        const auto* inputs = report.find("sourceInputs");
        EXPECT_NE(inputs, nullptr);
        if (inputs)
            for (const auto& input : inputs->items())
                if (input.stringAt("path") == scratch.join(file))
                    return std::string(input.stringAt("sha256"));
        ADD_FAILURE() << "actual parsed source missing from manifest: " << file;
        return std::string{};
    };
    EXPECT_EQ(digest(*first, "fixture.cpp"), frost::io::sha256Hex(source));
    EXPECT_EQ(digest(*first, "native_header.hpp"), frost::io::sha256Hex(header));
    const std::string changedHeader = "#pragma once\ninline constexpr int headerValue = 2;\n";
    ASSERT_TRUE(write("native_header.hpp", changedHeader));
    auto second = run(request(Json::array({registration("direct", "visibleLuaName")})));
    ASSERT_TRUE(second);
    ASSERT_EQ(targets(row(*first, "direct")).size(), 1U);
    ASSERT_EQ(targets(row(*second, "direct")).size(), 1U);
    EXPECT_EQ(targets(row(*first, "direct")).front().stringAt("usr"),
              targets(row(*second, "direct")).front().stringAt("usr"));
    EXPECT_EQ(digest(*second, "fixture.cpp"), frost::io::sha256Hex(source));
    EXPECT_EQ(digest(*second, "native_header.hpp"), frost::io::sha256Hex(changedHeader));
    EXPECT_NE(digest(*first, "native_header.hpp"), digest(*second, "native_header.hpp"));
}

TEST_F(BindingMapTest, TextualExternalHeaderPreservesContextWithoutBecomingAWorkspaceManifestInput) {
    frost::test::ScratchDir external{"clang-binding-map-external-text"};
    const auto header = external.join("context.hpp");
    ASSERT_TRUE(
        frost::io::realFileSystem().writeFileAtomic(header, "#pragma once\ninline constexpr int externalValue = 9;\n"));
    source = "#include \"" + header + "\"\nstatic_assert(externalValue == 9);\n" + source;
    ASSERT_TRUE(write("fixture.cpp", source));
    const auto externalBefore = snapshot(external.path());
    ASSERT_TRUE(externalBefore);
    auto output = run(request(Json::array({registration("direct", "visibleLuaName")})));
    ASSERT_TRUE(output);
    EXPECT_EQ(output->boolAt("translationUnitParsed"), true);
    EXPECT_EQ(row(*output, "direct").stringAt("status"), "mapped");
    const auto* inputs = output->find("sourceInputs");
    ASSERT_NE(inputs, nullptr);
    EXPECT_TRUE(
        std::ranges::none_of(inputs->items(), [&](const Json& input) { return input.stringAt("path") == header; }));
    EXPECT_EQ(snapshot(external.path()), externalBefore);
}

TEST_F(BindingMapTest, SerializedHeaderAstInsideOrOutsideWorkspaceCannotClaimFreshlyParsedSourceDigests) {
    frost::test::ScratchDir external{"clang-binding-map-external-pch"};
    source = "static_assert(serializedValue == 3);\n" + source;
    ASSERT_TRUE(write("fixture.cpp", source));
    for (const bool outsideWorkspace : {false, true}) {
        SCOPED_TRACE(outsideWorkspace ? "external serialized header" : "workspace serialized header");
        const auto header = outsideWorkspace ? external.join("serialized.hpp") : scratch.join("serialized.hpp");
        const auto serialized = outsideWorkspace ? external.join("serialized.pch") : scratch.join("serialized.pch");
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(
            header, "#pragma once\ninline constexpr int serializedValue = 3;\n"));
        frost::io::ProcessSpec compile;
        compile.executable = FROST_TEST_CXX_COMPILER;
        compile.workingDirectory = scratch.path();
        compile.arguments = {"--driver-mode=g++",
                             "--no-default-config",
                             "-std=c++20",
                             "-fno-implicit-modules",
                             "-fno-implicit-module-maps",
                             "-x",
                             "c++-header",
                             header,
                             "-o",
                             serialized};
        auto pch = frost::io::runToCompletion(frost::io::realProcessLauncher(), compile, "", 60s);
        ASSERT_TRUE(pch) << pch.error().message();
        ASSERT_EQ(pch->exitCode, 0) << pch->stderrBytes;
        Json cdb = command();
        cdb.find("arguments")->push("-include-pch").push(serialized);
        ASSERT_TRUE(write("compile_commands.json", Json::array({std::move(cdb)}).dump()));
        const auto externalBefore = snapshot(external.path());
        ASSERT_TRUE(externalBefore);
        auto output = run(request(Json::array({registration("direct", "visibleLuaName")})), 1);
        ASSERT_TRUE(output);
        EXPECT_EQ(output->boolAt("translationUnitParsed"), false);
        EXPECT_EQ(row(*output, "direct").stringAt("reason"), "source-input-buffers-unavailable");
        EXPECT_TRUE(targets(row(*output, "direct")).empty());
        EXPECT_EQ(snapshot(external.path()), externalBefore);
    }
}

TEST_F(BindingMapTest, ProvenMsvcYuFirstForcedHeaderIsParsedTextuallyAndInvocationReplaysInPinnedCompiler) {
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(scratch.join("include")));
    ASSERT_TRUE(write("include/nested.hpp", "#pragma once\n#define FEC13_NESTED 23\nstruct PchOnlyType {};\n"));
    const std::string header = "#pragma once\n#include \"nested.hpp\"\n";
    ASSERT_TRUE(write("cmake_pch.hxx", header));
    ASSERT_TRUE(write("pch_source.cpp", "// actual compiler creates the PCH from its explicit forced source header\n"));
    const std::vector<std::string> semantic{"--driver-mode=cl",  "--no-default-config",         "/std:c++20", "/TP",
                                            "/DFEC13_ORACLE=17", "/I" + scratch.join("include")};
    frost::io::ProcessSpec create;
    create.executable = FROST_TEST_CXX_COMPILER;
    create.workingDirectory = scratch.path();
    create.arguments = semantic;
    create.arguments.insert(create.arguments.end(),
                            {"/Yc" + scratch.join("cmake_pch.hxx"), "/Fp" + scratch.join("native.pch"),
                             "/FI" + scratch.join("cmake_pch.hxx"), "/c", scratch.join("pch_source.cpp"),
                             "/Fo" + scratch.join("pch.obj")});
    auto pch = frost::io::runToCompletion(frost::io::realProcessLauncher(), create, "", 60s);
    ASSERT_TRUE(pch) << pch.error().message();
    ASSERT_EQ(pch->exitCode, 0) << pch->stderrBytes;
    source = "#if FEC13_ORACLE != 17 || FEC13_NESTED != 23\n#error lost actual PCH source context\n#endif\nPchOnlyType "
             "parsedType;\n" +
             source;
    ASSERT_TRUE(write("fixture.cpp", source));
    frost::io::ProcessSpec baseline = create;
    baseline.arguments = semantic;
    baseline.arguments.insert(baseline.arguments.end(),
                              {"/Yu" + scratch.join("cmake_pch.hxx"), "/Fp" + scratch.join("native.pch"),
                               "/FI" + scratch.join("cmake_pch.hxx"), "/c", scratch.join("fixture.cpp"),
                               "/Fo" + scratch.join("baseline.obj")});
    auto originalCompiled = frost::io::runToCompletion(frost::io::realProcessLauncher(), baseline, "", 60s);
    ASSERT_TRUE(originalCompiled) << originalCompiled.error().message();
    ASSERT_EQ(originalCompiled->exitCode, 0) << originalCompiled->stderrBytes;
    Json arguments = Json::array({FROST_TEST_CXX_COMPILER});
    for (const auto& argument : semantic)
        arguments.push(argument);
    arguments.push("/Yu" + scratch.join("cmake_pch.hxx"))
        .push("/Fp" + scratch.join("native.pch"))
        .push("/FI" + scratch.join("cmake_pch.hxx"))
        .push("/c")
        .push(scratch.join("fixture.cpp"))
        .push("/Fo" + scratch.join("forbidden.obj"));
    const Json original = arguments;
    ASSERT_TRUE(write("compile_commands.json", Json::array({Json::object()
                                                                .set("directory", scratch.path())
                                                                .set("file", scratch.join("fixture.cpp"))
                                                                .set("arguments", std::move(arguments))})
                                                   .dump()));
    auto output = run(request(Json::array({registration("direct", "visibleLuaName")})));
    ASSERT_TRUE(output);
    ASSERT_EQ(row(*output, "direct").stringAt("status"), "mapped") << output->dump();
    const auto* context = output->find("compilationContext");
    ASSERT_NE(context, nullptr);
    ASSERT_NE(context->find("originalArguments"), nullptr);
    EXPECT_EQ(*context->find("originalArguments"), original);
    const auto* actual = context->find("analysisArguments");
    ASSERT_NE(actual, nullptr);
    ASSERT_FALSE(actual->items().empty());
    frost::io::ProcessSpec replay;
    ASSERT_NE(actual->items().front().asString(), nullptr);
    replay.executable = *actual->items().front().asString();
    replay.workingDirectory = scratch.path();
    for (std::size_t i = 1; i < actual->items().size(); ++i) {
        ASSERT_NE(actual->items()[i].asString(), nullptr);
        replay.arguments.push_back(*actual->items()[i].asString());
    }
    EXPECT_TRUE(std::ranges::find(replay.arguments, "/Yu" + scratch.join("cmake_pch.hxx")) == replay.arguments.end());
    EXPECT_TRUE(std::ranges::find(replay.arguments, "/Fp" + scratch.join("native.pch")) == replay.arguments.end());
    EXPECT_NE(std::ranges::find(replay.arguments, "/FI" + scratch.join("cmake_pch.hxx")), replay.arguments.end());
    const auto before = snapshot(scratch.path());
    ASSERT_TRUE(before);
    auto compiled = frost::io::runToCompletion(frost::io::realProcessLauncher(), replay, "", 60s);
    ASSERT_TRUE(compiled) << compiled.error().message();
    EXPECT_EQ(compiled->exitCode, 0) << compiled->stderrBytes;
    EXPECT_EQ(snapshot(scratch.path()), before);
    const auto* inputs = output->find("sourceInputs");
    ASSERT_NE(inputs, nullptr);
    EXPECT_TRUE(std::ranges::any_of(inputs->items(), [&](const Json& input) {
        return input.stringAt("path") == scratch.join("cmake_pch.hxx") &&
               input.stringAt("sha256") == frost::io::sha256Hex(header);
    }));
    EXPECT_TRUE(std::ranges::any_of(inputs->items(), [&](const Json& input) {
        return input.stringAt("path") == scratch.join("include/nested.hpp");
    }));
}

TEST_F(BindingMapTest, MsvcPchWithoutExactFirstForcedSourceHeaderNeverUsesAGuessedContext) {
    ASSERT_TRUE(write("pch.hpp", "#pragma once\n"));
    ASSERT_TRUE(write("other.hpp", "#pragma once\n"));
    for (const bool mismatched : {false, true}) {
        Json arguments = Json::array({FROST_TEST_CXX_COMPILER, "--driver-mode=cl", "/std:c++20", "/TP",
                                      "/Yu" + scratch.join("pch.hpp"), "/Fp" + scratch.join("not-read.pch")});
        if (mismatched)
            arguments.push("/FI" + scratch.join("other.hpp"));
        arguments.push(scratch.join("fixture.cpp"));
        ASSERT_TRUE(write("compile_commands.json", Json::array({Json::object()
                                                                    .set("directory", scratch.path())
                                                                    .set("file", scratch.join("fixture.cpp"))
                                                                    .set("arguments", std::move(arguments))})
                                                       .dump()));
        auto output = run(request(Json::array({registration("direct", "visibleLuaName")})), 1);
        ASSERT_TRUE(output);
        EXPECT_EQ(row(*output, "direct").stringAt("reason"), "unproven-msvc-pch-source-context");
        EXPECT_TRUE(targets(row(*output, "direct")).empty());
    }
}

TEST_F(BindingMapTest, NativeUsrsEqualIndependentPinnedRawClangdAtExactDeclarationTokens) {
    const std::vector<std::pair<std::string_view, std::string_view>> cases{{"direct", "int actualTarget"},
                                                                           {"integer", "int overloaded"},
                                                                           {"real", "double overloaded"},
                                                                           {"member", "int method"},
                                                                           {"field", "int field"}};
    std::vector<std::size_t> offsets;
    for (const auto& [id, declaration] : cases) {
        (void)id;
        const auto at = source.find(declaration);
        ASSERT_NE(at, std::string::npos);
        offsets.push_back(at + declaration.find(' ') + 1);
    }
    auto output = run(request(Json::array({registration("direct", "visibleLuaName"), registration("integer", "integer"),
                                           registration("real", "real"), registration("member", "member"),
                                           registration("field", "field", "Property")})));
    ASSERT_TRUE(output.has_value());
    auto before = snapshot(scratch.path());
    ASSERT_TRUE(before.has_value());
    const auto expected = frost::test::rawBindingUsrs(FROST_TEST_CXX_COMPILER, scratch.path(),
                                                      scratch.join("fixture.cpp"), source, offsets);
    auto after = snapshot(scratch.path());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*before, *after) << "raw-clangd oracle changed the workspace";
    ASSERT_TRUE(expected.has_value());
    ASSERT_EQ(expected->size(), cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& actual = row(*output, cases[i].first);
        ASSERT_EQ(actual.stringAt("status"), "mapped") << actual.dump();
        ASSERT_EQ(targets(actual).size(), 1U);
        EXPECT_EQ(targets(actual).front().stringAt("usr"), (*expected)[i]) << cases[i].first;
        ASSERT_NE(targets(actual).front().find("declaration"), nullptr);
        EXPECT_EQ(targets(actual).front().find("declaration")->unsignedAt("offset"), offsets[i]);
    }
}

TEST_F(BindingMapTest, LuaSurfaceLiteralEvidenceIncludesExactPhysicalSpellingAndDecodedValue) {
    auto output = run(request(
        Json::array({registration("direct", "visibleLuaName"), registration("escaped", "escaped"),
                     registration("concatenated", "concatenated"), registration("computed", "computed"),
                     registration("macroName", "macroName"), registration("constructor", "Object", "Constructor"),
                     registration("operator", "__add", "Operator"), registration("duplicate", "duplicate")})));
    ASSERT_TRUE(output.has_value());
    for (const auto id : {"direct", "escaped"}) {
        const auto& actual = row(*output, id);
        ASSERT_EQ(actual.stringAt("status"), "mapped") << actual.dump();
        const auto* literal = actual.find("nameLiteral");
        ASSERT_NE(literal, nullptr);
        ASSERT_NE(literal->find("range"), nullptr);
        EXPECT_EQ(rangeBytes(*literal->find("range")), literal->stringAt("spelling"));
        EXPECT_EQ(literal->stringAt("decodedValue"), id == std::string_view("direct") ? "visibleLuaName" : "escaped");
    }
    EXPECT_EQ(row(*output, "direct").find("nameLiteral")->stringAt("spelling"), "\"visibleLuaName\"");
    EXPECT_EQ(row(*output, "escaped").find("nameLiteral")->stringAt("spelling"), "\"esc\\x61ped\"");
    for (const auto id : {"concatenated", "computed", "macroName", "constructor", "operator", "duplicate"}) {
        const auto& actual = row(*output, id);
        EXPECT_EQ(actual.find("nameLiteral"), nullptr) << actual.dump();
        EXPECT_FALSE(actual.stringAt("nameLiteralReason").empty()) << actual.dump();
    }
    EXPECT_EQ(row(*output, "computed").stringAt("status"), "unavailable");
    EXPECT_EQ(row(*output, "duplicate").stringAt("status"), "ambiguous");
}

TEST_F(BindingMapTest, StructuredSignatureEvidenceUsesOnlyTheActualSelectedDocField) {
    auto output =
        run(request(Json::array({registration("documented", "documented"), registration("docEscaped", "docEscaped"),
                                 registration("computedDoc", "computedDoc"), registration("mutableDoc", "mutableDoc"),
                                 registration("mismatchedDoc", "mismatchedDoc")})));
    ASSERT_TRUE(output);
    for (const auto id : {"documented", "docEscaped"}) {
        const auto& actual = row(*output, id);
        ASSERT_EQ(actual.stringAt("status"), "mapped") << actual.dump();
        const auto* signature = actual.find("docSignatureLiteral");
        ASSERT_NE(signature, nullptr);
        ASSERT_NE(signature->find("range"), nullptr);
        EXPECT_EQ(rangeBytes(*signature->find("range")), signature->stringAt("spelling"));
        const auto offset = signature->unsignedAt("decodedNameOffset");
        const auto length = signature->unsignedAt("decodedNameLength");
        ASSERT_TRUE(offset && length);
        EXPECT_EQ(signature->stringAt("decodedValue").substr(*offset, *length), id);
    }
    EXPECT_EQ(row(*output, "documented").find("docSignatureLiteral")->unsignedAt("decodedNameOffset"), 2U);
    EXPECT_EQ(row(*output, "docEscaped").find("docSignatureLiteral")->stringAt("spelling"),
              "\"\\x64ocEscaped(): integer\"");
    for (const auto id : {"computedDoc", "mutableDoc", "mismatchedDoc"}) {
        const auto& actual = row(*output, id);
        EXPECT_EQ(actual.find("docSignatureLiteral"), nullptr);
        EXPECT_FALSE(actual.stringAt("docSignatureLiteralReason").empty());
    }
}

TEST_F(BindingMapTest, CastOverloadsAndLambdasKeepTheirOwnDistinctSemanticIdentities) {
    auto output = run(request(Json::array({registration("integer", "integer"), registration("real", "real"),
                                           registration("lambda", "lambda"), registration("lambdaTwo", "lambdaTwo")})));
    ASSERT_TRUE(output.has_value());
    for (const auto id : {"integer", "real", "lambda", "lambdaTwo"}) {
        ASSERT_EQ(row(*output, id).stringAt("status"), "mapped") << row(*output, id).dump();
        ASSERT_EQ(targets(row(*output, id)).size(), 1U);
    }
    const auto& integer = targets(row(*output, "integer")).front();
    const auto& real = targets(row(*output, "real")).front();
    EXPECT_NE(integer.stringAt("usr"), real.stringAt("usr"));
    ASSERT_NE(integer.find("declaration"), nullptr);
    ASSERT_NE(real.find("declaration"), nullptr);
    EXPECT_EQ(rangeBytes(*integer.find("declaration")), "overloaded");
    EXPECT_EQ(rangeBytes(*real.find("declaration")), "overloaded");
    EXPECT_EQ(integer.find("declaration")->unsignedAt("offset"), source.find("int overloaded") + 4);
    EXPECT_EQ(real.find("declaration")->unsignedAt("offset"), source.find("double overloaded") + 7);
    const auto& firstLambda = targets(row(*output, "lambda")).front();
    const auto& secondLambda = targets(row(*output, "lambdaTwo")).front();
    EXPECT_EQ(firstLambda.stringAt("kind"), "lambda");
    EXPECT_EQ(secondLambda.stringAt("kind"), "lambda");
    EXPECT_NE(firstLambda.stringAt("usr"), secondLambda.stringAt("usr"));
    EXPECT_NE(firstLambda.stringAt("qualifiedName"), "native::actualTarget");
    EXPECT_NE(secondLambda.stringAt("qualifiedName"), "native::actualTarget");
    ASSERT_NE(firstLambda.find("declaration"), nullptr);
    EXPECT_NE(rangeBytes(*firstLambda.find("declaration")).find("return native::actualTarget(input)"),
              std::string::npos);
}

TEST_F(BindingMapTest, PropertyAccessorsAndMemberPointersRetainActualRolesAndDeclarations) {
    auto output =
        run(request(Json::array({registration("property", "property", "Property"),
                                 registration("field", "field", "Property"), registration("member", "member")})));
    ASSERT_TRUE(output.has_value());
    const auto& property = row(*output, "property");
    ASSERT_EQ(property.stringAt("status"), "mapped") << property.dump();
    ASSERT_EQ(targets(property).size(), 2U);
    EXPECT_EQ(targets(property)[0].stringAt("role"), "getter");
    EXPECT_EQ(targets(property)[0].stringAt("qualifiedName"), "native::getter");
    EXPECT_EQ(targets(property)[1].stringAt("role"), "setter");
    EXPECT_EQ(targets(property)[1].stringAt("qualifiedName"), "native::setter");
    EXPECT_NE(targets(property)[0].stringAt("usr"), targets(property)[1].stringAt("usr"));
    const auto& field = row(*output, "field");
    ASSERT_EQ(field.stringAt("status"), "mapped") << field.dump();
    ASSERT_EQ(targets(field).size(), 1U);
    EXPECT_EQ(targets(field).front().stringAt("kind"), "field");
    EXPECT_EQ(targets(field).front().stringAt("qualifiedName"), "native::Object::field");
    const auto& member = row(*output, "member");
    ASSERT_EQ(member.stringAt("status"), "mapped") << member.dump();
    ASSERT_EQ(targets(member).size(), 1U);
    EXPECT_EQ(targets(member).front().stringAt("kind"), "method");
    EXPECT_EQ(targets(member).front().stringAt("qualifiedName"), "native::Object::method");
}

TEST_F(BindingMapTest, GlobalStaticAndConstructorRegistrationsUseTheirActualBuilderSignatures) {
    auto output = run(request(Json::array(
        {registration("global", "global", "GlobalFunction"), registration("static", "static"),
         registration("constructor", "Object", "Constructor"), registration("operator", "__add", "Operator")})));
    ASSERT_TRUE(output.has_value());
    for (const auto id : {"global", "static", "constructor", "operator"}) {
        ASSERT_EQ(row(*output, id).stringAt("status"), "mapped") << row(*output, id).dump();
        ASSERT_EQ(targets(row(*output, id)).size(), 1U);
    }
    EXPECT_EQ(targets(row(*output, "global")).front().stringAt("qualifiedName"), "native::actualTarget");
    EXPECT_EQ(targets(row(*output, "global")).front().stringAt("usr"),
              targets(row(*output, "static")).front().stringAt("usr"));
    EXPECT_EQ(targets(row(*output, "constructor")).front().stringAt("kind"), "lambda");
    EXPECT_EQ(targets(row(*output, "operator")).front().stringAt("qualifiedName"), "native::actualTarget");
}

TEST_F(BindingMapTest, UnsupportedAndAmbiguousArgumentsNeverProduceGuessedTargets) {
    auto output = run(request(Json::array(
        {registration("mutable", "mutable"), registration("mutableMember", "mutableMember"),
         registration("factory", "factory"), registration("macro", "macro"), registration("conditional", "conditional"),
         registration("overloadSet", "overloadSet"), registration("duplicate", "duplicate"),
         registration("foreign", "foreign"), registration("comment", "comment"), registration("string", "string")})));
    ASSERT_TRUE(output.has_value());
    for (const auto id : {"mutable", "mutableMember", "factory", "macro", "foreign", "comment", "string"}) {
        const auto& entry = row(*output, id);
        EXPECT_EQ(entry.stringAt("status"), "unavailable") << entry.dump();
        EXPECT_FALSE(entry.stringAt("reason").empty());
        EXPECT_TRUE(targets(entry).empty());
    }
    for (const auto id : {"conditional", "overloadSet", "duplicate"}) {
        const auto& entry = row(*output, id);
        EXPECT_EQ(entry.stringAt("status"), "ambiguous") << entry.dump();
        EXPECT_FALSE(entry.stringAt("reason").empty());
        EXPECT_TRUE(targets(entry).empty());
    }
}

TEST_F(BindingMapTest, ParseFailureInvalidatesEvenAnEarlierValidRegistration) {
    ASSERT_TRUE(write("fixture.cpp", source + "\nthis is invalid C++ !\n"));
    auto output = run(request(Json::array({registration("direct", "visibleLuaName")})), 1);
    ASSERT_TRUE(output.has_value());
    EXPECT_EQ(output->boolAt("translationUnitParsed"), false);
    EXPECT_EQ(row(*output, "direct").stringAt("status"), "unavailable");
    EXPECT_EQ(row(*output, "direct").stringAt("reason"), "translation-unit-parse-failed");
    EXPECT_TRUE(targets(row(*output, "direct")).empty());
}

TEST_F(BindingMapTest, MultipleCompilationContextsRequireExplicitSelection) {
    ASSERT_TRUE(writeDatabase({}, true));
    Json input = request(Json::array({registration("direct", "visibleLuaName")}));
    auto refused = run(input, 1);
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(row(*refused, "direct").stringAt("reason"), "ambiguous-compilation-command");
    input.set("compileCommandIndex", 1);
    auto selected = run(std::move(input));
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(row(*selected, "direct").stringAt("status"), "mapped");
}

TEST_F(BindingMapTest, SideEffectChannelsAndNonCanonicalSchemaPathsAreRefused) {
    for (const auto flag : {"-fplugin=must-not-load", "-ftime-trace", "-save-temps", "-MJforbidden.json",
                            "--config=must-not-expand.cfg", "/clang:--config=must-not-expand.cfg", "--"}) {
        ASSERT_TRUE(writeDatabase(flag));
        auto output = run(request(Json::array({registration("direct", "visibleLuaName")})), 1);
        ASSERT_TRUE(output.has_value());
        EXPECT_EQ(row(*output, "direct").stringAt("reason"), "compile-command-has-unsafe-side-effects");
        EXPECT_TRUE(targets(row(*output, "direct")).empty());
    }
    ASSERT_TRUE(writeDatabase());
    auto bad = registration("direct", "visibleLuaName");
    bad.set("source", Json::object().set("file", "../fixture.cpp").set("line", 1));
    auto output = run(request(Json::array({std::move(bad)})), 2);
    ASSERT_TRUE(output.has_value());
    ASSERT_NE(output->find("error"), nullptr);
    EXPECT_EQ(output->find("error")->stringAt("code"), "invalid-request");
}

} // namespace
