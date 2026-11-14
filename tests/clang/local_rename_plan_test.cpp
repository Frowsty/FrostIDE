// SPDX-License-Identifier: Apache-2.0
// REF-4 preparation, real pinned LibTooling process oracles. Plans are applied only to disposable
// test buffers for compiler validation; product source, headers and compile commands stay intact.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/spawn_log.hpp"
#include "frost/rpc/json.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::rpc::Json;
using namespace std::chrono_literals;

class LocalRenamePlan : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"local-rename-plan"};
    std::string source = scratch.join("input.cpp");
    std::string snapshot = scratch.join("snapshot.cpp");
    std::string database = scratch.join("compile_commands.json");
    std::string compiler = *frost::io::normalizePath(FROST_TEST_CXX_COMPILER);
    std::string helper = *frost::io::normalizePath(FROST_CLANG_BINARY);
    std::optional<int> exit;
    std::string raw;

    void write(const std::string& path, std::string_view bytes) {
        auto result = frost::io::realFileSystem().writeFileAtomic(path, bytes);
        ASSERT_TRUE(result.has_value()) << result.error().describe();
    }

    Json plan(std::string_view bytes, std::size_t offset, std::string name = "renamed",
              std::vector<std::string> extra = {}, std::optional<std::string> disk = std::nullopt) {
        write(source, disk ? *disk : std::string(bytes));
        write(snapshot, bytes);
        // A selected real-tool compile command: preserve its argv0, directory and source while
        // the planner records its syntax-only/output adjustments. No guessed driver mode.
        std::vector<std::string> arguments{compiler};
#ifdef _WIN32
        arguments.insert(arguments.end(), {"/std:c++20", "/c", source, "/Fo" + scratch.join("must-not-exist.obj")});
#else
        arguments.insert(arguments.end(), {"-std=c++20", "-c", source, "-o", scratch.join("must-not-exist.o")});
#endif
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        std::vector<Json> command;
        for (const auto& argument : arguments) {
            command.emplace_back(argument);
        }
        const auto compiledb = Json::array({Json::object()
                                                .set("directory", scratch.path())
                                                .set("file", source)
                                                .set("arguments", Json::array(std::move(command)))})
                                   .dump();
        write(database, compiledb);
        auto selected = frost::rpc::parseJson(*frost::io::realFileSystem().readFile(database));
        EXPECT_TRUE(selected.has_value());
        if (!selected) {
            return {};
        }
        const auto& entry = selected->items().front();
        const auto* argv = entry.find("arguments");
        EXPECT_NE(argv, nullptr);
        if (!argv) {
            return {};
        }
        frost::io::ProcessSpec process;
        process.executable = helper;
        process.workingDirectory = scratch.path();
        process.environment.emplace_back("FROST_LLVM_ROOT",
                                         std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        process.arguments = {"--plan-local-rename",
                             std::string(entry.stringAt("file")),
                             std::to_string(offset),
                             std::move(name),
                             "--snapshot",
                             snapshot,
                             "--working-directory",
                             std::string(entry.stringAt("directory")),
                             "--driver",
                             *argv->items().front().asString(),
                             "--"};
        for (std::size_t i = 1; i < argv->items().size(); ++i) {
            process.arguments.push_back(*argv->items()[i].asString());
        }
        auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), process, "", 60s);
        EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().describe());
        if (!result) {
            return {};
        }
        exit = result->exitCode;
        raw = result->stdoutBytes;
        EXPECT_EQ(*frost::io::realFileSystem().readFile(source), disk ? *disk : std::string(bytes));
        EXPECT_EQ(*frost::io::realFileSystem().readFile(snapshot), bytes);
        EXPECT_EQ(*frost::io::realFileSystem().readFile(database), compiledb);
        EXPECT_FALSE(std::filesystem::exists(scratch.join("must-not-exist.obj")));
        EXPECT_FALSE(std::filesystem::exists(scratch.join("must-not-exist.o")));
        auto parsed = frost::rpc::parseJson(raw);
        EXPECT_TRUE(parsed.has_value()) << raw << '\n' << result->stderrBytes;
        return parsed ? std::move(*parsed) : Json();
    }

    void refuses(const Json& value, std::string_view status = {}) {
        ASSERT_TRUE(exit.has_value());
        EXPECT_NE(*exit, 0);
        EXPECT_NE(value.stringAt("status"), "ready") << raw;
        if (!status.empty()) {
            EXPECT_EQ(value.stringAt("status"), status) << raw;
        }
        const auto* edits = value.find("edits");
        ASSERT_NE(edits, nullptr);
        EXPECT_TRUE(edits->isArray());
        EXPECT_TRUE(edits->items().empty());
    }

    std::string apply(const Json& value, std::string bytes) {
        EXPECT_EQ(exit, 0);
        EXPECT_EQ(value.stringAt("status"), "ready") << raw;
        const auto* edits = value.find("edits");
        EXPECT_NE(edits, nullptr);
        if (!edits) {
            return {};
        }
        std::size_t previousEnd = 0;
        for (const auto& edit : edits->items()) {
            const auto offset = edit.unsignedAt("offset").value_or(bytes.size() + 1);
            const auto length = edit.unsignedAt("length").value_or(bytes.size() + 1);
            EXPECT_GE(offset, previousEnd);
            EXPECT_LE(offset, bytes.size());
            if (offset > bytes.size() || length > bytes.size() - offset) {
                ADD_FAILURE() << "Out-of-snapshot edit";
                return {};
            }
            EXPECT_EQ(bytes.substr(offset, length), edit.stringAt("expectedText"));
            previousEnd = offset + length;
        }
        for (auto it = edits->items().rbegin(); it != edits->items().rend(); ++it) {
            bytes.replace(*it->unsignedAt("offset"), *it->unsignedAt("length"), it->stringAt("replacement"));
        }
        return bytes;
    }

    void compiles(std::string_view bytes) {
        const auto candidate = scratch.join("candidate.cpp");
        write(candidate, bytes);
        frost::io::ProcessSpec process;
        process.executable = compiler;
        process.workingDirectory = scratch.path();
#ifdef _WIN32
        process.arguments = {"/Zs", "/std:c++20", "/EHsc", candidate};
#else
        process.arguments = {"-fsyntax-only", "-std=c++20", candidate};
#endif
        auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), process, "", 60s);
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_EQ(result->exitCode, 0) << result->stdoutBytes << '\n' << result->stderrBytes;
    }
};

