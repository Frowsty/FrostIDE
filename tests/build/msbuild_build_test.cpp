// SPDX-License-Identifier: Apache-2.0
//
// Real vswhere and MSBuild (Windows, label "msbuild"): discovery on this machine, and builds of the
// tiny fixture solution (fixtures/tiny_sln: a static library and a program that links it) copied
// into a work directory whose name has a space and a non-ASCII character, directly and through
// BuildService (trust, diagnostics with absolute paths), and the run service's part: MSBuild
// -getProperty for the program and the debugger settings of App.vcxproj.user, then a build and a
// run in the Run console (output, input, exit code). Each build takes about a second. SLN-9 and
// SLN-11 on the synthetic generated solution (tests/project/fixtures/msvc_generated) and the tiny
// one: Compile (Ctrl+F7) of single files and a Rebuild whose error lists must equal what MSBuild's
// own file loggers recorded (msbuild_diagnostic_oracle.hpp), and the .vcxproj.user FrostIDE writes
// as MSBuild and Run read it back. A machine without Visual Studio skips these tests.

#include "frost/build/build_service.hpp"
#include "frost/build/msbuild_driver.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/build/run_service.hpp"
#include "frost/build/run_target.hpp"
#include "frost/build/vcxproj_user.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"
#include "frost/project/project_host.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"

#include "build/msbuild_diagnostic_oracle.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

namespace {

using frost::ErrorCode;
using frost::build::BuildAction;
using frost::build::BuildResult;
using frost::build::Diagnostic;
using frost::build::MsBuildRequest;
using frost::build::Severity;
using frost::build::VsInstance;

constexpr auto kBuildTimeout = std::chrono::seconds(120);

// This machine's MSBuild, or nullopt when no Visual Studio is installed (the test then skips).
std::optional<VsInstance> machineMsBuild() {
    auto found = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!found) {
        if (found.error().code() != ErrorCode::NotFound) {
            ADD_FAILURE() << found.error().describe();
        }
        return std::nullopt;
    }
    return *found;
}

bool copyTree(frost::io::IFileSystem& fs, const std::string& from, const std::string& to) {
    if (!fs.createDirectories(to)) {
        return false;
    }
    auto entries = fs.listDirectory(from);
    if (!entries) {
        return false;
    }
    for (const frost::io::DirEntry& entry : *entries) {
        const std::string source = frost::io::joinPath(from, entry.name).value_or("");
        const std::string target = frost::io::joinPath(to, entry.name).value_or("");
        if (entry.type == frost::io::FileType::Directory) {
            if (!copyTree(fs, source, target)) {
                return false;
            }
            continue;
        }
        auto bytes = fs.readFile(source);
        if (!bytes || !fs.writeFileAtomic(target, *bytes)) {
            return false;
        }
    }
    return true;
}

// A copy of one fixture under this build tree (not %TEMP%, where MSBuild warns with MSB8029),
// removed afterwards. The name holds a space and "é" so paths with both reach every tool.
class WorkDir {
public:
    WorkDir(std::string_view tag, std::string_view fixture, std::string_view fixtures = FROST_BUILD_FIXTURES) {
        static std::atomic<unsigned> counter{0};
        auto root = frost::io::normalizePath(FROST_BUILD_WORK_ROOT);
        auto source = frost::io::normalizePath(std::format("{}/{}", fixtures, fixture));
        if (!root || !source) {
            ADD_FAILURE() << "bad fixture or work directory";
            return;
        }
        m_path = frost::io::joinPath(*root, std::format("{} \xC3\xA9-{}-{}", tag, GetCurrentProcessId(), counter++))
                     .value_or("");
        frost::test::removeTree(frost::io::realFileSystem(), m_path);
        EXPECT_TRUE(copyTree(frost::io::realFileSystem(), *source, m_path)) << m_path;
    }
    ~WorkDir() {
        if (!m_path.empty() && !frost::test::removeTree(frost::io::realFileSystem(), m_path)) {
            ADD_FAILURE() << "could not remove " << m_path;
        }
    }
    WorkDir(const WorkDir&) = delete ("owns the directory");
    WorkDir& operator=(const WorkDir&) = delete ("owns the directory");

    [[nodiscard]] std::string join(std::string_view name) const {
        return frost::io::joinPath(m_path, name).value_or("");
    }
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    [[nodiscard]] bool exists(std::string_view name) const {
        return frost::io::realFileSystem().exists(join(name)).value_or(false);
    }

private:
    std::string m_path;
};

// What one build streamed, for the assertions.
struct Observed {
    frost::Mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> lines FROST_GUARDED_BY(mutex);
    std::vector<Diagnostic> diagnostics FROST_GUARDED_BY(mutex);

    frost::build::BuildCallbacks callbacks() {
        return {.onLine =
                    [this](frost::build::OutputStream, std::string_view line) {
                        {
                            frost::LockGuard lock(mutex);
                            lines.emplace_back(line);
                        }
                        changed.notify_all();
                    },
                .onDiagnostic =
                    [this](const Diagnostic& diagnostic) {
                        frost::LockGuard lock(mutex);
                        diagnostics.push_back(diagnostic);
                    }};
    }

    bool sawLineContaining(std::string_view text) {
        frost::LockGuard lock(mutex);
        return std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); });
    }

    // Waits up to `timeout` for a line containing `text`.
    bool waitForLine(std::string_view text, std::chrono::seconds timeout) {
        frost::UniqueLock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!std::ranges::any_of(lines, [&](const std::string& line) { return line.contains(text); })) {
            if (changed.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
                return false;
            }
        }
        return true;
    }
};

MsBuildRequest requestFor(const VsInstance& msbuild, BuildAction action) {
    return {.msbuildPath = msbuild.msbuildPath,
            .configuration = "Debug",
            .platform = "x64",
            .action = action,
            .trusted = true};
}

