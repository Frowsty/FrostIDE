// SPDX-License-Identifier: Apache-2.0
// Future real-tool regression oracles. No fixture result is synthesized or skipped.
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/inspection_scope_fixes.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
namespace {
using namespace std::chrono_literals;
using namespace frost;
TEST(TidyFixesDecode, ActualLlvmYamlRetainsOverlappingPrimaryAndAlternativeNoteGroups) {
    const std::string yaml = R"yaml(---
MainSourceFile: '/project/input.cpp'
Diagnostics:
  - DiagnosticName: modernize-use-nullptr
    DiagnosticMessage:
      Message: actual replacement group
      FilePath: '/project/input.cpp'
      FileOffset: 7
      Replacements:
        - { FilePath: '/project/input.cpp', Offset: 7, Length: 1, ReplacementText: nullptr }
        - { FilePath: '/project/input.cpp', Offset: 6, Length: 2, ReplacementText: ' = nullptr' }
    Notes:
      - Message: alternative fix
        FilePath: '/project/input.cpp'
        FileOffset: 7
        Replacements:
          - { FilePath: '/project/input.cpp', Offset: 7, Length: 1, ReplacementText: NULL }
    Level: Warning
    BuildDirectory: '/project'
...
)yaml";
    io::ProcessSpec spec;
    spec.executable = io::normalizePath(FROST_CLANG_BINARY).value();
    spec.arguments = {"--decode-tidy-fixes"};
    spec.killOnClose = true;
    auto output = io::runToCompletion(io::realProcessLauncher(), spec, yaml, 60s);
    ASSERT_TRUE(output) << output.error().describe();
    ASSERT_EQ(output->exitCode, 0) << output->stderrBytes;
    auto body = rpc::parseJson(output->stdoutBytes);
    ASSERT_TRUE(body);
    const auto* diagnostics = body->find("diagnostics");
    ASSERT_NE(diagnostics, nullptr);
    ASSERT_EQ(diagnostics->size(), 1U);
    const auto& diagnostic = diagnostics->items().front();
    EXPECT_EQ(diagnostic.find("primary")->find("replacements")->size(), 2U);
    EXPECT_EQ(diagnostic.find("notes")->items().front().find("replacements")->size(), 1U);
    // A malformed machine report cannot become an empty successful fix set.
    auto invalid =
        io::runToCompletion(io::realProcessLauncher(), spec, "MainSourceFile: [\nDiagnostics: not-a-sequence\n", 60s);
    ASSERT_TRUE(invalid);
    EXPECT_NE(invalid->exitCode, 0);
}
TEST(TidyScopeFixes, RealClangTidyExportsSourceAndDirtyHeaderByteEditsWithoutChangingDisk) {
    test::ScratchDir project{"tidy-scope-project"}, temporary{"tidy-scope-export"};
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    const auto source = project.join("input.cpp"), header = project.join("header.hpp");
    const std::string sourceBefore = "#include \"header.hpp\"\nint* sourcePointer = 0;\n";
    const std::string diskHeader = "inline int* headerPointer = 0;\n";
    const std::string dirtyHeader = "// unsaved prefix changes exported byte offsets\ninline int* headerPointer = 0;\n";
    ASSERT_TRUE(fs.writeFileAtomic(source, sourceBefore));
    ASSERT_TRUE(fs.writeFileAtomic(header, diskHeader));
    std::vector<std::string> argv{io::normalizePath(FROST_TEST_CXX_COMPILER).value()};
#ifdef _WIN32
    argv.insert(argv.end(), {"/std:c++20", "/c", source});
#else
    argv.insert(argv.end(), {"-std=c++20", "-c", source});
#endif
    ASSERT_TRUE(fs.writeFileAtomic(
        project.join("compile_commands.json"),
        lang::compileCommandsJson(std::vector<lang::CompileCommand>{{project.path(), source, argv}})));
    lang::InspectionRequest request;
    request.compileCommandsDir = project.path();
    request.scope.kind = lang::InspectionScopeKind::Project;
    request.profile.respectProjectClangTidy = false;
    request.profile.rules = {{"*", lang::InspectionSeverity::Warning, false},
                             {"modernize-use-nullptr", lang::InspectionSeverity::Warning, true}};
    auto location = lang::findClangTidy(fs, launcher);
    ASSERT_TRUE(location) << location.error().describe();
    request.clangTidyPath = location->path;
    const std::vector<std::pair<std::string, std::string>> buffers{{header, dirtyHeader}};
    auto capture = lang::captureInspectionScope(fs, request, project.path(), buffers);
    ASSERT_TRUE(capture) << capture.error().describe();
    auto proposal = lang::runInspectionScopeFixes(launcher, fs, request, *capture,
                                                  io::normalizePath(FROST_CLANG_BINARY).value(), temporary.path());
    ASSERT_TRUE(proposal) << proposal.error().describe();
    ASSERT_TRUE(proposal->ready());
    ASSERT_EQ(proposal->files.size(), 2U);
    const auto headerFix = std::ranges::find(proposal->files, header, &lang::InspectionFixFile::path);
    ASSERT_NE(headerFix, proposal->files.end());
    EXPECT_TRUE(headerFix->buffer);
    EXPECT_EQ(headerFix->before, dirtyHeader);
    EXPECT_EQ(headerFix->after,
              "// unsaved prefix changes exported byte offsets\ninline int* headerPointer = nullptr;\n");
    const auto sourceFix = std::ranges::find(proposal->files, source, &lang::InspectionFixFile::path);
    ASSERT_NE(sourceFix, proposal->files.end());
    EXPECT_EQ(sourceFix->after, "#include \"header.hpp\"\nint* sourcePointer = nullptr;\n");
    EXPECT_EQ(fs.readFile(source).value(), sourceBefore);
    EXPECT_EQ(fs.readFile(header).value(), diskHeader);
    ASSERT_TRUE(fs.writeFileAtomic(header, "inline int* changedAfterCapture = 0;\n"));
    EXPECT_FALSE(lang::validateInspectionCapture(fs, *capture));
}
} // namespace
