// SPDX-License-Identifier: Apache-2.0
// Authored real recovered-AST golden/refusal/link cases; execution is deferred.
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
class CreateUsage : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"create-from-usage"};
    std::string source = scratch.join("input.cpp"), header = scratch.join("box.hpp");
    std::string compiler = frost::io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    Json proposal(std::string bytes, std::string selection, std::string body, std::string resultType = "",
                  std::string headerBytes = "", bool noExceptions = false, bool secondConfig = false, unsigned caretInside = 0) {
        auto& fs = frost::io::realFileSystem();
        EXPECT_TRUE(fs.writeFileAtomic(source, bytes));
        if (!headerBytes.empty()) EXPECT_TRUE(fs.writeFileAtomic(header, headerBytes));
        std::vector<std::string> args{compiler};
#ifdef _WIN32
        args.insert(args.end(), {"/std:c++20", "/c", source, "/Fo" + scratch.join("must-not-exist.obj")});
        if (noExceptions) args.push_back("/EHs-c-");
#else
        args.insert(args.end(), {"-std=c++20", "-c", source, "-o", scratch.join("must-not-exist.o")});
        if (noExceptions) args.push_back("-fno-exceptions");
#endif
        auto command = [&](const std::vector<std::string>& values) {
            auto argv = Json::array(); for (const auto& value : values) argv.push(value);
            return Json::object().set("directory", scratch.path()).set("file", source).set("arguments", std::move(argv));
        };
        auto commands = Json::array({command(args)});
        if (secondConfig) { args.push_back("-DALTERNATE=1"); commands.push(command(args)); }
        const auto tokenOffset = bytes.find(selection); EXPECT_NE(tokenOffset, std::string::npos);
        const auto offset = tokenOffset + caretInside;
        auto request = Json::object().set("projectRoot", scratch.path()).set("path", source)
            .set("seedTranslationUnit", source).set("compilationDatabase", commands.dump())
            .set("operation", "create-from-usage").set("newName", "unused").set("offset", offset)
            .set("endOffset", offset + selection.size()).set("usageReturnType", std::move(resultType))
            .set("usageBody", std::move(body)).set("overlays", Json::array());
        frost::io::ProcessSpec spec; spec.executable = frost::io::normalizePath(FROST_CLANG_BINARY).value();
        spec.arguments = {"--plan-ast-refactoring"}; spec.workingDirectory = scratch.path(); spec.killOnClose = true;
        spec.environment.emplace_back("FROST_LLVM_ROOT", std::filesystem::path(compiler).parent_path().parent_path().generic_string());
        auto output = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, request.dump(), std::chrono::minutes(5));
        EXPECT_TRUE(output); if (!output) return {};
        auto parsed = frost::rpc::parseJson(output->stdoutBytes); EXPECT_TRUE(parsed) << output->stdoutBytes << output->stderrBytes;
        EXPECT_EQ(fs.readFile(source).value_or("missing"), bytes);
        if (!headerBytes.empty()) EXPECT_EQ(fs.readFile(header).value_or("missing"), headerBytes);
        EXPECT_FALSE(fs.exists(scratch.join("must-not-exist.obj")).value_or(true));
        EXPECT_FALSE(fs.exists(scratch.join("must-not-exist.o")).value_or(true));
        return parsed ? std::move(*parsed) : Json{};
    }
    void refused(const Json& value) {
        EXPECT_NE(value.stringAt("status"), "ready"); const auto* files = value.find("files");
        ASSERT_NE(files, nullptr); EXPECT_TRUE(files->items().empty());
    }
    void linkCandidate(const Json& value, bool noExceptions = false) {
        const auto* files = value.find("files"); ASSERT_NE(files, nullptr);
        for (const auto& file : files->items()) ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(
            std::string(file.stringAt("path")), file.stringAt("after")));
        frost::io::ProcessSpec spec; spec.executable = compiler; spec.workingDirectory = scratch.path(); spec.killOnClose = true;
#ifdef _WIN32
        spec.arguments = {"/std:c++20", source, "/Fe" + scratch.join("acceptance.exe"), "/Fo" + scratch.join("acceptance.obj")};
        if (noExceptions) spec.arguments.push_back("/EHs-c-");
#else
        spec.arguments = {"-std=c++20", source, "-o", scratch.join("acceptance")};
        if (noExceptions) spec.arguments.push_back("-fno-exceptions");
