// SPDX-License-Identifier: Apache-2.0
// Future real pinned Clang semantic/CFG oracles. No package/tool absence is silently skipped.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/inspection.hpp"
#include "frost/rpc/json.hpp"

#include "analyzer_cases.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
namespace {
using frost::rpc::Json;
class Analyzer : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"native-analyzer"};
    std::string file = scratch.join("input.cpp");
    std::string compiler = frost::io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    Json run(const std::string& bytes, std::string rule, std::string mode = "--analyze-unit",
             Json overlays = Json::array()) {
        auto& fs = frost::io::realFileSystem();
        EXPECT_TRUE(fs.writeFileAtomic(file, bytes));
        auto arguments = Json::array({Json(compiler)});
#ifdef _WIN32
        for (const auto& arg :
             std::vector<std::string>{"/std:c++20", "/EHsc", "/c", file, "/Fo" + scratch.join("forbidden.obj")})
            arguments.push(arg);
#ifdef FROST_TEST_VCTOOLS_DIR
        // Replay the configured compiler's SDK pins inside the frozen source snapshot.
        arguments.push("-vctoolsdir" FROST_TEST_VCTOOLS_DIR);
        arguments.push("-winsdkdir" FROST_TEST_WINSDK_DIR);
        arguments.push("-winsdkversion" FROST_TEST_WINSDK_VERSION);
#endif
#else
        for (const auto& arg : std::vector<std::string>{"-std=c++20", "-c", file, "-o", scratch.join("forbidden.o")})
            arguments.push(arg);
#endif
        auto database = Json::array({Json::object()
                                         .set("directory", scratch.path())
                                         .set("file", file)
                                         .set("arguments", std::move(arguments))})
                            .dump();
        auto request = Json::object()
                           .set("projectRoot", scratch.path())
                           .set("compilationDatabase", database)
                           .set("configuration", "fixture-active")
                           .set("commandIndex", 0)
                           .set("overlays", std::move(overlays))
                           .set("rules", Json::array({Json(rule)}));
        frost::io::ProcessSpec spec;
        spec.executable = frost::io::normalizePath(FROST_ANALYZER_BINARY).value();
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.arguments = {mode};
        spec.workingDirectory = scratch.path();
        spec.killOnClose = true;
        spec.environment.emplace_back("FROST_LLVM_ROOT",
                                      std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto output =
            frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, request.dump(), std::chrono::minutes(2));
        if (!output) {
            ADD_FAILURE() << output.error().message();
            return {};
        }
        auto result = frost::rpc::parseJson(output->stdoutBytes);
        EXPECT_TRUE(result) << output->stdoutBytes << output->stderrBytes;
        EXPECT_EQ(output->exitCode, 0) << output->stdoutBytes << output->stderrBytes;
        EXPECT_EQ(fs.readFile(file).value_or("missing"), bytes);
        EXPECT_FALSE(fs.exists(scratch.join("forbidden.obj")).value_or(true));
        EXPECT_FALSE(fs.exists(scratch.join("forbidden.o")).value_or(true));
        return result ? std::move(*result) : Json{};
    }
};
TEST_F(Analyzer, EveryRegisteredRuleHasActualPositiveAndNegativeCppFixtures) {
    for (const auto& fixture : analyzerCases) {
        SCOPED_TRACE(fixture.rule);
        auto positive = run(fixture.positive, fixture.rule);
        ASSERT_EQ(positive.stringAt("status"), "ready");
        const auto* actual = positive.find("findings");
        ASSERT_NE(actual, nullptr);
        EXPECT_TRUE(std::ranges::any_of(actual->items(),
                                        [&](const auto& row) { return row.stringAt("ruleId") == fixture.rule; }));
        auto negative = run(fixture.negative, fixture.rule);
        ASSERT_EQ(negative.stringAt("status"), "ready");
        EXPECT_TRUE(negative.find("findings")->items().empty());
        const auto* registry = positive.find("rules");
        ASSERT_NE(registry, nullptr);
        for (const auto& row : registry->items())
            EXPECT_TRUE(
                std::ranges::any_of(analyzerCases, [&](const auto& test) { return row.stringAt("id") == test.rule; }));
    }
}
TEST_F(Analyzer, ActualNativeSarifUsesSourceRulesAndStandardRegions) {
    auto report = run("int f(int* p){if(!p)return *p;return 0;}\n", "frost-null-dereference", "--sarif");
    ASSERT_TRUE(frost::lang::validateSarifSchema(report));
    const auto& first = report.find("runs")->items().front();
    EXPECT_EQ(first.find("tool")->find("driver")->stringAt("name"), "frost-analyzer");
    EXPECT_EQ(first.stringAt("columnKind"), "unicodeCodePoints");
    ASSERT_FALSE(first.find("results")->items().empty());
    EXPECT_EQ(first.find("results")->items().front().stringAt("ruleId"), "frost-null-dereference");
}
TEST_F(Analyzer, CfgJoinsReinitializationEscapesAndCastOnlyMovesDoNotManufactureDefiniteBugs) {
    for (const auto& source :
         std::vector<std::string>{"int f(bool b){int x=1;int* p=nullptr;if(b)p=&x;return p ? *p : 0;}\n",
                                  "void change(int*&);int f(){int* p=nullptr;change(p);return *p;}\n",
                                  "int f(){int x=1;int* p=nullptr;p=&x;return *p;}\n"}) {
        auto value = run(source, "frost-null-dereference");
        ASSERT_EQ(value.stringAt("status"), "ready");
        EXPECT_TRUE(value.find("findings")->items().empty());
    }
    auto moved = run("#include <string>\n#include <utility>\nunsigned f(){std::string x=\"old\";auto "
                     "y=std::move(x);x.clear();return x.size();}\n",
                     "frost-use-after-move");
    ASSERT_EQ(moved.stringAt("status"), "ready");
    EXPECT_TRUE(moved.find("findings")->items().empty());
}
TEST_F(Analyzer, CleanupBearingReturnMoveStillProducesAnExactFix) {
    auto value =
        run("#include <string>\n#include <utility>\nstd::string f(){std::string x=\"owned\";return std::move(x); }\n",
            "frost-return-move");
    ASSERT_EQ(value.stringAt("status"), "ready");
    ASSERT_EQ(value.find("findings")->items().size(), 1U);
    EXPECT_EQ(value.find("findings")->items().front().stringAt("replacement"), "x");
}
TEST_F(Analyzer, StringViewFindingsRequireAStdStringMaterializedForTheFullExpression) {
    auto value = run("#include <string>\n#include <string_view>\n"
                     "std::string_view from_data(){return std::string(\"abc\").data();}\n"
                     "std::string_view from_cast(){return std::string_view(std::string(\"abc\"));}\n"
                     "std::string_view borrowed(){static const std::string s=\"safe\";return s;}\n",
                     "frost-dangling-string-view");
    ASSERT_EQ(value.stringAt("status"), "ready");
    const auto* findings = value.find("findings");
    ASSERT_NE(findings, nullptr);
    ASSERT_EQ(findings->items().size(), 2U);
    EXPECT_EQ(findings->items()[0].integerAt("line"), 3);
    EXPECT_EQ(findings->items()[1].integerAt("line"), 4);
}
TEST_F(Analyzer, ActualDirtyOverlayChangesFindingsAndReceiptsWhileDiskStaysUnchanged) {
    const std::string disk = "int f(int* p){return 0;}\n", dirty = "int f(int* p){if(!p)return *p;return 0;}\n";
    auto result = run(disk, "frost-null-dereference", "--analyze-unit",
                      Json::array({Json::object().set("path", file).set("contents", dirty)}));
    ASSERT_EQ(result.stringAt("status"), "ready");
    EXPECT_FALSE(result.find("findings")->items().empty());
    EXPECT_FALSE(result.find("dependencies")->items().empty());
}
TEST_F(Analyzer, AstFixesStayInClassAndReferenceReferentsAreNotClaimedLocalStorage) {
    auto outline = run("struct B{virtual void f();virtual ~B()=default;};struct D:B{void f();};void D::f(){}\n",
                       "frost-missing-override");
    ASSERT_EQ(outline.stringAt("status"), "ready");
    for (const auto& finding : outline.find("findings")->items())
        EXPECT_EQ(finding.find("replacement"), nullptr);
    for (const auto& source :
         std::vector<std::string>{"int* f(int& x){return &x;}\n", "int* f(int* p){int& r=*p;return &r;}\n"}) {
        auto value = run(source, "frost-return-local-address");
        ASSERT_EQ(value.stringAt("status"), "ready");
        EXPECT_TRUE(value.find("findings")->items().empty());
    }
    auto repeated = run("int change();int f(int x){if(x)return 1;else if(x=change();x)return 2;return 0;}\n",
                        "frost-repeated-else-if");
    ASSERT_EQ(repeated.stringAt("status"), "ready");
    EXPECT_TRUE(repeated.find("findings")->items().empty());
}
TEST_F(Analyzer, UnsupportedPointerMutationOptionalSwapConstructorAndLambdaEscapesLoseMustFacts) {
    for (const auto& source : std::vector<std::string>{"void f(){int* p=new int[2];++p;delete p;}\n",
                                                       "void f(){int* p=new int[2];p+=1;delete p;}\n"}) {
        auto value = run(source, "frost-delete-kind");
        ASSERT_EQ(value.stringAt("status"), "ready");
        EXPECT_TRUE(value.find("findings")->items().empty());
    }
    for (const auto& source : std::vector<std::string>{
             "#include <optional>\nint f(){std::optional<int> a;std::optional<int>b{1};a.swap(b);return a.value();}\n",
             "#include <optional>\nstruct Fill{Fill(std::optional<int>& x){x.emplace(1);}};int "
             "f(){std::optional<int>x;Fill fill(x);return x.value();}\n",
             "#include <optional>\nint f(){std::optional<int>x;auto fill=[&x]{x.emplace(1);};fill();return "
             "x.value();}\n",
             "#include <optional>\nint f(){std::optional<int>x;auto fill=[&]{x.emplace(1);};fill();return "
             "x.value();}\n"}) {
        auto value = run(source, "frost-empty-optional");
        ASSERT_EQ(value.stringAt("status"), "ready");
        EXPECT_TRUE(value.find("findings")->items().empty());
    }
}

} // namespace
