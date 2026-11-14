// SPDX-License-Identifier: Apache-2.0
// Authored REF-6/7 real-AST golden/refusal cases. Execution deferred under implementation-first.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/rpc/json.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace {
using frost::rpc::Json;
class AstRefactoring : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"ast-refactoring"};
    Json plan(std::string source, std::string operation, std::string selection, Json order = Json::array(),
              std::string name = "extracted", bool header = false) {
        const auto file = scratch.join(header ? "fixture.hpp" : "fixture.cpp");
        const auto unit = scratch.join("fixture.cpp");
        const auto compiler = *frost::io::normalizePath(FROST_TEST_CXX_COMPILER);
        auto& fs = frost::io::realFileSystem();
        EXPECT_TRUE(fs.writeFileAtomic(file, source));
        if (header)
            EXPECT_TRUE(fs.writeFileAtomic(unit, "#include \"fixture.hpp\"\nint main(){return 0;}\n"));
        const auto offset = source.find(selection);
        EXPECT_NE(offset, std::string::npos);
        std::vector<Json> arguments{Json(compiler)};
#ifdef _WIN32
        for (const auto& arg :
             std::vector<std::string>{"/std:c++20", "/c", unit, "/Fo" + scratch.join("forbidden.obj")})
            arguments.emplace_back(arg);
#else
        for (const auto& arg : std::vector<std::string>{"-std=c++20", "-c", unit, "-o", scratch.join("forbidden.o")})
            arguments.emplace_back(arg);
#endif
        auto database = Json::array({Json::object()
                                         .set("directory", scratch.path())
                                         .set("file", unit)
                                         .set("arguments", Json::array(std::move(arguments)))})
                            .dump();
        auto request = Json::object()
                           .set("projectRoot", scratch.path())
                           .set("seedTranslationUnit", unit)
                           .set("path", file)
                           .set("offset", offset)
                           .set("endOffset", offset + selection.size())
                           .set("operation", operation)
                           .set("newName", name)
                           .set("parameterOrder", std::move(order))
                           .set("compilationDatabase", database)
                           .set("overlays", Json::array());
        frost::io::ProcessSpec spec;
        spec.executable = *frost::io::normalizePath(FROST_CLANG_BINARY);
        spec.arguments = {"--plan-ast-refactoring"};
        spec.workingDirectory = scratch.path();
        spec.stdinMode = frost::io::Stdio::Pipe;
        spec.environment.emplace_back("FROST_LLVM_ROOT",
                                      std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto result =
            frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, request.dump(), std::chrono::minutes(5));
        EXPECT_TRUE(result);
        if (!result)
            return {};
        auto value = frost::rpc::parseJson(result->stdoutBytes);
        EXPECT_TRUE(value) << result->stdoutBytes << result->stderrBytes;
        EXPECT_EQ(fs.readFile(file).value_or("missing"), source);
        EXPECT_FALSE(fs.exists(scratch.join("forbidden.obj")).value_or(true));
        EXPECT_FALSE(fs.exists(scratch.join("forbidden.o")).value_or(true));
        if (value)
            EXPECT_EQ(result->exitCode, value->stringAt("status") == "ready" ? 0 : 1);
        return value ? std::move(*value) : Json{};
    }
    std::string after(const Json& result) {
        const auto* files = result.find("files");
        if (!files || files->items().empty())
            return {};
        return std::string(files->items().front().stringAt("after"));
    }
    void refused(const Json& result) {
        EXPECT_NE(result.stringAt("status"), "ready");
        ASSERT_NE(result.find("files"), nullptr);
        EXPECT_TRUE(result.find("files")->items().empty());
    }
};
TEST_F(AstRefactoring, PermutesCanonicalFunctionParametersAndDirectCalls) {
    auto result = plan("static int sum(int left, int right) { return left-right; }\nint main(){return sum(1,2);}\n",
                       "change-signature", "sum(int", Json::array({Json(1), Json(0)}));
    ASSERT_EQ(result.stringAt("status"), "ready");
    EXPECT_NE(after(result).find("int right, int left"), std::string::npos);
    EXPECT_NE(after(result).find("sum(2, 1)"), std::string::npos);
}
TEST_F(AstRefactoring, InlineNeverDropsUnusedArgumentSideEffects) {
    refused(plan("static int f(int unused){return 1;}\nint main(){int i=0; return f(i++);}\n", "inline-function",
                 "f(i++)"));
}
TEST_F(AstRefactoring, InlineConstantCannotRebindShadowedInitializer) {
    refused(plan("int main(){const int a=2; const int x=a; {const int a=3; return x;}}\n", "inline-variable", "x=a"));
}
TEST_F(AstRefactoring, InlineMacroInitializerCannotBecomeEmptyValueInitialization) {
    refused(plan("#define VALUE 7\nint main(){const int x=VALUE;return x;}\n", "inline-variable", "x=VALUE"));
}
TEST_F(AstRefactoring, InlinePureCallRetainsArithmeticParameterAndReturnConversions) {
    auto result = plan("static int twice(int x){return x+x;}\nint main(){int n=4;return twice(n);}\n",
                       "inline-function", "twice(n)");
    ASSERT_EQ(result.stringAt("status"), "ready");
    EXPECT_NE(after(result).find("static_cast<int>(n)"), std::string::npos);
}
TEST_F(AstRefactoring, ExtractVariableConstantAndMethodProduceRealReparsedImages) {
    for (const std::string operation : {"extract-variable", "extract-constant", "extract-method"}) {
        auto result = plan("static int source(){return 2+3;}\nint main(){return source();}\n", operation, "2+3");
        ASSERT_EQ(result.stringAt("status"), "ready") << operation;
        EXPECT_NE(after(result).find("extracted"), std::string::npos);
    }
}
TEST_F(AstRefactoring, ExtractionRefusesConditionalSubexpressionAndEffectfulExpression) {
    refused(plan("static int source(int x){return x && 10/x;}\n", "extract-variable", "10/x"));
    refused(plan("static int source(int x){return x++;}\n", "extract-variable", "x++"));
}
TEST_F(AstRefactoring, ExtractFieldRequiresAnInternalNonAggregateClass) {
    auto result = plan("namespace {class Box{public:Box(){} int value(){return 2+3;}};}\n", "extract-field", "2+3");
    ASSERT_EQ(result.stringAt("status"), "ready");
    EXPECT_NE(after(result).find("this->extracted"), std::string::npos);
    EXPECT_NE(after(result).find("const int extracted = 2+3"), std::string::npos);
}
TEST_F(AstRefactoring, GenerateMembersUsesActualProjectHeaderAndFieldTypes) {
    auto accessors = plan("struct Data { int count; const int fixed = 2; };\n", "generate-accessors", "Data",
                          Json::array(), "unused", true);
    ASSERT_EQ(accessors.stringAt("status"), "ready");
    EXPECT_NE(after(accessors).find("getCount() const"), std::string::npos);
    EXPECT_NE(after(accessors).find("setCount(int value)"), std::string::npos);
    EXPECT_EQ(after(accessors).find("setFixed"), std::string::npos);
    EXPECT_NE(after(accessors).find("@param value"), std::string::npos);
    auto equality = plan("struct Data { int count; };\n", "generate-equality", "Data", Json::array(), "unused", true);
    ASSERT_EQ(equality.stringAt("status"), "ready");
    EXPECT_NE(after(equality).find("this->count == other.count"), std::string::npos);
    auto formatting =
        plan("struct Data { int count; };\n", "generate-formatting", "Data", Json::array(), "unused", true);
    ASSERT_EQ(formatting.stringAt("status"), "ready");
    EXPECT_NE(after(formatting).find("formatTo(Stream& out) const"), std::string::npos);
}
TEST_F(AstRefactoring, GenerateOverridesUsesBaseSignaturesAndExplicitStubBodies) {
    auto overrides = plan("struct Base { virtual int evaluate(int value) const = 0; }; struct Child : Base {};\n",
                          "generate-overrides", "Child", Json::array(), "unused", true);
    ASSERT_EQ(overrides.stringAt("status"), "ready");
    EXPECT_NE(after(overrides).find("evaluate(int arg0) const override"), std::string::npos);
    EXPECT_NE(after(overrides).find("Unimplemented override: evaluate"), std::string::npos);
    refused(plan("struct Base { virtual int evaluate() noexcept = 0; }; struct Child : Base {};\n",
                 "generate-overrides", "Child", Json::array(), "unused", true));
    refused(plan("struct Data { int count; int getCount() const {return count;} };\n", "generate-accessors", "Data",
                 Json::array(), "unused", true));
}
TEST_F(AstRefactoring, SafeDeleteRemovesOnlyUnreferencedLocalsAndInternalFunctions) {
    auto local = plan("int main(){int unused=2+3; return 0;}\n", "safe-delete", "unused");
    ASSERT_EQ(local.stringAt("status"), "ready");
    EXPECT_EQ(after(local).find("unused"), std::string::npos);
    auto function = plan("static int unused(){return 7;}\nint main(){return 0;}\n", "safe-delete", "unused");
    ASSERT_EQ(function.stringAt("status"), "ready");
    EXPECT_EQ(after(function).find("unused"), std::string::npos);
    refused(plan("int main(){int used=2;return used;}\n", "safe-delete", "used"));
    refused(plan("int effect(); int main(){int unused=effect();return 0;}\n", "safe-delete", "unused"));
    refused(plan("static int used(){return 7;}\nint main(){return used();}\n", "safe-delete", "used"));
    refused(plan("int main(){if(true) int unused; return 0;}\n", "safe-delete", "unused"));
    refused(plan("static int used(int x){return x;}\ntemplate<class T> auto call(T x){return used(x);}\n",
                 "safe-delete", "used"));
}
} // namespace