#endif
        auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, "", std::chrono::minutes(2));
        ASSERT_TRUE(result); EXPECT_EQ(result->exitCode, 0) << result->stderrBytes << result->stdoutBytes;
    }
};
TEST_F(CreateUsage, InfersArgumentsAndReturnContextFromRealUnresolvedFreeCallAndLinks) {
    auto value = proposal("int main(){int left=2; int right=3; return combine(left,right)-5;}\n", "combine", "return left + right;", "int");
    ASSERT_EQ(value.stringAt("status"), "ready");
    const auto after = value.find("files")->items().front().stringAt("after");
    EXPECT_NE(after.find("static int combine(int left, int right)"), std::string::npos);
    EXPECT_NE(after.find("@param left"), std::string::npos); EXPECT_NE(after.find("@brief User-supplied implementation of combine"), std::string::npos);
    linkCandidate(value);
}
TEST_F(CreateUsage, DirectReturnContextIsInferredWithoutInventingAResultOrExceptionStub) {
    auto value = proposal("int main(){return answer(42);}\n", "answer", "return arg1;", "", "", true);
    ASSERT_EQ(value.stringAt("status"), "ready");
    EXPECT_NE(value.find("files")->items().front().stringAt("after").find("static int answer(int arg1)"), std::string::npos);
    linkCandidate(value, true);
}
TEST_F(CreateUsage, AddsUsefulPublicInlineMethodToActualCapturedHeaderAndLinksNoExceptionProfile) {
    auto value = proposal("#include \"box.hpp\"\nint main(){const Box box{}; int value=7; return box.read(value)-7;}\n", "read", "return value;", "int", "struct Box {};\n", true);
    ASSERT_EQ(value.stringAt("status"), "ready"); const auto* files = value.find("files"); ASSERT_EQ(files->size(), 1U);
    EXPECT_EQ(files->items().front().stringAt("path"), header);
    const auto after = files->items().front().stringAt("after"); EXPECT_NE(after.find("int read(int value) const"), std::string::npos);
    EXPECT_NE(after.find("public:"), std::string::npos); EXPECT_NE(after.find("return value;"), std::string::npos);
    ASSERT_FALSE(value.find("dependencies")->items().empty()); linkCandidate(value, true);
}

TEST_F(CreateUsage, InteriorIdentifierCaretsSelectRecoveredFreeAndHeaderMemberCalls) {
    auto free = proposal("int main(){return missing(1);}\n", "missing", "return arg1;", "int", "", false, false, 3);
    ASSERT_EQ(free.stringAt("status"), "ready");
    auto member = proposal("#include \"box.hpp\"\nint main(){Box box; return box.missing(1);}\n", "missing", "return arg1;", "int", "struct Box {};\n", false, false, 3);
    ASSERT_EQ(member.stringAt("status"), "ready");
    EXPECT_EQ(member.find("files")->items().front().stringAt("path"), header);
}
TEST_F(CreateUsage, UnrelatedBaselineErrorsMacrosAmbiguousLookupAndUnknownArgumentTypesRefuse) {
    refused(proposal("int main(){int bad=other; return missing(1);}\n", "missing", "return arg1;", "int"));
    refused(proposal("#define CALL missing(1)\nint main(){return CALL;}\n", "CALL", "return arg1;", "int"));
    refused(proposal("int existing(double); int existing(float); int main(){return existing(1);}\n", "existing(1)", "return arg1;", "int"));
    refused(proposal("int main(){return missing(unknown);}\n", "missing", "return arg1;", "int"));
}
TEST_F(CreateUsage, CandidateRequiresAllConfigurationsAndRejectsEscapesMissingBodiesAndUnknownReturn) {
    refused(proposal("int main(){return missing(1);}\n", "missing", "} int injected; {", "int"));
    refused(proposal("int main(){return missing(1);}\n", "missing", "", "int"));
    refused(proposal("int main(){auto x=missing(1); return 0;}\n", "missing", "return arg1;"));
    refused(proposal("#ifdef ALTERNATE\nusing Argument=const char*;\n#else\nusing Argument=int;\n#endif\nint main(){Argument argument{}; return missing(argument);}\n", "missing", "return argument;", "int", "", false, true));
    refused(proposal("int main(){return missing(1);}\n", "missing", "return absentInsideBody;", "int"));
}
}
