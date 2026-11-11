// SPDX-License-Identifier: Apache-2.0
//
// Tests for the C++ inspection framework (inspection.hpp, epic CPP-10):
//   - InspectionProfile toChecksArgument() and effectiveSeverity() with wildcards and disabled rules
//   - InspectionScope file resolution across File, Directory, Target, and Project scopes
//   - SARIF 2.1.0 export and schema validator (valid SARIF acceptance and malformed SARIF rejection)
//   - Real clang-tidy discovery and execution via IProcessLauncher on a C++ translation unit,
//     verifying SARIF structure and exact parity with a direct clang-tidy CLI oracle run.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/inspection.hpp"
#include "frost/sched/resource_governor.hpp"

#include "support/crash_dialogs.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {

namespace fl = frost::lang;
namespace fio = frost::io;

[[nodiscard]] std::string joinChecked(std::string_view base, std::string_view leaf) {
    auto joined = fio::joinPath(base, leaf);
    EXPECT_TRUE(joined.has_value());
    if (!joined) {
        return {};
    }
    auto normalized = fio::normalizePath(*joined);
    EXPECT_TRUE(normalized.has_value());
    return normalized ? std::move(*normalized) : std::string();
}

TEST(InspectionTest, ProfileChecksArgumentAndEffectiveSeverity) {
    fl::InspectionProfile defaultProfile;
    EXPECT_TRUE(defaultProfile.respectProjectClangTidy);
    EXPECT_EQ(defaultProfile.toChecksArgument(), "");
    EXPECT_EQ(defaultProfile.effectiveSeverity("modernize-use-nullptr", fl::DiagnosticSeverity::Warning),
              fl::InspectionSeverity::Warning);
    EXPECT_EQ(defaultProfile.effectiveSeverity("clang-diagnostic-error", fl::DiagnosticSeverity::Error),
              fl::InspectionSeverity::Error);

    fl::InspectionProfile custom;
    custom.name = "StrictModern";
    custom.respectProjectClangTidy = false;
    custom.rules = {
        {.checkPattern = "modernize-*", .severity = fl::InspectionSeverity::Warning, .enabled = true},
        {.checkPattern = "modernize-use-nullptr", .severity = fl::InspectionSeverity::Error, .enabled = true},
        {.checkPattern = "readability-*", .severity = fl::InspectionSeverity::WeakWarning, .enabled = true},
        {.checkPattern = "modernize-use-trailing-return-type",
         .severity = fl::InspectionSeverity::Warning,
         .enabled = false},
        {.checkPattern = "readability-magic-numbers", .severity = fl::InspectionSeverity::Disabled, .enabled = true},
    };

    EXPECT_EQ(custom.toChecksArgument(), "-*,modernize-*,modernize-use-nullptr,readability-*,"
                                         "-modernize-use-trailing-return-type,-readability-magic-numbers");

    EXPECT_EQ(custom.effectiveSeverity("modernize-use-nullptr"), fl::InspectionSeverity::Error);
    EXPECT_EQ(custom.effectiveSeverity("modernize-use-using"), fl::InspectionSeverity::Warning);
    EXPECT_EQ(custom.effectiveSeverity("readability-identifier-naming"), fl::InspectionSeverity::WeakWarning);
    EXPECT_EQ(custom.effectiveSeverity("modernize-use-trailing-return-type"), fl::InspectionSeverity::Disabled);
    EXPECT_EQ(custom.effectiveSeverity("readability-magic-numbers"), fl::InspectionSeverity::Disabled);
    EXPECT_EQ(custom.effectiveSeverity("bugprone-use-after-move", fl::DiagnosticSeverity::Information),
              fl::InspectionSeverity::Info);
}

