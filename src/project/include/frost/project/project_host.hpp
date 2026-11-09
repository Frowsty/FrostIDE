// SPDX-License-Identifier: Apache-2.0
//
// ProjectHost (SLN-2, PLAN.md section 2 "MSBuild / .vcxproj" path (b)): the C# helper that
// evaluates MSBuild projects with the Visual Studio instance's own Microsoft.Build, its client,
// its MSBuild.exe fallback (path (c)) and the solution-wide evaluation that feeds the Solution
// Explorer (SLN-3). Windows only in practice: off Windows there is no Visual Studio MSBuild, and
// SolutionEvaluation reports "MSBuild unavailable".
/*
 * The helper is tools/project-host-spike's ProjectHostVsProbe.exe (net472; built by
 * tools/build_project_host.py next to frost.exe in project-host/). It is started through
 * IProcessLauncher with one argument, the absolute directory of the chosen instance's
 * MSBuild.exe (frost::build::findMsBuild's vswhere result), and registers that instance the way
 * MSBuildLocator does (ProjectEvaluator.cs). It speaks JSON lines over stdin and stdout, one
 * response line per request line, strictly in order:
 *
 *   {"Op":"Hello"}
 *     -> {"Op":"Hello","Protocol":1,"MSBuildVersion":"17.14.x","MSBuildAssembly":...,"ProcessId":n}
 *   {"Op":"Evaluate","Id":...,"Trusted":"true","Project":<abs>,"ItemTypes":"ClCompile;...",
 *    "Metadata":"A;B","Properties":"C;D","Property:Configuration":"Debug","Property:Platform":"x64",...}
 *     -> {"Op":"Evaluate","Id":...,"Error":null,"Project":...,"Items":[{"Type","Include","FullPath",
 *         "DefiningProject","Metadata":{...}}],"Imports":[...],"Properties":{...},
 *         "ProjectConfigurations":[...]}
 *   A failed request answers {"Op":...,"Id":...,"Error":"<message>"}.
 *
 * Evaluation runs project logic (conditions, property functions, imports), so it needs a trusted
 * root: the request says so explicitly and the helper refuses it otherwise. The global properties
 * are exactly evaluationGlobalProperties(), which the MSBuild.exe fallback and the tests' oracle
 * pass too, so all three evaluate the same project.
 *
 * Errors: a request whose answer carries Error fails with ErrorCode::Corrupt for an unreadable
 * answer and ErrorCode::IoError for the helper's own message (a project MSBuild cannot load);
 * the helper ending fails every pending and later request with ErrorCode::IoError naming its exit
 * code; Timeout ends the helper.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/process.hpp"
#include "frost/project/solution_model.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

// The protocol version ProjectEvaluator.cs speaks; a helper answering another one is refused.
inline constexpr int kProjectHostProtocol = 1;

// What the helper's Hello answer says about it.
struct ProjectHostHello {
    int protocol = 0;
    std::string msbuildVersion;      // Microsoft.Build.dll's file version, "17.14.40.60911".
    std::string msbuildAssembly;     // The Microsoft.Build.dll it loaded (absolute).
    std::string visualStudioVersion; // "17.0".
    std::uint32_t processId = 0;
};

// One project evaluation.
struct ProjectEvaluationRequest {
    std::string projectPath;   // Absolute .vcxproj.
    std::string configuration; // The project configuration ("Debug").
    std::string platform;      // The project platform ("x64").
    std::string solutionPath;  // Absolute .sln/.slnx for the Solution* properties; may be empty.
    // The item types to return; empty means fileItemTypeNames().
    std::vector<std::string> itemTypes = {};
    // Item metadata values to return per item ("PreprocessorDefinitions").
    std::vector<std::string> metadata = {};
    // Property values to return ("ConfigurationType").
    std::vector<std::string> properties = {};
};

/*
 * The global properties of an evaluation, in order: Configuration, Platform and, with a solution,
 * SolutionDir (with a trailing '\'), SolutionPath, SolutionName, SolutionFileName and SolutionExt,
 * as Visual Studio sets them for a project of a loaded solution. Pure.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
evaluationGlobalProperties(const ProjectEvaluationRequest& request);

// One evaluated item.
struct HostItem {
    std::string type;            // "ClCompile".
    std::string include;         // The evaluated include.
    std::string fullPath;        // Absolute (the FullPath metadata).
    std::string definingProject; // The project or import that defines the item (absolute); "" when unknown.
    std::map<std::string, std::string, std::less<>> metadata; // The requested metadata, by name as requested.
};

// One project's evaluation.
struct HostEvaluation {
    std::string projectPath;
    std::string configuration;
    std::string platform;
    std::vector<HostItem> items;      // Evaluation order, per requested type.
    std::vector<std::string> imports; // Evaluation order, each once; empty for the MSBuild.exe fallback.
    std::map<std::string, std::string, std::less<>> properties;
    std::vector<std::string> projectConfigurations; // "Debug|x64", ...; empty for the fallback.
};

// The request lines (no trailing newline). Errors: InvalidArgument (a path that is not absolute,
// an empty configuration or platform).
[[nodiscard]] std::string projectHostHelloRequest();
[[nodiscard]] Result<std::string> projectHostEvaluateRequest(const ProjectEvaluationRequest& request,
                                                             std::string_view id);

// Parses the answer lines. Errors: Corrupt (not the expected JSON, another protocol), IoError
// (the helper's Error message).
[[nodiscard]] Result<ProjectHostHello> parseProjectHostHello(std::string_view line);
[[nodiscard]] Result<HostEvaluation> parseProjectHostEvaluation(std::string_view line);

/*
 * Path (c), the fallback: MSBuild.exe <project> -nologo -nodeReuse:false -getItem:<types>
 * -getProperty:<properties> -p:<evaluationGlobalProperties> in the project's directory, with
 * MSBUILDDISABLENODEREUSE=1 and MSBUILDTERMINALLOGGER=off. Errors: InvalidArgument.
 */