TEST_F(LocalRenamePlan, DeclarationIdentityKeepsShadowedAndOtherFunctionLocals) {
    const std::string bytes = "int f(){ int count=1; { int count=3; (void)count; } return count; }\n"
                              "int g(){ int count=8; return count; } //count\n"
                              "const char* text=\"count\";\n";
    auto result = plan(bytes, bytes.find("count"));
    ASSERT_EQ(result.stringAt("status"), "ready") << raw;
    EXPECT_EQ(result.find("edits")->items().size(), 2);
    const auto candidate = apply(result, bytes);
    EXPECT_EQ(candidate, "int f(){ int renamed=1; { int count=3; (void)count; } return renamed; }\n"
                         "int g(){ int count=8; return count; } //count\nconst char* text=\"count\";\n");
    compiles(candidate);
}

TEST_F(LocalRenamePlan, ReferenceSelectionAndParameterCoverAllExactOccurrences) {
    const std::string bytes = "int f(int value){ int values=value+1; return value+values; }\n";
    auto result = plan(bytes, bytes.find("return value") + 8);
    ASSERT_EQ(result.stringAt("status"), "ready") << raw;
    EXPECT_EQ(result.find("edits")->items().size(), 3);
    const auto candidate = apply(result, bytes);
    EXPECT_EQ(candidate, "int f(int renamed){ int values=renamed+1; return renamed+values; }\n");
    compiles(candidate);
}

TEST_F(LocalRenamePlan, DirtyUtf8SnapshotIsTheOnlyMainFileAuthorityAndIsDeterministic) {
    const std::string bytes = "// \xC3\xA5\xE2\x98\x83\nint f(){ int local=1; return local; }\n";
    const std::string disk = "int unrelated(){return 4;}\n";
    auto first = plan(bytes, bytes.find("local"), "renamed", {}, disk);
    ASSERT_EQ(first.stringAt("status"), "ready") << raw;
    const auto receipt = raw;
    auto second = plan(bytes, bytes.find("local"), "renamed", {}, disk);
    EXPECT_EQ(raw, receipt);
    EXPECT_EQ(first, second);
    ASSERT_NE(first.find("source"), nullptr);
    EXPECT_EQ(first.find("source")->unsignedAt("snapshotBytes"), bytes.size());
    EXPECT_EQ(first.find("source")->stringAt("snapshotSha256"), frost::io::sha256Hex(bytes));
    compiles(apply(first, bytes));
}

TEST_F(LocalRenamePlan, ExistingIdentifiersAndCaptureAreExplicitConflicts) {
    for (const std::string bytes : {"int f(){int local=1; int renamed=2; return local+renamed;}",
                                    "int renamed=2; int f(){int local=1; return local+renamed;}",
                                    "int f(){int local=1; {int renamed=2;(void)renamed;} return local;}",
                                    "using renamed=int; int f(){int local=1; renamed other=2;return local+other;}"}) {
        SCOPED_TRACE(bytes);
        refuses(plan(bytes, bytes.find("local")), "conflict");
    }
}

