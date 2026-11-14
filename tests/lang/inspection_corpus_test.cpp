// SPDX-License-Identifier: Apache-2.0
//
// CPP-10 corpus acceptance against the pinned FrostEngine checkout and the real clang-tidy CLI.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/inspection.hpp"
#include "frost/vcs/git_locator.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

namespace {
namespace fl = frost::lang;
namespace fio = frost::io;

TEST(InspectionCorpus, FrostEngineModuleFindingsMatchRealClangTidyCliAndExportSarif) {
    frost::test::disableCrashDialogs();
    auto& fs = fio::realFileSystem();
    auto& launcher = fio::realProcessLauncher();
    const std::string root = frost::test::environment("FROST_CORPORA_ROOT");
    ASSERT_FALSE(root.empty()) << "-Corpus requires FROST_CORPORA_ROOT";
    constexpr std::string_view sha = FROST_CORPUS_FROSTENGINE_SHA;
    const std::string corpus = fio::joinPath(root, std::format("frostengine-{}", sha.substr(0, 12))).value_or("");
    ASSERT_FALSE(corpus.empty());
    ASSERT_TRUE(fs.status(corpus).has_value()) << "pinned FrostEngine corpus is missing: " << corpus;

    auto git = frost::vcs::findGit(fs, frost::vcs::defaultGitSearch());
    ASSERT_TRUE(git.has_value()) << git.error().describe();
    fio::ProcessSpec headSpec;
    headSpec.executable = git->path;
    headSpec.arguments = {"-C", corpus, "rev-parse", "HEAD"};
    auto head = fio::runToCompletion(launcher, headSpec, {}, std::chrono::seconds{30});
    ASSERT_TRUE(head.has_value()) << head.error().describe();
    std::string actualSha = head->stdoutBytes;
    while (!actualSha.empty() && (actualSha.back() == '\r' || actualSha.back() == '\n' || actualSha.back() == ' ')) {
        actualSha.pop_back();
    }
    ASSERT_EQ(actualSha, sha) << "corpus checkout does not match tests/corpus/corpora.lock.json";

    const std::string buildDir =
        fio::joinPath(corpus, std::format("build/{}", FROST_CORPUS_FROSTENGINE_PRESET)).value_or("");
    const std::string cdb = fio::joinPath(buildDir, "compile_commands.json").value_or("");
    ASSERT_TRUE(fs.status(cdb).has_value()) << "-Corpus requires configured compile_commands.json: " << cdb;
    const std::string module = fio::joinPath(corpus, "Frost/src/Frost/Core/FileSystem/Internal").value_or("");
    auto tidy = fl::findClangTidy(fs, launcher);
    ASSERT_TRUE(tidy.has_value()) << tidy.error().describe();

    fl::InspectionRequest request;
    request.clangTidyPath = tidy->path;
    request.compileCommandsDir = buildDir;
    request.scope = {.kind = fl::InspectionScopeKind::Directory, .paths = {module}, .targetName = {}};
    request.profile.name = "FrostEngineModule";
    request.profile.respectProjectClangTidy = true;
    // The pinned tool's unconfigured defaults contain only clang-diagnostic-*.
    // Select real checks explicitly so this corpus gate exercises inspections
    // even when the corpus has no .clang-tidy. The CLI oracle reuses this profile.
    request.profile.rules = {
        {.checkPattern = "modernize-use-nullptr", .severity = fl::InspectionSeverity::Warning, .enabled = true},
        {.checkPattern = "modernize-use-using", .severity = fl::InspectionSeverity::Warning, .enabled = true},
    };
    request.governor = nullptr;
    auto report = fl::runClangTidyInspection(launcher, fs, request);
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    ASSERT_EQ(report->exitCode, 0) << report->rawStderr;
    ASSERT_FALSE(report->inspectedFiles.empty());

    fio::ProcessSpec oracle;
    oracle.executable = report->toolPath;
    oracle.arguments = report->arguments;
    oracle.workingDirectory = buildDir;
    auto cli = fio::runToCompletion(launcher, oracle, {}, std::chrono::minutes{30});
    ASSERT_TRUE(cli.has_value()) << cli.error().describe();
    ASSERT_EQ(cli->exitCode, report->exitCode) << cli->stderrBytes;
    std::string cliOutput = cli->stdoutBytes;
    if (!cli->stderrBytes.empty()) {
        if (!cliOutput.empty() && cliOutput.back() != '\n')
            cliOutput += '\n';
        cliOutput += cli->stderrBytes;
    }
    EXPECT_EQ(report->findings, fl::parseClangTidyOutput(cliOutput, request.profile));

    frost::test::ScratchDir scratch("cpp10-frostengine-sarif");
    const std::string sarifPath = scratch.join("inspection.sarif.json");
    ASSERT_TRUE(fl::writeSarifReport(fs, sarifPath, *report).has_value());
    fio::ProcessSpec schemaSpec;
    schemaSpec.executable = FROST_TEST_PYTHON;
    schemaSpec.arguments = {"-I", "-B", FROST_VALIDATE_SARIF, sarifPath};
    schemaSpec.workingDirectory = scratch.path();
    auto schema = fio::runToCompletion(launcher, schemaSpec, {}, std::chrono::seconds{30});
    ASSERT_TRUE(schema.has_value()) << schema.error().describe();
    ASSERT_EQ(schema->exitCode, 0) << schema->stdoutBytes << schema->stderrBytes;
}
} // namespace
