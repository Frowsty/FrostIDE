// SPDX-License-Identifier: Apache-2.0
// CPP-13 real -E oracle, include/macro callbacks and changed-input refusal.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/cpp_preprocessor.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <filesystem>
namespace {
TEST(SemanticPreprocessor, ExpandedSourceEqualsPinnedClangEAndRetainsActualEvents) {
    frost::test::ScratchDir scratch("semantic-preprocess");
    auto& fs = frost::io::realFileSystem();
    const auto source = scratch.join("unit.cpp"), header = scratch.join("settings.hpp");
    ASSERT_TRUE(fs.writeFileAtomic(header, "#define ADD(x,y) ((x)+(y))\n"));
    ASSERT_TRUE(
        fs.writeFileAtomic(source, "#include \"settings.hpp\"\n#define TWICE(x) ADD(x,x)\nint value=TWICE(MODE);\n"));
    const auto compiler = frost::io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    using frost::lang::Json;
    std::vector<std::string> arguments{compiler, "--driver-mode=g++", "-std=c++20", "-DMODE=3", "-c", source};
    std::vector<Json> argv;
    for (const auto& value : arguments)
        argv.emplace_back(value);
    const auto database = Json::array({Json::object()
                                           .set("directory", scratch.path())
                                           .set("file", source)
                                           .set("arguments", Json::array(std::move(argv)))})
                              .dump();
    auto request = Json::object()
                       .set("projectRoot", scratch.path())
                       .set("configuration", "test")
                       .set("compilationDatabase", database)
                       .set("commandIndex", 0);
    frost::io::ProcessSpec helper;
    helper.executable = frost::io::normalizePath(FROST_CLANG_BINARY).value();
    helper.arguments = {"--preprocess-unit"};
    helper.workingDirectory = scratch.path();
    helper.stdinMode = frost::io::Stdio::Pipe;
    helper.environment.emplace_back("FROST_LLVM_ROOT",
                                    std::filesystem::path(compiler).parent_path().parent_path().generic_string());
    auto produced =
        frost::io::runToCompletion(frost::io::realProcessLauncher(), helper, request.dump(), std::chrono::minutes(2));
    ASSERT_TRUE(produced);
    ASSERT_EQ(produced->exitCode, 0) << produced->stdoutBytes << produced->stderrBytes;
    auto result = frost::lang::parseCppPreprocessorResult(produced->stdoutBytes);
    ASSERT_TRUE(result) << produced->stdoutBytes;
    frost::io::ProcessSpec oracle;
    oracle.executable = compiler;
    oracle.arguments = {"--driver-mode=g++", "-std=c++20", "-DMODE=3", "-E", "-P", source};
    oracle.workingDirectory = scratch.path();
    auto expected = frost::io::runToCompletion(frost::io::realProcessLauncher(), oracle, "", std::chrono::minutes(2));
    ASSERT_TRUE(expected);
    ASSERT_EQ(expected->exitCode, 0) << expected->stderrBytes;
    EXPECT_EQ(result->text, expected->stdoutBytes);
    bool included = false, expanded = false;
    for (const auto& edge : result->includes.items())
        included |= edge.stringAt("target") == header;
    for (const auto& event : result->macros.items())
        expanded |= event.stringAt("name") == "TWICE";
    EXPECT_TRUE(included);
    EXPECT_TRUE(expanded);
    EXPECT_TRUE(frost::lang::validateCppIndexInputs(fs, result->receipt, {}));
    ASSERT_TRUE(fs.writeFileAtomic(header, "#define ADD(x,y) 0\n"));
    EXPECT_FALSE(frost::lang::validateCppIndexInputs(fs, result->receipt, {}));

    request.set("commandIndex", 1);
    const auto refused =
        frost::io::runToCompletion(frost::io::realProcessLauncher(), helper, request.dump(), std::chrono::minutes(2));
    ASSERT_TRUE(refused);
    EXPECT_NE(refused->exitCode, 0);
    const auto invalid = frost::lang::parseCppPreprocessorResult(refused->stdoutBytes);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().message(), "Command ordinal outside captured database");
}
TEST(SemanticPreprocessor, MalformedAndUnavailableWireResultsRefuse) {
    EXPECT_FALSE(frost::lang::parseCppPreprocessorResult("{}"));
    EXPECT_FALSE(frost::lang::parseCppPreprocessorResult(
        R"({"kind":"frost-preprocessor","schema":1,"status":"unavailable","error":"missing tool"})"));
}
} // namespace