TEST_F(LocalRenamePlan, MacroCandidateStillRefusesAfterLaterUndef) {
    const std::string bytes = "#define renamed 4\nint f(){int local=1;return local;}\n#undef renamed\n";
    refuses(plan(bytes, bytes.find("local")), "conflict");
}

TEST_F(LocalRenamePlan, MacroExpandedUsesDefinitionsArgumentsAndPasteAreNotSpellingRewrites) {
    for (const std::string bytes : {"#define READ(x) x\nint f(){int local=1;return READ(local);}",
                                    "#define DECL int local=1\nint f(){DECL;return local;}",
                                    "#define JOIN(a,b) a##b\nint f(){int local=1;return JOIN(lo,cal);}",
                                    "#define READ local\nint f(){int local=1;return READ;}"}) {
        SCOPED_TRACE(bytes);
        refuses(plan(bytes, bytes.find("local")), "unsupported");
    }
}

TEST_F(LocalRenamePlan, MacroTypesInsideSizeofAndParametersAreExplicitlyUnsupported) {
    for (const std::string bytes : {"#define TYPE int\nint f(){int local=1;return local+sizeof(TYPE);}",
                                    "#define TYPE int\nint f(TYPE local){return local;}",
                                    "#define PLUS +\nint f(){int local=1;return local PLUS 2;}"}) {
        SCOPED_TRACE(bytes);
        refuses(plan(bytes, bytes.find("local")), "unsupported");
    }
}

TEST_F(LocalRenamePlan, TemplateLambdaMethodAndNonAutomaticSelectionsRefuse) {
    for (const std::string bytes : {"template<class T> int f(T x){int local=1;return local;}",
                                    "int f(){int local=1;auto fn=[&]{return local;};return fn();}",
                                    "struct A{int f(){int local=1;return local;}};",
                                    "int local=1;int f(){return local;}", "int f(){static int local=1;return local;}",
                                    "int f(){struct P{int x,y;};auto [local,y]=P{1,2};return local+y;}"}) {
        SCOPED_TRACE(bytes);
        refuses(plan(bytes, bytes.find("local")), "unsupported");
    }
}

TEST_F(LocalRenamePlan, UnrelatedTemplateAndMacroDoNotDisableOrdinaryFunction) {
    const std::string bytes = "#define OTHER 7\ntemplate<class T>T identity(T t){return t;}\n"
                              "int f(){int local=1;return local;}\n";
    auto result = plan(bytes, bytes.find("local"));
    ASSERT_EQ(result.stringAt("status"), "ready") << raw;
    compiles(apply(result, bytes));
}

TEST_F(LocalRenamePlan, PchResponsePluginsAndOutputActionsRefuseWithoutArtifacts) {
    const std::string bytes = "int f(){int local=1;return local;}\n";
    const auto response = scratch.join("danger.rsp");
    write(response, "-Xclang -load evil-plugin");
    for (const std::vector<std::string> flags : {std::vector<std::string>{"-include-pch", "precompiled.pch"},
                                                 {"/Yucommon.h", "/Fpcommon.pch"},
                                                 {"@" + response},
                                                 {"-I", "@" + response},
                                                 {"-Xclang", "-load", "evil-plugin"},
                                                 {"-fplugin=evil-plugin"},
                                                 {"-M", "-MF", scratch.join("must-not-exist.d")},
                                                 {"--analyze"},
                                                 {"-emit-ast"},
                                                 {"-fmodule-output=" + scratch.join("must-not-exist.pcm")}}) {
        SCOPED_TRACE(flags.front());
        refuses(plan(bytes, bytes.find("local"), "renamed", flags), "unsupported");
        EXPECT_FALSE(std::filesystem::exists(scratch.join("must-not-exist.d")));
        EXPECT_FALSE(std::filesystem::exists(scratch.join("must-not-exist.pcm")));
    }
    EXPECT_EQ(*frost::io::realFileSystem().readFile(response), "-Xclang -load evil-plugin");
}

TEST_F(LocalRenamePlan, NamesOffsetsAndParseFailuresReturnNoEdits) {
    const std::string bytes = "int f(){int local=1;return local;}\n";
    for (const std::string name : {"", "bad-name", "4bad", "int"}) {
        SCOPED_TRACE(name);
        refuses(plan(bytes, bytes.find("local"), name));
    }
    refuses(plan(bytes, bytes.size()), "invalid-input");
    refuses(plan(bytes, bytes.find("return")), "unsupported");
    const std::string invalid = "int f(){int local=; return local;}";
    refuses(plan(invalid, invalid.find("local")), "parse-error");
    const std::string include = "#include \"missing-header.h\"\nint f(){int local=1;return local;}";
    refuses(plan(include, include.find("local")), "parse-error");
}