// Runs one build to its end; fails the test (and cancels) when it takes longer than kBuildTimeout.
BuildResult runBuild(const MsBuildRequest& request, Observed* observed = nullptr) {
    Observed local;
    Observed& sink = observed != nullptr ? *observed : local;
    auto session = frost::build::startMsBuild(frost::io::realProcessLauncher(), request, sink.callbacks());
    if (!session) {
        ADD_FAILURE() << session.error().describe();
        return {};
    }
    std::optional<BuildResult> result = (*session)->waitFor(kBuildTimeout);
    if (!result) {
        ADD_FAILURE() << "the build did not finish in time";
        (*session)->cancel();
        return (*session)->wait();
    }
    return *result;
}

std::string describe(const BuildResult& result) {
    std::string text =
        std::format("exit {}, {} errors, {} warnings", result.exitCode, result.errorCount, result.warningCount);
    for (const Diagnostic& d : result.diagnostics) {
        text +=
            std::format("\n  {}({},{}): {} {}: {}", d.file, d.line, d.column, toString(d.severity), d.code, d.message);
    }
    return text;
}

bool sameFile(std::string_view a, std::string_view b) {
    auto wa = frost::io::toWide(a);
    auto wb = frost::io::toWide(b);
    return wa && wb &&
           CompareStringOrdinal(wa->data(), static_cast<int>(wa->size()), wb->data(), static_cast<int>(wb->size()),
                                TRUE) == CSTR_EQUAL;
}

struct ProcessEntry {
    DWORD pid = 0;
    DWORD parent = 0;
    std::wstring exe;
};

std::vector<ProcessEntry> processSnapshot() {
    std::vector<ProcessEntry> entries;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return entries;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry)) {
        entries.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    }
    CloseHandle(snapshot);
    return entries;
}

// The live processes below `root` (by parent links in one snapshot), `root` excluded.
std::vector<ProcessEntry> descendantsOf(DWORD root) {
    const std::vector<ProcessEntry> all = processSnapshot();
    std::vector<DWORD> frontier{root};
    std::vector<ProcessEntry> found;
    while (!frontier.empty()) {
        const DWORD parent = frontier.back();
        frontier.pop_back();
        for (const ProcessEntry& entry : all) {
            if (entry.parent == parent && entry.pid != root &&
                std::ranges::none_of(found, [&](const ProcessEntry& seen) { return seen.pid == entry.pid; })) {
                found.push_back(entry);
                frontier.push_back(entry.pid);
            }
        }
    }
    return found;
}

TEST(MsBuildDiscovery, FindsThisMachinesVisualStudio) {
    auto instances = frost::build::listVsInstances(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    if (!instances && instances.error().code() == ErrorCode::NotFound) {
        GTEST_SKIP() << instances.error().describe();
    }
    ASSERT_TRUE(instances.has_value()) << instances.error().describe();
    ASSERT_FALSE(instances->empty());
    for (const VsInstance& instance : *instances) {
        std::cout << "  " << instance.displayName << " " << instance.installationVersion << " (" << instance.instanceId
                  << "): " << instance.msbuildPath << "\n";
    }
    auto chosen = frost::build::selectVsInstance(*instances);
    ASSERT_TRUE(chosen.has_value()) << chosen.error().describe();
    EXPECT_TRUE(frost::io::realFileSystem().exists(chosen->msbuildPath).value_or(false));
    EXPECT_TRUE(chosen->msbuildPath.ends_with("\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe"));
    // VS 2022 (17.x) or later.
    EXPECT_GE(std::stoi(chosen->installationVersion), 17) << chosen->installationVersion;

    // The override hook: a chosen instance by id, and a missing one.
    auto byId = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                          {.instanceId = chosen->instanceId});
    ASSERT_TRUE(byId.has_value()) << byId.error().describe();
    EXPECT_EQ(byId->installationPath, chosen->installationPath);
    auto missing = frost::build::findMsBuild(frost::io::realProcessLauncher(), frost::io::realFileSystem(),
                                             {.instanceId = "no-such-instance"});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(MsBuildBuild, SolutionBuildsCleanRunsAndCleans) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("solution", "tiny_sln");
    MsBuildRequest request = requestFor(*msbuild, BuildAction::Build);
    request.solutionPath = work.join("Tiny.sln");
    Observed observed;
    const BuildResult built = runBuild(request, &observed);
    EXPECT_TRUE(built.success) << describe(built);
    EXPECT_FALSE(built.cancelled);
    EXPECT_EQ(built.exitCode, 0);
    EXPECT_EQ(built.errorCount, 0U);
    EXPECT_EQ(built.warningCount, 0U);
    EXPECT_TRUE(built.diagnostics.empty()) << describe(built);
    EXPECT_GT(built.duration.count(), 0);
    EXPECT_TRUE(work.exists("x64\\Debug\\Lib.lib"));
    ASSERT_TRUE(work.exists("x64\\Debug\\App.exe"));
    // MSBuild's output arrives as UTF-8: the work directory's "é" is intact in the streamed lines.
    EXPECT_TRUE(observed.sawLineContaining("App.vcxproj -> "));
    EXPECT_TRUE(observed.sawLineContaining("\xC3\xA9-"));

    // The program links the library: tinyAnswer() returns 42, so main returns 0.
    frost::io::ProcessSpec app;
    app.executable = work.join("x64\\Debug\\App.exe");
    auto ran = frost::io::runToCompletion(frost::io::realProcessLauncher(), app, {}, std::chrono::seconds(30));
    ASSERT_TRUE(ran.has_value()) << ran.error().describe();
    EXPECT_EQ(ran->exitCode, 0);

    request.action = BuildAction::Clean;
    const BuildResult cleaned = runBuild(request);
    EXPECT_TRUE(cleaned.success) << describe(cleaned);
    EXPECT_FALSE(work.exists("x64\\Debug\\App.exe"));
    EXPECT_FALSE(work.exists("x64\\Debug\\Lib.lib"));
}