TEST(InspectionTest, ResolveInspectionScopeFilesAcrossFileDirectoryTargetAndProject) {
#ifdef _WIN32
    const std::string root = "C:/workspace/project";
#else
    const std::string root = "/workspace/project";
#endif
    const std::string coreA = joinChecked(root, "src/core/alpha.cpp");
    const std::string coreB = joinChecked(root, "src/core/beta.cpp");
    const std::string appMain = joinChecked(root, "src/app/main.cpp");

    const std::vector<fl::CompileCommandEntry> commands = {
        {.directory = root,
         .file = coreA,
         .arguments = {"clang++", "-c", coreA, "-o", "src/core/CMakeFiles/frost_core.dir/alpha.cpp.obj"}},
        {.directory = root,
         .file = coreB,
         .arguments = {"clang++", "-c", coreB, "-o", "src/core/CMakeFiles/frost_core.dir/beta.cpp.obj"}},
        {.directory = root,
         .file = appMain,
         .arguments = {"clang++", "-c", appMain, "-o", "src/app/CMakeFiles/frost_app.dir/main.cpp.obj"}},
    };

    // File scope
    {
        const fl::InspectionScope fileScope{
            .kind = fl::InspectionScopeKind::File,
            .paths = {coreB},
            .targetName = {},
        };
        const auto files = fl::resolveInspectionScopeFiles(commands, fileScope);
        ASSERT_EQ(files.size(), 1U);
        EXPECT_EQ(files[0], coreB);
    }

    // Directory scope
    {
        const fl::InspectionScope dirScope{
            .kind = fl::InspectionScopeKind::Directory,
            .paths = {joinChecked(root, "src/core")},
            .targetName = {},
        };
        const auto files = fl::resolveInspectionScopeFiles(commands, dirScope);
        ASSERT_EQ(files.size(), 2U);
        EXPECT_EQ(files[0], coreA);
        EXPECT_EQ(files[1], coreB);
    }

    // Target scope
    {
        const fl::InspectionScope targetScope{
            .kind = fl::InspectionScopeKind::Target,
            .paths = {},
            .targetName = "frost_core",
        };
        const auto files = fl::resolveInspectionScopeFiles(commands, targetScope);
        ASSERT_EQ(files.size(), 2U);
        EXPECT_EQ(files[0], coreA);
        EXPECT_EQ(files[1], coreB);
    }

    // Project scope
    {
        const fl::InspectionScope projectScope{
            .kind = fl::InspectionScopeKind::Project,
            .paths = {},
            .targetName = {},
        };
        const auto files = fl::resolveInspectionScopeFiles(commands, projectScope);
        ASSERT_EQ(files.size(), 3U);
        EXPECT_EQ(files[0], coreA);
        EXPECT_EQ(files[1], coreB);
        EXPECT_EQ(files[2], appMain);
    }
}

TEST(InspectionTest, DirectoryScopeHonorsComponentBoundariesRootsAndDuplicateFiles) {
#ifdef _WIN32
    const std::string root = "C:/";
#else
    const std::string root = "/";
#endif
    const std::string project = joinChecked(root, "workspace/project");
    const std::string source = joinChecked(project, "src/core/alpha.cpp");
    const std::string sibling = joinChecked(project, "src/core-other/beta.cpp");
    const std::vector<fl::CompileCommandEntry> commands = {
        {.directory = project, .file = "src/core/alpha.cpp", .arguments = {}},
        {.directory = project, .file = source, .arguments = {}},
        {.directory = project, .file = sibling, .arguments = {}},
    };
    const fl::InspectionScope directory{
        .kind = fl::InspectionScopeKind::Directory,
        .paths = {joinChecked(project, "src/core") + "/"},
        .targetName = {},
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(commands, directory), std::vector<std::string>{source});

    const fl::InspectionScope rootScope{
        .kind = fl::InspectionScopeKind::Directory,
        .paths = {root},
        .targetName = {},
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(commands, rootScope), (std::vector<std::string>{source, sibling}));
}