[[nodiscard]] Result<io::ProcessSpec> msbuildCliEvaluationSpec(std::string_view msbuildPath,
                                                               const ProjectEvaluationRequest& request);
// Reads that run's JSON (text before the first '{' is skipped). Metadata names are matched
// case-insensitively. Errors: Corrupt.
[[nodiscard]] Result<HostEvaluation> parseMsBuildCliEvaluation(std::string_view output,
                                                               const ProjectEvaluationRequest& request);

// The Solution Explorer's view of an evaluation: file items only, paths normalised, each path
// once (the first item wins). Pure.
[[nodiscard]] EvaluatedProjectItems toEvaluatedProjectItems(const HostEvaluation& evaluation, EvaluationSource source,
                                                            std::string_view msbuildVersion);

// How to start the helper.
struct ProjectHostLaunch {
    std::string hostPath;    // Absolute ProjectHostVsProbe.exe.
    std::string msbuildPath; // Absolute MSBuild.exe of the instance (its directory is passed).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment = {}; // Added to the child's.
    IClock::Duration startTimeout = std::chrono::seconds(60);    // Process start to the Hello answer.
    IClock::Duration requestTimeout = std::chrono::seconds(180); // One evaluation.
};

// The helper's process spec. Errors: InvalidArgument (paths not absolute).
[[nodiscard]] Result<io::ProcessSpec> projectHostSpec(const ProjectHostLaunch& launch);

/*
 * One running helper. start() launches it and waits for its Hello; evaluate() sends one request
 * and waits for its answer. A reader thread collects stdout lines and a second one drains stderr
 * (its last lines go into the error when the helper ends). Destruction ends the helper's process
 * tree and joins both threads. One caller at a time for evaluate(); pid() and running() from any
 * thread.
 */
class ProjectHostClient {
public:
    // Errors: those of projectHostSpec and the launch; IoError (the helper ended before Hello),
    // Timeout, Cancelled (stop requested), Corrupt/Unsupported (an unexpected Hello).
    [[nodiscard]] static Result<std::unique_ptr<ProjectHostClient>>
    start(io::IProcessLauncher& launcher, const ProjectHostLaunch& launch, std::stop_token stop = {});

    ~ProjectHostClient();
    ProjectHostClient(const ProjectHostClient&) = delete ("one client owns one helper process");
    ProjectHostClient& operator=(const ProjectHostClient&) = delete ("one client owns one helper process");

    // Errors: see the header comment; Cancelled when `stop` is requested (the helper keeps running).
    [[nodiscard]] Result<HostEvaluation> evaluate(const ProjectEvaluationRequest& request, std::stop_token stop = {});

    [[nodiscard]] const ProjectHostHello& hello() const noexcept { return m_hello; }
    [[nodiscard]] io::ProcessId pid() const noexcept { return m_pid; }
    // False once the helper's stdout has ended (it exited, crashed or was killed).
    [[nodiscard]] bool running() const;
    // Blocks until the helper ends (true) or `stop` is requested (false).
    [[nodiscard]] bool waitUntilEnded(std::stop_token stop);
    // Why the helper ended ("ProjectHost ended (exit code 1: ...)"); meaningful once running() is false.
    [[nodiscard]] Error endedError();

private:
    struct Shared;
    ProjectHostClient() = default;
    // The next stdout line, waiting until the deadline. Errors: IoError (ended), Timeout, Cancelled.
    [[nodiscard]] Result<std::string> nextLine(IClock::Duration timeout, std::stop_token stop);

    std::unique_ptr<io::IProcess> m_process;
    std::shared_ptr<Shared> m_shared;
    std::jthread m_stdoutReader;
    std::jthread m_stderrReader;
    ProjectHostHello m_hello;
    io::ProcessId m_pid = 0;
    IClock::Duration m_requestTimeout = std::chrono::seconds(180);
    std::uint64_t m_nextId = 1;
};