TEST(MsBuildBuild, ReportsTheWarningAndTheErrorWithTheirLocations) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("diagnostics", "tiny_sln");
    // Line 6 calls a deprecated function (C4996), line 7 names an undeclared identifier (C2065).
    ASSERT_TRUE(frost::io::realFileSystem()
                    .writeFileAtomic(work.join("App\\main.cpp"), "#include \"lib.h\"\n"
                                                                 "\n"
                                                                 "[[deprecated(\"use tinyAnswer\")]] int oldAnswer();\n"
                                                                 "\n"
                                                                 "int main() {\n"
                                                                 "    int value = oldAnswer();\n"
                                                                 "    return tinyAnswer() + value + undeclared_thing;\n"
                                                                 "}\n")
                    .has_value());
    MsBuildRequest request = requestFor(*msbuild, BuildAction::Rebuild);
    request.solutionPath = work.join("Tiny.sln");
    Observed observed;
    const BuildResult result = runBuild(request, &observed);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.exitCode, 0);
    EXPECT_EQ(result.errorCount, 1U) << describe(result);
    EXPECT_EQ(result.warningCount, 1U) << describe(result);

    const auto byCode = [&](std::string_view code) {
        const auto it = std::ranges::find_if(result.diagnostics, [&](const Diagnostic& d) { return d.code == code; });
        return it == result.diagnostics.end() ? Diagnostic{} : *it;
    };
    const Diagnostic warning = byCode("C4996");
    EXPECT_EQ(warning.severity, Severity::Warning) << describe(result);
    EXPECT_TRUE(sameFile(warning.file, work.join("App\\main.cpp"))) << warning.file;
    EXPECT_EQ(warning.line, 6);
    EXPECT_EQ(warning.column, 17);
    EXPECT_TRUE(warning.message.contains("use tinyAnswer")) << warning.message;
    EXPECT_TRUE(sameFile(warning.project, work.join("App\\App.vcxproj"))) << warning.project;

    const Diagnostic error = byCode("C2065");
    EXPECT_EQ(error.severity, Severity::Error) << describe(result);
    EXPECT_TRUE(sameFile(error.file, work.join("App\\main.cpp"))) << error.file;
    EXPECT_EQ(error.line, 7);
    EXPECT_GT(error.column, 0);
    EXPECT_TRUE(error.message.contains("undeclared_thing")) << error.message;

    frost::LockGuard lock(observed.mutex);
    EXPECT_EQ(observed.diagnostics, result.diagnostics); // Each one also came through onDiagnostic.
}

TEST(MsBuildBuild, OneProjectOfTheSolutionAndAProjectFile) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("project", "tiny_sln");
    // Lib alone, through the solution's "Lib:Rebuild" target: the program is not built.
    MsBuildRequest request = requestFor(*msbuild, BuildAction::Rebuild);
    request.solutionPath = work.join("Tiny.sln");
    request.projectName = "Lib";
    const BuildResult lib = runBuild(request);
    EXPECT_TRUE(lib.success) << describe(lib);
    EXPECT_TRUE(work.exists("x64\\Debug\\Lib.lib"));
    EXPECT_FALSE(work.exists("x64\\Debug\\App.exe"));

    // App.vcxproj itself, with SolutionDir from the solution, so its outputs land beside Lib's.
    request.projectName.clear();
    request.projectPath = work.join("App\\App.vcxproj");
    request.action = BuildAction::Build;
    const BuildResult app = runBuild(request);
    EXPECT_TRUE(app.success) << describe(app);
    EXPECT_TRUE(work.exists("x64\\Debug\\App.exe"));
}

TEST(MsBuildBuild, BuildsAnSlnx) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("slnx", "tiny_sln");
    MsBuildRequest request = requestFor(*msbuild, BuildAction::Rebuild);
    request.solutionPath = work.join("Tiny.slnx");
    const BuildResult result = runBuild(request);
    EXPECT_TRUE(result.success) << describe(result);
    EXPECT_TRUE(work.exists("x64\\Debug\\App.exe"));
}