TEST_F(LocalRenamePlan, IncludedHeaderReceiptsAreFrozenAndInputsRemainUntouched) {
    const auto header = scratch.join("included.h");
    write(header, "inline int extra(){return 3;}\n");
    const std::string bytes = "#include \"included.h\"\nint f(){int local=1;return local+extra();}\n";
    auto result = plan(bytes, bytes.find("local"));
    ASSERT_EQ(result.stringAt("status"), "ready") << raw;
    ASSERT_NE(result.find("dependencies"), nullptr);
    const auto& dependencies = result.find("dependencies")->items();
    EXPECT_TRUE(std::ranges::any_of(dependencies, [](const Json& input) {
        return input.stringAt("path").ends_with("included.h") &&
               input.stringAt("sha256") == frost::io::sha256Hex("inline int extra(){return 3;}\n");
    }));
    EXPECT_EQ(*frost::io::realFileSystem().readFile(header), "inline int extra(){return 3;}\n");
    compiles(apply(result, bytes));
}

TEST_F(LocalRenamePlan, OversizedMainAndArgumentCountRefuseBeforeParsing) {
    std::string bytes(8 * 1024 * 1024 + 1, ' ');
    refuses(plan(bytes, 0), "invalid-input");
    bytes = "int f(){int local=1;return local;}";
    refuses(plan(bytes, bytes.find("local"), "renamed", std::vector<std::string>(4096, "-Dq")), "invalid-input");
}

TEST_F(LocalRenamePlan, TestOnlyDiagnosticsObserveFrozenNamespaceAndAllBudgetBoundaries) {
    frost::io::ProcessSpec process;
    process.executable = helper;
    process.workingDirectory = scratch.path();
    process.environment.emplace_back("FROST_LLVM_ROOT",
                                     std::filesystem::path(compiler).parent_path().parent_path().generic_string());
    process.arguments = {"--check-local-rename-planner"};
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), process, "", 60s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stdoutBytes << '\n' << result->stderrBytes;
    auto parsed = frost::rpc::parseJson(result->stdoutBytes);
    ASSERT_TRUE(parsed.has_value()) << result->stdoutBytes;
    EXPECT_EQ(parsed->stringAt("kind"), "internal-local-rename-checks");
    EXPECT_EQ(parsed->boolAt("passed"), true);
    const auto* checks = parsed->find("checks");
    ASSERT_NE(checks, nullptr);
    const std::set<std::string> expected{"main-bytes",
                                         "dependency-files",
                                         "argument-count",
                                         "argument-bytes",
                                         "edit-count",
                                         "diagnostic-count",
                                         "namespace-path-bytes",
                                         "namespace-entries",
                                         "dependency-bytes",
                                         "output-bytes",
                                         "offset-prefix-map",
                                         "negative-lookup-appearance",
                                         "positive-lookup-removal",
                                         "dependency-content-change",
                                         "frozen-new-namespace-refusal"};
    std::set<std::string> observed;
    ASSERT_EQ(checks->items().size(), expected.size());
    for (const auto& check : checks->items()) {
        ASSERT_TRUE(observed.insert(std::string(check.stringAt("name"))).second);
        EXPECT_EQ(check.boolAt("passed"), true) << check.dump();
        const auto* observations = check.find("observations");
        ASSERT_NE(observations, nullptr);
        if (check.stringAt("kind") == "actual-two-parse-private-VFS") {
            const auto value = check.stringAt("name") == "negative-lookup-appearance" ? 3 : 7;
            EXPECT_EQ(observations->integerAt("originalSetting"), value);
            EXPECT_EQ(observations->integerAt("candidateSetting"), value);
            EXPECT_EQ(observations->boolAt("mutatedBaseHasHeader"),
                      check.stringAt("name") != "positive-lookup-removal");
            EXPECT_EQ(observations->stringAt("plannerStatus"), "ready");
            const auto* actualPlan = observations->find("plannerResult");
            ASSERT_NE(actualPlan, nullptr);
            EXPECT_EQ(actualPlan->stringAt("status"), "ready");
            ASSERT_NE(actualPlan->find("edits"), nullptr);
            EXPECT_EQ(actualPlan->find("edits")->items().size(), 2);
        } else if (check.stringAt("kind") == "pure-production-budget") {
            EXPECT_EQ(observations->boolAt("atBoundaryAdmitted"), true);
            EXPECT_EQ(observations->boolAt("aboveBoundaryRefused"), true);
        }
    }
    EXPECT_EQ(observed, expected);
}

} // namespace