// The state of a solution's evaluation, for the Solution Explorer's banner.
enum class SolutionEvaluationState : unsigned char {
    Starting,   // Locating MSBuild and starting ProjectHost.
    Evaluating, // Projects are being evaluated (by ProjectHost or the fallback).
    Ready,      // Every project was evaluated (some may have failed; see the warnings).
    Stopped,    // requestStop() was called.
};

struct SolutionEvaluationStatus {
    SolutionEvaluationState state = SolutionEvaluationState::Starting;
    // Which evaluator answers now: ProjectHost, or MSBuild.exe after ProjectHost became unavailable.
    std::optional<EvaluationSource> source;
    // Why ProjectHost is not used ("" while it is): it is missing, failed to start or ended.
    std::string projectHostProblem;
    // Why nothing evaluates ("" while something does): no MSBuild, not trusted, not Windows.
    std::string msbuildProblem;
    std::string msbuildVersion;        // Of the helper's Microsoft.Build, or "" for the fallback.
    std::size_t evaluated = 0;         // Projects evaluated so far.
    std::size_t total = 0;             // Projects to evaluate.
    std::vector<std::string> warnings; // One per project whose evaluation failed (it stays static).
    std::uint64_t generation = 0;      // Increases with every change of results().
};

struct SolutionEvaluationOptions {
    std::string hostPath; // Absolute ProjectHostVsProbe.exe; empty or missing: straight to the fallback.
    // MSBuild.exe; when empty, locateMsBuild finds it (frost::build::findMsBuild in the app).
    std::string msbuildPath;
    std::function<Result<std::string>()> locateMsBuild;
    std::string configuration; // The solution configuration ("Debug|x64"); empty selects the first.
    // Live trust of the solution's root: nothing runs unless it returns true (checked before each project).
    std::function<bool()> trusted;
    std::vector<std::pair<std::string, std::optional<std::string>>> environment = {};
    IClock::Duration startTimeout = std::chrono::seconds(60);
    IClock::Duration requestTimeout = std::chrono::seconds(180);
    // Called on the worker thread after each project's result is published, with the number of
    // projects done and the running helper's process id (tests end the helper at a known point
    // with it); may be empty.
    std::function<void(std::size_t evaluated, std::optional<io::ProcessId> hostPid)> afterProject = {};
};

/*
 * Evaluates every loaded C++ project that builds in the solution configuration, in solution-file
 * order (ADR 0006 decision 5), on its own thread: through ProjectHost first and, once ProjectHost
 * is missing, fails to start or ends (killed, crashed, timed out), through MSBuild.exe for the
 * remaining projects (path (c)), with the reason in status().projectHostProblem for the banner.
 * Without MSBuild nothing is evaluated and status().msbuildProblem says why. A project whose
 * evaluation fails keeps its static items, with a warning. results() grows project by project;
 * SolutionLoadOptions::evaluated takes it as it is.
 *
 * Thread safety: status(), results() and hostPid() from any thread. The destructor (and
 * stopAndJoin) requests a stop, ends the helper and joins the worker: it blocks for at most one
 * running MSBuild.exe, which is ended too, so call it off the GUI thread when that matters.
 */
class SolutionEvaluation {
public:
    // `launcher` and `fs` must outlive this object.
    SolutionEvaluation(io::IProcessLauncher& launcher, io::IFileSystem& fs, std::shared_ptr<const SolutionModel> model,
                       SolutionEvaluationOptions options);
    ~SolutionEvaluation();
    SolutionEvaluation(const SolutionEvaluation&) = delete ("owns a worker thread");
    SolutionEvaluation& operator=(const SolutionEvaluation&) = delete ("owns a worker thread");

    void requestStop();
    void stopAndJoin();

    [[nodiscard]] SolutionEvaluationStatus status() const;
    [[nodiscard]] std::vector<EvaluatedProjectItems> results() const;
    // The running helper's process id, if one runs.
    [[nodiscard]] std::optional<io::ProcessId> hostPid() const;

private:
    void run(std::stop_token stop);
    void publish(std::function<void(SolutionEvaluationStatus&)> change);

    io::IProcessLauncher& m_launcher;
    io::IFileSystem& m_fs;
    std::shared_ptr<const SolutionModel> m_model;
    SolutionEvaluationOptions m_options;
    mutable Mutex m_mutex;
    SolutionEvaluationStatus m_status FROST_GUARDED_BY(m_mutex);
    std::vector<EvaluatedProjectItems> m_results FROST_GUARDED_BY(m_mutex);
    std::optional<io::ProcessId> m_hostPid FROST_GUARDED_BY(m_mutex);
    std::jthread m_worker; // Its stop token also ends a running MSBuild.exe of the fallback.
};

} // namespace frost::project
