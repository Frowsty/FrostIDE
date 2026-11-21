// SPDX-License-Identifier: Apache-2.0
// RUN-12 deferred real compiler/profile/export oracle. No result is acceptance evidence until run.
#include "frost/io/path.hpp"
#include "frost/run/native_analysis.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <regex>
namespace frost::run {
TEST(NativeCoverageReal, InstrumentedRunMatchesActualLlvmCovSourceCounts) {
    frost::test::ScratchDir scratch("native-coverage-real");
    auto& fs = io::realFileSystem();
    const auto source = scratch.join("coverage.cpp");
    const auto binary = scratch.join(
#ifdef _WIN32
        "coverage.exe"
#else
        "coverage"
#endif
    );
    ASSERT_TRUE(fs.writeFileAtomic(source, "int cold(){return 9;}\nint hot(){return 0;}\nint main(){return hot();}\n"));
    const auto compiler = io::normalizePath(FROST_TEST_CXX_COMPILER).value();
    const auto tool = [&](std::string name) {
#ifdef _WIN32
        name += ".exe";
#endif
        return io::joinPath(io::parentPath(compiler), name).value();
    };
    io::ProcessSpec compile;
    compile.executable = compiler;
    compile.workingDirectory = scratch.path();
    compile.arguments = {"--driver-mode=g++",  "-std=c++20", "-O0", "-g",  "-fprofile-instr-generate",
                         "-fcoverage-mapping", source,       "-o",  binary};
    auto built = runAnalysisCommand(io::realProcessLauncher(), compile, {});
    ASSERT_TRUE(built) << built.error().describe();
    ASSERT_EQ(built->exitCode, 0) << built->error;
    CoverageRequest request;
    request.llvmCov = tool("llvm-cov");
    request.llvmProfdata = tool("llvm-profdata");
    request.outputDirectory = scratch.join("session");
    request.compilationDirectory = scratch.path();
    request.plan = {binary, {}, scratch.path(), {}};
    auto report = collectNativeCoverage(fs, io::realProcessLauncher(), request);
    ASSERT_TRUE(report) << report.error().describe();
    ASSERT_EQ(report->programExitCode, 0);
    ASSERT_EQ(report->files.size(), 1u);
    ASSERT_EQ(report->files.front().path, source);
    io::ProcessSpec oracle;
    oracle.executable = request.llvmCov;
    oracle.workingDirectory = scratch.path();
    oracle.arguments = {"show",
                        "-format=text",
                        "-show-line-counts-or-regions",
                        "-debuginfod=false",
                        "-instr-profile=" + report->profileDataPath,
                        binary};
    auto shown = runAnalysisCommand(io::realProcessLauncher(), oracle, {});
    ASSERT_TRUE(shown);
    ASSERT_EQ(shown->exitCode, 0) << shown->error;
    EXPECT_TRUE(std::regex_search(shown->output, std::regex(R"(1\|\s*0\|int cold\(\))")));
    EXPECT_TRUE(std::regex_search(shown->output, std::regex(R"(2\|\s*1\|int hot\(\))")));
    const auto& lines = report->files.front().lines;
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines[0], (CoverageLine{1, 0}));
    EXPECT_EQ(lines[1], (CoverageLine{2, 1}));
    ASSERT_TRUE(report->files.front().identity);
#ifdef _WIN32
    const auto raw = fs.listDirectory(request.outputDirectory);
    ASSERT_TRUE(raw);
    request.execute = false;
    for (const auto& entry : *raw)
        if (entry.name.ends_with(".profraw"))
            request.rawProfiles.push_back(io::joinPath(request.outputDirectory, entry.name).value());
    ASSERT_EQ(request.rawProfiles.size(), 1U);
    const auto otherSource = scratch.join("other.cpp");
    const auto otherBinary = scratch.join("other.exe");
    ASSERT_TRUE(fs.writeFileAtomic(otherSource, "int cold(){return 17;}\nint hot(){return 0;}\nint main(){return hot();}\n"));
    compile.arguments[6] = otherSource;
    compile.arguments.back() = otherBinary;
    auto otherBuilt = runAnalysisCommand(io::realProcessLauncher(), compile, {});
    ASSERT_TRUE(otherBuilt) << otherBuilt.error().describe();
    ASSERT_EQ(otherBuilt->exitCode, 0) << otherBuilt->error;
    request.plan.program = otherBinary;
    request.outputDirectory = scratch.join("mismatched-session");
    const auto mismatched = collectNativeCoverage(fs, io::realProcessLauncher(), request);
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error().code(), ErrorCode::Corrupt);
    EXPECT_NE(mismatched.error().message().find("does not name the selected COFF binary"), std::string::npos);
#endif
    ASSERT_TRUE(fs.writeFileAtomic(source, "int edited;\n"));
    EXPECT_FALSE(validateAnalysisIdentity(fs, *report->files.front().identity));
}
} // namespace frost::run