#ifdef _WIN32
TEST(InspectionTest, WindowsScopeMatchesMixedSeparatorsAndCaseWithoutChangingReturnedPaths) {
    const std::vector<fl::CompileCommandEntry> commands = {
        {.directory = "C:/Workspace/Project", .file = "Src/Core/Alpha.cpp", .arguments = {}},
        {.directory = "c:\\workspace\\project", .file = "src\\core\\alpha.cpp", .arguments = {}},
    };
    const std::vector<std::string> expected = {"C:\\Workspace\\Project\\Src\\Core\\Alpha.cpp"};
    const fl::InspectionScope file{
        .kind = fl::InspectionScopeKind::File,
        .paths = {"c:/workspace/project/src/core/alpha.cpp"},
        .targetName = {},
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(commands, file), expected);
    const fl::InspectionScope directory{
        .kind = fl::InspectionScopeKind::Directory,
        .paths = {"c:/WORKSPACE/project\\src/core"},
        .targetName = {},
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(commands, directory), expected);
}
#else
TEST(InspectionTest, PosixScopePreservesCaseAndLiteralBackslashes) {
    const std::vector<fl::CompileCommandEntry> commands = {
        {.directory = "/workspace", .file = "Src/alpha.cpp", .arguments = {}},
        {.directory = "/workspace", .file = "src/alpha.cpp", .arguments = {}},
        {.directory = "/workspace", .file = "src\\alpha.cpp", .arguments = {}},
    };
    const fl::InspectionScope file{
        .kind = fl::InspectionScopeKind::Project,
        .paths = {},
        .targetName = {},
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(commands, file),
              (std::vector<std::string>{"/workspace/Src/alpha.cpp", "/workspace/src/alpha.cpp",
                                        "/workspace/src\\alpha.cpp"}));
}
#endif

TEST(InspectionTest, LoadCommandFormDatabasePreservesQuotedArgumentBoundaries) {
    auto& fs = fio::realFileSystem();
    frost::test::ScratchDir scratch("cpp10-command-form");
    const std::string file = joinChecked(scratch.path(), "sample.cpp");
    const fl::Json database =
        fl::Json::array().push(fl::Json::object()
                                   .set("directory", scratch.path())
                                   .set("file", file)
                                   .set("command", "\"clang-cl\" /Yu\"pch.h\" /Fp\"build folder/pch.pch\" "
                                                   "-o \"CMakeFiles/sample.dir/sample.cpp.obj\" -c sample.cpp"));
    ASSERT_TRUE(fs.writeFileAtomic(joinChecked(scratch.path(), "compile_commands.json"), database.dump()).has_value());
    const auto commands = fl::loadCompileCommands(fs, scratch.path());
    ASSERT_TRUE(commands.has_value()) << commands.error().describe();
    ASSERT_EQ(commands->size(), 1U);
    EXPECT_EQ(commands->front().arguments,
              (std::vector<std::string>{"clang-cl", "/Yupch.h", "/Fpbuild folder/pch.pch", "-o",
                                        "CMakeFiles/sample.dir/sample.cpp.obj", "-c", "sample.cpp"}));
    const fl::InspectionScope target{
        .kind = fl::InspectionScopeKind::Target,
        .paths = {},
        .targetName = "sample",
    };
    EXPECT_EQ(fl::resolveInspectionScopeFiles(*commands, target), std::vector<std::string>{file});
}