TEST(MsBuildBuild, CancelEndsTheWholeProcessTree) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("cancel", "slow");
    MsBuildRequest request = requestFor(*msbuild, BuildAction::Build);
    request.projectPath = work.join("Slow.proj");
    Observed observed;
    const auto started = std::chrono::steady_clock::now();
    auto session = frost::build::startMsBuild(frost::io::realProcessLauncher(), request, observed.callbacks());
    ASSERT_TRUE(session.has_value()) << session.error().describe();
    ASSERT_TRUE(observed.waitForLine("frost-slow-build-started", std::chrono::seconds(60)));

    // Wait until ping.exe runs below MSBuild, then hold a handle to every process of the tree.
    std::vector<ProcessEntry> tree;
    for (int i = 0; i < 300; ++i) {
        tree = descendantsOf((*session)->processId());
        if (std::ranges::any_of(tree,
                                [](const ProcessEntry& e) { return _wcsicmp(e.exe.c_str(), L"PING.EXE") == 0; })) {
            break;
        }
        Sleep(50);
    }
    ASSERT_TRUE(std::ranges::any_of(tree, [](const ProcessEntry& e) {
        return _wcsicmp(e.exe.c_str(), L"PING.EXE") == 0;
    })) << "ping.exe never started";
    tree.push_back({(*session)->processId(), 0, L"MSBuild.exe"});
    std::vector<HANDLE> handles;
    for (const ProcessEntry& entry : tree) {
        if (HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, entry.pid); handle != nullptr) {
            handles.push_back(handle);
        }
    }
    EXPECT_GE(handles.size(), 3U); // MSBuild, cmd.exe and ping.exe at least.

    (*session)->cancel();
    const std::optional<BuildResult> result = (*session)->waitFor(std::chrono::seconds(30));
    ASSERT_TRUE(result.has_value()) << "the cancelled build did not end";
    EXPECT_TRUE(result->cancelled);
    EXPECT_FALSE(result->success);
    EXPECT_EQ(result->exitCode, frost::io::kTerminatedExitCode);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60)); // Not ping's 120 s.
    for (HANDLE handle : handles) {
        EXPECT_EQ(WaitForSingleObject(handle, 10'000), WAIT_OBJECT_0) << "a process of the build outlived the cancel";
        CloseHandle(handle);
    }
}

// The warning and the error of the diagnostics test, now through BuildService: the untrusted copy
// waits for trust, then MSBuild runs and both diagnostics arrive with absolute, jumpable paths.
TEST(MsBuildBuild, BuildServiceBuildsAndResolvesTheDiagnostics) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("service", "tiny_sln");
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    const std::string mainPath = work.join("App\\main.cpp");
    ASSERT_TRUE(fs.writeFileAtomic(mainPath, "#include \"lib.h\"\n"
                                             "\n"
                                             "[[deprecated(\"use tinyAnswer\")]] int oldAnswer();\n"
                                             "\n"
                                             "int main() {\n"
                                             "    int value = oldAnswer();\n"
                                             "    return tinyAnswer() + value + undeclared_thing;\n"
                                             "}\n")
                    .has_value());
    auto loaded = frost::project::SolutionModel::load(fs, work.join("Tiny.sln"));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    auto solution = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
    auto store = frost::project::TrustStore::open(fs, work.join("data\\trusted-roots.txt"));
    ASSERT_TRUE(store.has_value()) << store.error().describe();

    auto service = frost::build::BuildService::create(
        fs, frost::io::realProcessLauncher(), **store,
        {.stateFilePath = work.join("data\\active-configurations.txt"), .msbuildPath = msbuild->msbuildPath});
    ASSERT_TRUE(service->openSolution(solution).has_value());
    EXPECT_EQ(service->activeConfiguration()->name(), "Debug|x64");
    ASSERT_TRUE(service->build(BuildAction::Rebuild).has_value());
    EXPECT_EQ(service->state(), frost::build::BuildState::NeedsTrust);
    const std::vector<std::string> roots = service->untrustedRoots();
    ASSERT_EQ(roots.size(), 1U);
    EXPECT_TRUE(sameFile(roots[0], solution->directory())) << roots[0];
    ASSERT_TRUE(service->trust(roots[0]).has_value());
    ASSERT_TRUE(service->waitForIdle(kBuildTimeout)) << "the build did not finish in time";

    const std::optional<frost::build::BuildOutcome> outcome = service->lastOutcome();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->succeeded());
    EXPECT_FALSE(outcome->error.has_value()) << outcome->error->describe();
    EXPECT_FALSE(outcome->result.cancelled);
    EXPECT_EQ(outcome->counts.errors, 1U) << describe(outcome->result);
    EXPECT_EQ(outcome->counts.warnings, 1U) << describe(outcome->result);

    const std::vector<frost::build::BuildDiagnostic> forMain = service->diagnosticsForDocument(mainPath);
    ASSERT_EQ(forMain.size(), 2U) << describe(outcome->result);
    for (const frost::build::BuildDiagnostic& d : forMain) {
        EXPECT_TRUE(sameFile(d.path, mainPath)) << d.path;
        EXPECT_TRUE(frost::io::isAbsolutePath(d.path)) << d.path;
        EXPECT_TRUE(d.jumpable) << d.diagnostic.code;
    }
    // cl's order of the two is its own; compare them by code.
    const auto lineOf = [&forMain](std::string_view code) {
        const auto it = std::ranges::find_if(
            forMain, [code](const frost::build::BuildDiagnostic& d) { return d.diagnostic.code == code; });
        return it == forMain.end() ? -1 : it->diagnostic.line;
    };
    EXPECT_EQ(lineOf("C4996"), 6);
    EXPECT_EQ(lineOf("C2065"), 7);
    const std::vector<frost::build::DiagnosticFileGroup> groups = service->diagnosticsByFile();
    ASSERT_FALSE(groups.empty());
    EXPECT_TRUE(sameFile(groups[0].file, mainPath)) << groups[0].file;
    service.reset();
}

// What MSBuild reports for the tiny program, with the settings of its App.vcxproj.user.
TEST(MsBuildRun, GetPropertyReportsTheProgramAndTheUserFilesSettings) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("properties", "tiny_sln");
    frost::build::RunPropertyRequest request{.msbuildPath = msbuild->msbuildPath,
                                             .projectPath = work.join("App\\App.vcxproj"),
                                             .solutionPath = work.join("Tiny.sln"),
                                             .configuration = "Debug",
                                             .platform = "x64"};
    auto properties = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kBuildTimeout);
    ASSERT_TRUE(properties.has_value()) << properties.error().describe();
    EXPECT_TRUE(properties->fromMsBuild);
    EXPECT_TRUE(sameFile(properties->targetPath, work.join("x64\\Debug\\App.exe"))) << properties->targetPath;
    EXPECT_TRUE(sameFile(properties->projectDir, work.join("App\\"))) << properties->projectDir;
    EXPECT_EQ(properties->configurationType, "Application");
    EXPECT_EQ(properties->commandArguments, "--print \"two words\" --echo --exit 3");
    EXPECT_TRUE(sameFile(properties->workingDirectory, work.join("Lib"))) << "MSBuild expands $(SolutionDir)";
    EXPECT_TRUE(properties->environment.contains("FROST_TINY_VALUE=hello from Debug")) << properties->environment;
    EXPECT_TRUE(properties->mergeEnvironment);

    const auto command = frost::build::resolveRunCommand(*properties, frost::build::runMacros(*properties, request));
    ASSERT_TRUE(command.has_value()) << command.error().describe();
    EXPECT_TRUE(sameFile(command->executable, properties->targetPath));
    EXPECT_TRUE(sameFile(command->workingDirectory, work.join("Lib"))) << command->workingDirectory;
    using Pairs = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(command->environment, (Pairs{{"FROST_TINY_VALUE", "hello from Debug"}, {"FROST_TINY_OTHER", "1"}}));

    // The static fallback finds the same program and arguments (it cannot expand imported macros).
    const auto fallback = frost::build::staticRunProperties(frost::io::realFileSystem(), request);
    ASSERT_TRUE(fallback.has_value()) << fallback.error().describe();
    EXPECT_FALSE(fallback->fromMsBuild);
    EXPECT_TRUE(sameFile(fallback->targetPath, properties->targetPath)) << fallback->targetPath;
    EXPECT_EQ(fallback->commandArguments, properties->commandArguments);

    // The user file sets Debug|x64 only.
    request.configuration = "Release";
    const auto release = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kBuildTimeout);
    ASSERT_TRUE(release.has_value()) << release.error().describe();
    EXPECT_TRUE(sameFile(release->targetPath, work.join("x64\\Release\\App.exe"))) << release->targetPath;
    EXPECT_TRUE(release->commandArguments.empty()) << release->commandArguments;
}