TEST(InspectionTest, SarifExportAndSchemaValidationRejectsMalformedDocuments) {
#ifdef _WIN32
    const std::string sampleFile = "C:/workspace/sample.cpp";
#else
    const std::string sampleFile = "/workspace/sample.cpp";
#endif
    fl::InspectionReport report;
    report.profileName = "Default";
    report.toolPath = "clang-tidy";
    report.toolVersion = "23.1.4";
    report.commandLine = "clang-tidy -p C:/workspace --quiet C:/workspace/sample.cpp";
    report.inspectedFiles = {sampleFile};
    report.findings = {
        fl::InspectionFinding{
            .file = sampleFile,
            .line = 12,
            .column = 9,
            .endLine = 12,
            .endColumn = 13,
            .ruleId = "modernize-use-nullptr",
            .message = "use nullptr",
            .severity = fl::InspectionSeverity::Error,
            .toolSeverity = fl::DiagnosticSeverity::Warning,
            .fixIt = std::nullopt,
        },
        fl::InspectionFinding{
            .file = sampleFile,
            .line = 20,
            .column = 5,
            .endLine = 20,
            .endColumn = 5,
            .ruleId = "modernize-use-using",
            .message = "use 'using' instead of 'typedef'",
            .severity = fl::InspectionSeverity::Warning,
            .toolSeverity = fl::DiagnosticSeverity::Warning,
            .fixIt = std::nullopt,
        },
    };

    const fl::Json sarif = fl::exportSarifJson(report);
    const auto valid = fl::validateSarifSchema(sarif);
    ASSERT_TRUE(valid.has_value()) << valid.error().describe();

    // 1. Bad version
    {
        fl::Json bad = sarif;
        bad.set("version", "2.0.0");
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
    // 2. Empty $schema
    {
        fl::Json bad = sarif;
        bad.set("$schema", "");
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
    // 3. Empty runs array
    {
        fl::Json bad = sarif;
        bad.set("runs", fl::Json::array());
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
    // 4. Duplicate rule IDs in tool.driver.rules
    {
        fl::Json bad = sarif;
        fl::Json* runs = bad.find("runs");
        ASSERT_NE(runs, nullptr);
        fl::Json run = runs->items()[0];
        fl::Json* driver = run.find("tool")->find("driver");
        fl::Json dupRule = fl::Json::object().set("id", "modernize-use-nullptr");
        driver->find("rules")->push(std::move(dupRule));
        bad.set("runs", fl::Json::array({std::move(run)}));
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
    // 5. Invalid result level enum
    {
        fl::Json bad = sarif;
        fl::Json run = bad.find("runs")->items()[0];
        fl::Json result = run.find("results")->items()[0];
        result.set("level", "fatal");
        run.set("results", fl::Json::array({std::move(result)}));
        bad.set("runs", fl::Json::array({std::move(run)}));
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
    // 6. Invalid region startLine < 1
    {
        fl::Json bad = sarif;
        fl::Json run = bad.find("runs")->items()[0];
        fl::Json result = run.find("results")->items()[0];
        fl::Json location = result.find("locations")->items()[0];
        fl::Json* region = location.find("physicalLocation")->find("region");
        region->set("startLine", 0);
        result.set("locations", fl::Json::array({std::move(location)}));
        run.set("results", fl::Json::array({std::move(result)}));
        bad.set("runs", fl::Json::array({std::move(run)}));
        EXPECT_FALSE(fl::validateSarifSchema(bad).has_value());
    }
}

TEST(InspectionTest, RunClangTidyInspectionAndCompareWithCliOracle) {
    frost::test::disableCrashDialogs();
    auto& fs = fio::realFileSystem();
    auto& launcher = fio::realProcessLauncher();

    auto tidy = fl::findClangTidy(fs, launcher);
    ASSERT_TRUE(tidy.has_value()) << tidy.error().describe();
    EXPECT_TRUE(fio::isAbsolutePath(tidy->path));
    EXPECT_FALSE(tidy->version.empty());

    frost::test::ScratchDir scratch("cpp10-clang-tidy");
    const std::string& dir = scratch.path();
    const std::string srcFile = joinChecked(dir, "sample.cpp");
    const std::string sarifFile = joinChecked(dir, "sample.sarif.json");

    constexpr std::string_view kSource = R"cpp(
typedef int LegacyInt;

int* makeNull() {
    int* p = 0;
    return p;
}
)cpp";
    ASSERT_TRUE(fs.writeFileAtomic(srcFile, kSource).has_value());

    const std::vector<fl::CompileCommandEntry> entries = {
        fl::CompileCommandEntry{
            .directory = dir,
            .file = srcFile,
            .arguments = {"clang++", "-std=c++20", "-c", srcFile, "-o", "sample.o"},
        },
    };
    const std::string ccJson = fl::compileCommandsJson(entries);
    ASSERT_TRUE(fs.writeFileAtomic(joinChecked(dir, "compile_commands.json"), ccJson).has_value());

    frost::sched::ResourceGovernor governor;

    fl::InspectionRequest request;
    request.clangTidyPath = tidy->path;
    request.compileCommandsDir = dir;
    request.scope = fl::InspectionScope{
        .kind = fl::InspectionScopeKind::File,
        .paths = {srcFile},
        .targetName = {},
    };
    request.profile.name = "TestProfile";
    request.profile.respectProjectClangTidy = false;
    request.profile.rules = {
        {.checkPattern = "modernize-use-nullptr", .severity = fl::InspectionSeverity::Error, .enabled = true},
        {.checkPattern = "modernize-use-using", .severity = fl::InspectionSeverity::Warning, .enabled = true},
    };
    request.governor = &governor;

    auto report = fl::runClangTidyInspection(launcher, fs, request);
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    EXPECT_EQ(report->exitCode, 0);
    EXPECT_TRUE(report->executionSuccessful);
    ASSERT_GE(report->findings.size(), 2U);

    bool sawNullptr = false;
    bool sawUsing = false;
    for (const fl::InspectionFinding& f : report->findings) {
        if (f.ruleId == "modernize-use-nullptr") {
            sawNullptr = true;
            EXPECT_EQ(f.severity, fl::InspectionSeverity::Error);
            EXPECT_EQ(f.line, 5U);
        } else if (f.ruleId == "modernize-use-using") {
            sawUsing = true;
            EXPECT_EQ(f.severity, fl::InspectionSeverity::Warning);
            EXPECT_EQ(f.line, 2U);
        }
    }
    EXPECT_TRUE(sawNullptr);
    EXPECT_TRUE(sawUsing);

    // Export SARIF, write to disk, and validate against schema
    ASSERT_TRUE(fl::writeSarifReport(fs, sarifFile, *report).has_value());
    fio::ProcessSpec schemaSpec;
    schemaSpec.executable = FROST_TEST_PYTHON;
    schemaSpec.arguments = {"-I", "-B", FROST_VALIDATE_SARIF, sarifFile};
    schemaSpec.workingDirectory = dir;
    auto schemaRun = fio::runToCompletion(launcher, schemaSpec, {}, std::chrono::seconds{30});
    ASSERT_TRUE(schemaRun.has_value()) << schemaRun.error().describe();
    ASSERT_EQ(schemaRun->exitCode, 0) << schemaRun->stdoutBytes << schemaRun->stderrBytes;
    auto sarifBytes = fs.readFile(sarifFile);
    ASSERT_TRUE(sarifBytes.has_value());
    auto parsedSarif = frost::rpc::parseJson(*sarifBytes);
    ASSERT_TRUE(parsedSarif.has_value());
    ASSERT_TRUE(fl::validateSarifSchema(*parsedSarif).has_value());

    // Direct CLI oracle run using the exact same clang-tidy binary and arguments
    fio::ProcessSpec cliSpec;
    cliSpec.executable = tidy->path;
    cliSpec.arguments = {
        "-p", dir, "--checks=" + request.profile.toChecksArgument(), "--quiet", srcFile,
    };
    cliSpec.workingDirectory = dir;
    auto cliRun = fio::runToCompletion(launcher, cliSpec, {}, std::chrono::minutes{2});
    ASSERT_TRUE(cliRun.has_value()) << cliRun.error().describe();
    EXPECT_EQ(cliRun->exitCode, report->exitCode);

    std::string cliOutput = cliRun->stdoutBytes;
    if (!cliRun->stderrBytes.empty()) {
        if (!cliOutput.empty() && cliOutput.back() != '\n')
            cliOutput += '\n';
        cliOutput += cliRun->stderrBytes;
    }
    const auto cliFindings = fl::parseClangTidyOutput(cliOutput, request.profile);
    EXPECT_EQ(report->findings, cliFindings);

    const fl::Json* sarifResults = parsedSarif->find("runs")->items()[0].find("results");
    ASSERT_NE(sarifResults, nullptr);
    ASSERT_EQ(sarifResults->size(), cliFindings.size());
    for (std::size_t i = 0; i < cliFindings.size(); ++i) {
        const fl::Json& item = sarifResults->items()[i];
        EXPECT_EQ(item.stringAt("ruleId"), cliFindings[i].ruleId);
        EXPECT_EQ(item.find("message")->stringAt("text"), cliFindings[i].message);
        const fl::Json* phys = item.find("locations")->items()[0].find("physicalLocation");
        EXPECT_EQ(phys->find("region")->integerAt("startLine"), static_cast<std::int64_t>(cliFindings[i].line));
        EXPECT_EQ(phys->find("region")->integerAt("startColumn"), static_cast<std::int64_t>(cliFindings[i].column));
    }
}

} // namespace