// Run through RunService over the real BuildService: build, then the program in the Run console
// with the user file's arguments, working directory and environment, a line of input, exit code 3.
TEST(MsBuildRun, RunServiceBuildsAndRunsTheStartupProject) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("run", "tiny_sln");
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    auto loaded = frost::project::SolutionModel::load(fs, work.join("Tiny.sln"));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    auto solution = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
    auto store = frost::project::TrustStore::open(fs, work.join("data\\trusted-roots.txt"));
    ASSERT_TRUE(store.has_value()) << store.error().describe();
    ASSERT_TRUE((*store)->trust(solution->directory()).has_value());

    auto buildService = frost::build::BuildService::create(
        fs, frost::io::realProcessLauncher(), **store,
        {.stateFilePath = work.join("data\\active-configurations.txt"), .msbuildPath = msbuild->msbuildPath});
    ASSERT_TRUE(buildService->openSolution(solution).has_value());
    const auto builder = frost::build::makeRunBuilder(*buildService);

    frost::Mutex mutex;
    std::string out;
    std::string err;
    std::string system;
    auto service = frost::build::RunService::create(
        fs, frost::io::realProcessLauncher(), **store, *builder,
        {.stateFilePath = work.join("data\\run-settings.txt"),
         .msbuildPath = msbuild->msbuildPath,
         .externalConsoleByDefault = false},
        {.onOutput = [&](frost::build::RunId, frost::build::RunOutputKind kind, std::string_view text) {
            frost::LockGuard lock(mutex);
            (kind == frost::build::RunOutputKind::Stdout   ? out
             : kind == frost::build::RunOutputKind::Stderr ? err
                                                           : system) += text;
        }});
    ASSERT_TRUE(service->openSolution(solution).has_value());
    ASSERT_TRUE(service->startupProject().has_value());
    EXPECT_EQ(solution->project(*service->startupProject()).name, "App") << "Lib is a library";

    auto run = service->run();
    ASSERT_TRUE(run.has_value()) << run.error().describe();
    const auto deadline = std::chrono::steady_clock::now() + kBuildTimeout;
    while (true) {
        const auto info = service->runInfo(*run);
        if (!info || info->state == frost::build::RunState::Running ||
            info->state == frost::build::RunState::Finished || std::chrono::steady_clock::now() > deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(service->sendInput(*run, "hi there\n").has_value());
    ASSERT_TRUE(service->waitForIdle(kBuildTimeout)) << "the run did not finish in time";
    const auto outcome = service->lastOutcome();
    ASSERT_TRUE(outcome.has_value());
    frost::LockGuard lock(mutex);
    std::erase(out, '\r'); // The program's C runtime writes text-mode CRLF; the Run console shows lines.
    ASSERT_TRUE(outcome->ran()) << (outcome->error ? outcome->error->describe() : "") << "\n" << system;
    EXPECT_EQ(outcome->exitCode, 3) << out;
    EXPECT_FALSE(outcome->stopped);
    EXPECT_EQ(outcome->run.mode, frost::build::ConsoleMode::RunConsole);
    EXPECT_GT(outcome->duration.count(), 0);
    EXPECT_TRUE(out.contains("tiny answer 42\n")) << out;
    EXPECT_TRUE(out.contains("arg 1: --print\narg 2: two words\narg 3: --echo\n")) << out;
    EXPECT_TRUE(out.contains("FROST_TINY_VALUE=hello from Debug\n")) << out;
    EXPECT_TRUE(out.contains("\\Lib\n")) << "the working directory is the user file's: " << out;
    EXPECT_TRUE(out.contains("echo: hi there\n")) << out;
    EXPECT_TRUE(err.contains("tiny stderr line")) << err;
    EXPECT_TRUE(system.contains("App.exe")) << system;
    service.reset();
    EXPECT_TRUE(buildService->waitForIdle(kBuildTimeout));
}

// ---- SLN-9: Ctrl+F7 and the error list against MSBuild's own log; SLN-11: the .vcxproj.user writer.

// The first file named `name` (ASCII case ignored) below `directory`, or "".
std::string findFileBelow(const std::string& directory, std::string_view name) {
    auto entries = frost::io::realFileSystem().listDirectory(directory);
    if (!entries) {
        return {};
    }
    for (const frost::io::DirEntry& entry : *entries) {
        const std::string path = frost::io::joinPath(directory, entry.name).value_or("");
        if (entry.type == frost::io::FileType::Directory) {
            if (std::string found = findFileBelow(path, name); !found.empty()) {
                return found;
            }
        } else if (_stricmp(entry.name.c_str(), std::string(name).c_str()) == 0) {
            return path;
        }
    }
    return {};
}

// The Solution* properties Visual Studio passes when it builds one project of `solution`, spelled
// out here (not taken from frost::build) for the oracle's own command line.
std::vector<std::string> solutionProperties(const WorkDir& work, std::string_view solutionFile) {
    const std::string name(solutionFile.substr(0, solutionFile.rfind('.')));
    return {"-p:SolutionDir=" + work.path() + "\\", "-p:SolutionPath=" + work.join(solutionFile),
            "-p:SolutionName=" + name, "-p:SolutionFileName=" + std::string(solutionFile),
            "-p:SolutionExt=" + std::string(solutionFile.substr(solutionFile.rfind('.')))};
}

// MSBuild's own evaluation of `project` (MSBuild.exe -getItem, the workbench's fallback path of
// SLN-2) as the solution model takes it, so items from property sheets are files of the project.
std::optional<frost::project::EvaluatedProjectItems> evaluateProject(const VsInstance& msbuild, std::string project,
                                                                     std::string solution) {
    const frost::project::ProjectEvaluationRequest request{.projectPath = std::move(project),
                                                           .configuration = "Debug",
                                                           .platform = "x64",
                                                           .solutionPath = std::move(solution)};
    auto spec = frost::project::msbuildCliEvaluationSpec(msbuild.msbuildPath, request);
    EXPECT_TRUE(spec.has_value()) << spec.error().describe();
    if (!spec) {
        return std::nullopt;
    }
    auto ran = frost::io::runToCompletion(frost::io::realProcessLauncher(), *spec, {}, kBuildTimeout);
    EXPECT_TRUE(ran.has_value() && ran->exitCode == 0) << (ran ? ran->stdoutBytes : ran.error().describe());
    if (!ran || ran->exitCode != 0) {
        return std::nullopt;
    }
    auto evaluation = frost::project::parseMsBuildCliEvaluation(ran->stdoutBytes, request);
    EXPECT_TRUE(evaluation.has_value()) << evaluation.error().describe();
    if (!evaluation) {
        return std::nullopt;
    }
    return frost::project::toEvaluatedProjectItems(*evaluation, frost::project::EvaluationSource::MsBuildCli,
                                                   msbuild.installationVersion);
}

// A BuildService over a solution of `work`, with this machine's MSBuild; the work directory is
// trusted. `evaluated` replaces those projects' static items, as the workbench does after SLN-2.
struct ServiceRig {
    ServiceRig(const WorkDir& work, std::string_view solutionFile, const VsInstance& msbuild,
               std::vector<frost::project::EvaluatedProjectItems> evaluated = {}) {
        frost::io::IFileSystem& fs = frost::io::realFileSystem();
        auto loaded =
            frost::project::SolutionModel::load(fs, work.join(solutionFile), {.evaluated = std::move(evaluated)});
        EXPECT_TRUE(loaded.has_value()) << loaded.error().describe();
        if (!loaded) {
            return;
        }
        solution = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
        auto opened = frost::project::TrustStore::open(fs, work.join("data\\trusted-roots.txt"));
        EXPECT_TRUE(opened.has_value());
        store = std::move(*opened);
        EXPECT_TRUE(store->trust(solution->directory()).has_value());
        service = frost::build::BuildService::create(
            fs, frost::io::realProcessLauncher(), *store,
            {.stateFilePath = work.join("data\\active-configurations.txt"), .msbuildPath = msbuild.msbuildPath});
        EXPECT_TRUE(service->openSolution(solution).has_value());
    }
    ~ServiceRig() { service.reset(); }
    ServiceRig(const ServiceRig&) = delete ("owns the service");
    ServiceRig& operator=(const ServiceRig&) = delete ("owns the service");

    // Waits for the build to end and returns its outcome.
    frost::build::BuildOutcome finished() const {
        EXPECT_TRUE(service->waitForIdle(kBuildTimeout)) << "the build did not finish in time";
        return service->lastOutcome().value_or(frost::build::BuildOutcome{});
    }

    std::shared_ptr<const frost::project::SolutionModel> solution;
    std::unique_ptr<frost::project::TrustStore> store;
    std::unique_ptr<frost::build::BuildService> service;
};

// The synthetic generated solution (tests/project/fixtures/msvc_generated) with one planted error
// and warning in App and a warning in Core's nested src\b.cpp: Ctrl+F7 compiles exactly the file
// (its Identity from MSBuild's evaluation), builds no referenced project, and the error list equals
// what MSBuild's own file loggers record for the same compile.
TEST(MsBuildCompile, CompilesOnlyTheFileAndTheErrorListEqualsMsBuildsLog) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("compile", "msvc_generated", FROST_PROJECT_FIXTURES);
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    const std::string app = work.join("App\\app.cpp");
    const std::string b = work.join("Core\\src\\b.cpp");
    // C4005 is a level-1 warning: the fixture keeps MSBuild's default warning level.
    ASSERT_TRUE(fs.writeFileAtomic(app, "#define GEN_PLANTED 1\n#define GEN_PLANTED 2\n"
                                        "int app() { return GEN_PLANTED + missing_symbol; }\n")
                    .has_value());
    ASSERT_TRUE(fs.writeFileAtomic(b, "#define GEN_PLANTED 1\n#define GEN_PLANTED 2\n"
                                      "int coreB() { return GEN_PLANTED; }\n")
                    .has_value());
    const auto core = evaluateProject(*msbuild, work.join("Core\\Core.vcxproj"), work.join("Generated.sln"));
    ASSERT_TRUE(core.has_value());
    ServiceRig rig(work, "Generated.sln", *msbuild, {*core});
    ASSERT_NE(rig.service, nullptr);
    ASSERT_EQ(rig.service->activeConfiguration()->name(), "Debug|x64");

    struct Case {
        std::string file;
        std::string project;
        std::string identity; // As the fixture's project file writes it: the oracle's own SelectedFiles.
        std::string object;   // Produced unless the file has an error.
        bool compiles = true;
    };
    const std::vector<Case> cases = {
        {app, "App\\App.vcxproj", "app.cpp", "app.obj", false},
        {b, "Core\\Core.vcxproj", "src\\b.cpp", "b.obj", true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        const auto project = rig.service->compileProjectFor(c.file);
        ASSERT_TRUE(project.has_value());
        EXPECT_TRUE(sameFile(rig.solution->project(*project).path, work.join(c.project)));
        ASSERT_TRUE(rig.service->compile(c.file).has_value());
        const frost::build::BuildOutcome outcome = rig.finished();
        EXPECT_FALSE(outcome.error.has_value()) << outcome.error->describe();
        EXPECT_EQ(outcome.target.action, BuildAction::Compile);

        // The oracle compiles the same file again: rewriting it makes MSBuild's up-to-date check
        // compile it (and log its diagnostics) rather than skip it.
        const std::string source = fs.readFile(c.file).value_or("");
        ASSERT_TRUE(fs.writeFileAtomic(c.file, source).has_value());
        std::vector<std::string> arguments = {work.join(c.project), "-t:ClCompile", "-p:SelectedFiles=" + c.identity,
                                              "-p:Configuration=Debug", "-p:Platform=x64"};
        for (std::string& property : solutionProperties(work, "Generated.sln")) {
            arguments.push_back(std::move(property));
        }
        const auto oracle = frost::test::runMsBuildOracle(
            msbuild->msbuildPath, arguments, std::string(frost::io::parentPath(work.join(c.project))), work.path());
        ASSERT_TRUE(oracle.has_value());
        EXPECT_FALSE(oracle->diagnostics.empty());
        EXPECT_EQ(outcome.result.success, oracle->exitCode == 0);
        const std::vector<frost::build::BuildDiagnostic> shown = rig.service->diagnostics();
        EXPECT_EQ(frost::test::errorListRows(shown), oracle->diagnostics)
            << "error list:" << frost::test::describe(frost::test::errorListRows(shown))
            << "\nMSBuild logged:" << frost::test::describe(oracle->diagnostics);
        EXPECT_EQ(outcome.counts.errors + outcome.counts.warnings, oracle->diagnostics.size());
        EXPECT_EQ(findFileBelow(work.path(), c.object).empty(), !c.compiles) << c.object;
    }
    // Only the selected files were compiled, and no referenced project was built.
    for (const std::string_view other : {"main.obj", "a.obj", "c.obj", "renderer.obj", "Core.lib", "Renderer.lib"}) {
        EXPECT_TRUE(findFileBelow(work.path(), other).empty()) << other << " should not exist";
    }

    // A property sheet's item (Identity = its full path) compiles too; a header compiles nothing.
    const std::string shared = work.join("shared\\deep_shared.cpp");
    ASSERT_TRUE(rig.service->compileProjectFor(shared).has_value());
    ASSERT_TRUE(rig.service->compile(shared).has_value());
    const frost::build::BuildOutcome sharedOutcome = rig.finished();
    EXPECT_TRUE(sharedOutcome.succeeded())
        << describe(sharedOutcome.result) << (sharedOutcome.error ? sharedOutcome.error->describe() : "");
    EXPECT_FALSE(findFileBelow(work.path(), "deep_shared.obj").empty());
    EXPECT_TRUE(findFileBelow(work.path(), "a.obj").empty());
    EXPECT_FALSE(rig.service->compileProjectFor(work.join("App\\app.h")).has_value());
    EXPECT_EQ(rig.service->compile(work.join("App\\app.h")).error().code(), ErrorCode::NotFound);
}

// Rebuild of the whole synthetic solution with diagnostics in three projects and MSBuild's own
// MSB8065 (the shader's custom build step): the error list equals MSBuild's file-logger record.
TEST(MsBuildBuild, RebuildErrorListEqualsMsBuildsLog) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("error-list", "msvc_generated", FROST_PROJECT_FIXTURES);
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    // C4005 is a level-1 warning: the fixture keeps MSBuild's default warning level.
    ASSERT_TRUE(fs.writeFileAtomic(work.join("Core\\src\\b.cpp"), "#define GEN_PLANTED 1\n#define GEN_PLANTED 2\n"
                                                                  "int coreB() { return GEN_PLANTED; }\n")
                    .has_value());
    ASSERT_TRUE(fs.writeFileAtomic(work.join("Renderer\\renderer.cpp"), "#define GEN_RENDER 1\n#define GEN_RENDER 2\n"
                                                                        "int renderer() { return GEN_RENDER; }\n")
                    .has_value());
    ASSERT_TRUE(fs.writeFileAtomic(work.join("App\\app.cpp"), "int app() { return missing_symbol + also_missing; }\n")
                    .has_value());
    ServiceRig rig(work, "Generated.sln", *msbuild);
    ASSERT_NE(rig.service, nullptr);
    ASSERT_TRUE(rig.service->build(BuildAction::Rebuild).has_value());
    const frost::build::BuildOutcome outcome = rig.finished();
    EXPECT_FALSE(outcome.error.has_value());

    const auto oracle = frost::test::runMsBuildOracle(
        msbuild->msbuildPath, {work.join("Generated.sln"), "-t:Rebuild", "-p:Configuration=Debug", "-p:Platform=x64"},
        work.path(), work.path());
    ASSERT_TRUE(oracle.has_value());
    EXPECT_NE(oracle->exitCode, 0);
    EXPECT_FALSE(outcome.succeeded());
    const std::set<frost::test::LoggedDiagnostic> shown = frost::test::errorListRows(rig.service->diagnostics());
    EXPECT_EQ(shown, oracle->diagnostics) << "error list:" << frost::test::describe(shown)
                                          << "\nMSBuild logged:" << frost::test::describe(oracle->diagnostics);
    // The record has more than cl's lines: MSBuild's own warning for the shader step is in it too.
    EXPECT_TRUE(std::ranges::any_of(oracle->diagnostics, [](const frost::test::LoggedDiagnostic& d) {
        return d.code == "C4005";
    })) << frost::test::describe(oracle->diagnostics);
    EXPECT_TRUE(std::ranges::any_of(oracle->diagnostics, [](const frost::test::LoggedDiagnostic& d) {
        return d.code.starts_with("MSB");
    })) << frost::test::describe(oracle->diagnostics);
}

// SLN-11: the .vcxproj.user FrostIDE writes is what MSBuild evaluates (-getProperty, the oracle) for
// each configuration, and Run starts the program with it: arguments, working directory, environment.
TEST(MsBuildRun, TheUserFileFrostIdeWritesIsWhatMsBuildAndRunUse) {
    const auto msbuild = machineMsBuild();
    if (!msbuild) {
        GTEST_SKIP() << "no Visual Studio with MSBuild";
    }
    WorkDir work("user-file", "tiny_sln");
    frost::io::IFileSystem& fs = frost::io::realFileSystem();
    const std::string project = work.join("App\\App.vcxproj");
    ASSERT_TRUE(frost::build::writeUserFileProperties(
                    fs, project, "Debug", "x64",
                    {{"LocalDebuggerCommandArguments", "--print \"from frost\" --exit 5"},
                     {"LocalDebuggerWorkingDirectory", "$(SolutionDir)x64"},
                     {"LocalDebuggerEnvironment", "FROST_TINY_VALUE=written by $(ProjectName)"}})
                    .has_value());
    ASSERT_TRUE(frost::build::writeUserFileProperties(fs, project, "Release", "x64",
                                                      {{"LocalDebuggerCommandArguments", "--release"}})
                    .has_value());
    const std::string user = fs.readFile(project + ".user").value_or("");
    EXPECT_TRUE(user.contains("<!-- SPDX-License-Identifier: Apache-2.0 -->")) << "the rest of the file stays";
    EXPECT_FALSE(user.contains("two words")) << user;

    frost::build::RunPropertyRequest request{.msbuildPath = msbuild->msbuildPath,
                                             .projectPath = project,
                                             .solutionPath = work.join("Tiny.sln"),
                                             .configuration = "Debug",
                                             .platform = "x64"};
    auto debug = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kBuildTimeout);
    ASSERT_TRUE(debug.has_value()) << debug.error().describe();
    EXPECT_EQ(debug->commandArguments, "--print \"from frost\" --exit 5");
    EXPECT_TRUE(sameFile(debug->workingDirectory, work.join("x64"))) << debug->workingDirectory;
    EXPECT_EQ(debug->environment, "FROST_TINY_VALUE=written by App");
    request.configuration = "Release";
    auto release = frost::build::queryRunProperties(frost::io::realProcessLauncher(), request, kBuildTimeout);
    ASSERT_TRUE(release.has_value()) << release.error().describe();
    EXPECT_EQ(release->commandArguments, "--release");
    EXPECT_TRUE(sameFile(release->workingDirectory, work.join("App\\"))) << "MSBuild's default, $(ProjectDir)";

    // Run (the path F5 shares up to the launch) starts the program with what MSBuild reported.
    auto loaded = frost::project::SolutionModel::load(fs, work.join("Tiny.sln"));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().describe();
    auto solution = std::make_shared<const frost::project::SolutionModel>(std::move(*loaded));
    auto store = frost::project::TrustStore::open(fs, work.join("data\\trusted-roots.txt"));
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE((*store)->trust(solution->directory()).has_value());
    auto buildService = frost::build::BuildService::create(
        fs, frost::io::realProcessLauncher(), **store,
        {.stateFilePath = work.join("data\\active-configurations.txt"), .msbuildPath = msbuild->msbuildPath});
    ASSERT_TRUE(buildService->openSolution(solution).has_value());
    const auto builder = frost::build::makeRunBuilder(*buildService);
    frost::Mutex mutex;
    std::string out;
    auto service = frost::build::RunService::create(
        fs, frost::io::realProcessLauncher(), **store, *builder,
        {.stateFilePath = work.join("data\\run-settings.txt"),
         .msbuildPath = msbuild->msbuildPath,
         .externalConsoleByDefault = false},
        {.onOutput = [&](frost::build::RunId, frost::build::RunOutputKind kind, std::string_view text) {
            if (kind == frost::build::RunOutputKind::Stdout) {
                frost::LockGuard lock(mutex);
                out += text;
            }
        }});
    ASSERT_TRUE(service->openSolution(solution).has_value());
    auto run = service->run();
    ASSERT_TRUE(run.has_value()) << run.error().describe();
    ASSERT_TRUE(service->waitForIdle(kBuildTimeout)) << "the run did not finish in time";
    const auto outcome = service->lastOutcome();
    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->ran()) << (outcome->error ? outcome->error->describe() : "");
    EXPECT_EQ(outcome->exitCode, 5);
    frost::LockGuard lock(mutex);
    std::erase(out, '\r');
    EXPECT_TRUE(out.contains("arg 1: --print\narg 2: from frost\n")) << out;
    EXPECT_TRUE(out.contains("FROST_TINY_VALUE=written by App\n")) << out;
    EXPECT_TRUE(out.contains("\\x64\n")) << "the working directory is the user file's: " << out;
    service.reset();
    EXPECT_TRUE(buildService->waitForIdle(kBuildTimeout));
}

} // namespace
