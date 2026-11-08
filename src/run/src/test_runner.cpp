// SPDX-License-Identifier: Apache-2.0
//
// TestRunner (test_runner.hpp): ctest and Google Test programs run through build::RunSession (the
// process tree contained and ended on cancel), their output handed to the parsers in
// ctest_parsers.cpp and gtest_parsers.cpp. This file builds no result itself.

#include "frost/run/test_runner.hpp"

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/run_session.hpp"
#include "frost/core/log.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/run/ctest_parsers.hpp"
#include "frost/run/gtest_parsers.hpp"
#include "frost/run/run_manager.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

namespace frost::run {

namespace {

constexpr std::string_view kLog = "run.tests";

// The end of a tool's output for an error message: its last lines, at most this many bytes.
constexpr std::size_t kErrorTailBytes = 2000;

std::string outputTail(std::string_view stdoutText, std::string_view stderrText) {
    std::string text = std::string(stderrText);
    if (!stdoutText.empty()) {
        if (!text.empty() && !text.ends_with('\n'))
            text += '\n';
        text += stdoutText;
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.size() > kErrorTailBytes)
        text = "..." + text.substr(text.size() - kErrorTailBytes);
    return text;
}

std::string toolFailure(std::string_view tool, int exitCode, std::string_view stdoutText, std::string_view stderrText) {
    const std::string tail = outputTail(stdoutText, stderrText);
    if (tail.empty())
        return std::format("{} exited with code {} and wrote no output", tool, exitCode);
    return std::format("{} exited with code {}:\n{}", tool, exitCode, tail);
}

// A ctest regular-expression character (cmsys::RegularExpression's metacharacters).
bool isRegexSpecial(char c) { return std::string_view("^$.[]()|*+?\\{}").find(c) != std::string_view::npos; }

std::string programDisplay(const io::ProcessSpec& spec) {
    LaunchPlan plan;
    plan.program = spec.executable;
    plan.arguments = spec.arguments;
    return displayCommandLine(plan);
}

// What a finished process printed and how it ended.
struct ProcessOutcome {
    std::string out{};
    std::string err{};
    build::RunExit exit{};
};

} // namespace

struct TestRunner::State {
    State(io::IProcessLauncher& l, io::IFileSystem& f, IClock& c) : launcher(l), fs(f), clock(c) {}

    io::IProcessLauncher& launcher;
    io::IFileSystem& fs;
    IClock& clock;

    mutable std::mutex mutex;
    mutable std::condition_variable idleChanged;
    bool busy = false;
    std::stop_source stop;
    std::jthread worker;
    std::atomic<std::uint64_t> jobs{0};

    /*
     * Runs `spec` to its end: stdout is collected (and passed to `onStdout` as it arrives), stderr
     * collected; `stop` ends the tree. Errors: the launcher's.
     */
    Result<ProcessOutcome> runProcess(io::ProcessSpec spec, std::stop_token token,
                                      const std::function<void(std::string_view)>& onStdout) {
        struct Collector {
            std::mutex mutex;
            std::condition_variable done;
            ProcessOutcome outcome;
            bool exited = false;
        };
        auto collector = std::make_shared<Collector>();
        build::RunSessionCallbacks callbacks;
        callbacks.onOutput = [collector, &onStdout](build::RunStream stream, std::string_view text) {
            {
                std::lock_guard lock(collector->mutex);
                (stream == build::RunStream::Stdout ? collector->outcome.out : collector->outcome.err) += text;
            }
            if (stream == build::RunStream::Stdout && onStdout)
                onStdout(text);
        };
        callbacks.onExited = [collector](const build::RunExit& exit) {
            std::lock_guard lock(collector->mutex);
            collector->outcome.exit = exit;
            collector->exited = true;
            collector->done.notify_all();
        };
        spec.killOnClose = true;
        spec.stdinMode = io::Stdio::Null;
        build::RunSessionOptions options;
        options.terminateDescendantsOnExit = true; // Tests ctest left behind end with it.
        auto session = build::RunSession::start(launcher, spec, std::move(callbacks), clock, options);
        if (!session)
            return std::unexpected(std::move(session.error()));
        {
            std::stop_callback onStop(token, [&session] { (*session)->stop(); });
            std::unique_lock lock(collector->mutex);
            collector->done.wait(lock, [&] { return collector->exited; });
        }
        session->reset(); // Joins the session's threads; every callback has returned.
        std::lock_guard lock(collector->mutex);
        return std::move(collector->outcome);
    }

    // A path under <build>/Testing/Temporary for this job's files.
    std::string scratchPath(const CTestSetup& setup, std::uint64_t job, std::string_view suffix) const {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = std::format("frostide-{:x}-{}-{}", stamp, job, suffix);
        auto dir = io::joinPath(setup.buildDirectory, "Testing");
        auto temporary = dir ? io::joinPath(*dir, "Temporary") : dir;
        auto path = temporary ? io::joinPath(*temporary, name) : temporary;
        return path.value_or(std::string{});
    }
};

namespace {

io::ProcessSpec ctestSpec(const CTestSetup& setup, std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = setup.ctest;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = setup.buildDirectory;
    spec.environment = setup.environment;
    return spec;
}

void appendSelection(const CTestSetup& setup, std::vector<std::string>& arguments) {
    if (!setup.configuration.empty())
        arguments.insert(arguments.end(), {"-C", setup.configuration});
    if (!setup.includeName.empty())
        arguments.insert(arguments.end(), {"-R", setup.includeName});
    if (!setup.excludeName.empty())
        arguments.insert(arguments.end(), {"-E", setup.excludeName});
    if (!setup.includeLabel.empty())
        arguments.insert(arguments.end(), {"-L", setup.includeLabel});
    if (!setup.excludeLabel.empty())
        arguments.insert(arguments.end(), {"-LE", setup.excludeLabel});
}

Result<void> checkSetup(const CTestSetup& setup) {
    if (!io::isAbsolutePath(setup.ctest))
        return fail(ErrorCode::InvalidArgument, std::format("ctest '{}' is not an absolute path", setup.ctest));
    if (!io::isAbsolutePath(setup.buildDirectory))
        return fail(ErrorCode::InvalidArgument,
                    std::format("the build directory '{}' is not an absolute path", setup.buildDirectory));
    return {};
}

} // namespace

Result<CTestSetup> ctestSetupFor(const project::CMakeProject& model, const project::CMakeProfile& profile,
                                 const project::CMakeTestPreset* preset) {
    if (!model.cmakeVersion.empty() && !build::versionAtLeast(model.cmakeVersion, 3, 21))
        return fail(ErrorCode::Unsupported,
                    std::format("ctest {} cannot write a JUnit report; the test runner needs CMake 3.21 or later",
                                model.cmakeVersion));
    CTestSetup setup;
    if (auto ctest = model.cacheValue("CMAKE_CTEST_COMMAND"); ctest && !ctest->empty()) {
        setup.ctest = *ctest;
    } else if (auto cmake = model.cacheValue("CMAKE_COMMAND"); cmake && !cmake->empty()) {
        const std::string_view cmakePath = *cmake;
        const auto slash = cmakePath.find_last_of("/\\");
        const std::string_view name = slash == std::string_view::npos ? cmakePath : cmakePath.substr(slash + 1);
        const bool exe = name.size() > 4 && (name.ends_with(".exe") || name.ends_with(".EXE"));
        setup.ctest = std::string(cmakePath.substr(0, slash == std::string_view::npos ? 0 : slash + 1)) + "ctest" +
                      (exe ? ".exe" : "");
    } else {
        return fail(ErrorCode::NotFound, "the CMake cache names no ctest (CMAKE_CTEST_COMMAND)");
    }
    auto ctest = io::normalizePath(setup.ctest);
    if (!ctest || !io::isAbsolutePath(*ctest))
        return fail(ErrorCode::InvalidArgument, std::format("ctest '{}' is not an absolute path", setup.ctest));
    setup.ctest = std::move(*ctest);
    setup.ctestVersion = model.cmakeVersion;
    setup.buildDirectory = model.buildDirectory;
    if (!io::isAbsolutePath(setup.buildDirectory))
        return fail(ErrorCode::InvalidArgument, "the CMake model has no absolute build directory");
    if (model.multiConfig)
        setup.configuration = model.configuration.empty() ? profile.effectiveConfiguration() : model.configuration;
    setup.environment = profile.buildEnvironment ? *profile.buildEnvironment : profile.environment;
    if (preset) {
        setup.testPreset = preset->name;
        if (!preset->configuration.empty())
            setup.configuration = preset->configuration;
        setup.includeName = preset->includeName;
        setup.excludeName = preset->excludeName;
        setup.includeLabel = preset->includeLabel;
        setup.excludeLabel = preset->excludeLabel;
        setup.jobs = preset->jobs.value_or(0);
        setup.timeout = preset->timeout;
        setup.stopOnFailure = preset->stopOnFailure;
        setup.environment = preset->environment;
    }
    return setup;
}

const project::CMakeTestPreset* testPresetFor(const project::CMakePresets& presets,
                                              const project::CMakeProfile& profile) {
    if (profile.configurePreset.empty())
        return nullptr;
    const std::vector<const project::CMakeTestPreset*> candidates = presets.testPresetsFor(profile.configurePreset);
    const project::CMakeTestPreset* first = nullptr;
    for (const project::CMakeTestPreset* preset : candidates) {
        if (!preset || preset->hidden || !preset->enabled)
            continue;
        if (preset->name == profile.buildPreset || preset->name == profile.configurePreset)
            return preset;
        if (!first)
            first = preset;
    }
    return first;
}

std::vector<std::string> ctestDiscoveryArguments(const CTestSetup& setup) {
    std::vector<std::string> arguments = {"--test-dir", setup.buildDirectory};
    appendSelection(setup, arguments);
    arguments.insert(arguments.end(), {"-N", "--show-only=json-v1"});
    return arguments;
}

std::string ctestExactNameRegex(const std::vector<std::string>& names) {
    std::string out = "^(";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0)
            out += '|';
        for (const char c : names[i]) {
            if (isRegexSpecial(c))
                out += '\\';
            out += c;
        }
    }
    out += ")$";
    return out;
}

std::unique_ptr<TestRunner> TestRunner::create(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                               TestRunnerOptions options) {
    auto state = std::make_shared<State>(launcher, fs, options.clock ? *options.clock : systemClock());
    return std::unique_ptr<TestRunner>(new TestRunner(std::move(state)));
}

TestRunner::TestRunner(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

TestRunner::~TestRunner() {
    std::jthread worker;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stop.request_stop();
        worker = std::move(m_state->worker);
    }
    if (worker.joinable())
        worker.join();
}

namespace {

/*
 * Starts `job` on a new worker thread once no job runs. The job calls `finish` right before its
 * last callback, so the runner is idle again by the time that callback reaches the caller.
 */
template<typename Job>
Result<void> startJob(TestRunner::State& state, std::shared_ptr<TestRunner::State> keep, Job job) {
    std::jthread previous;
    std::unique_lock lock(state.mutex);
    if (state.busy)
        return fail(ErrorCode::Busy, "the test runner is busy");
    state.busy = true;
    previous = std::move(state.worker);
    state.stop = std::stop_source{};
    const std::stop_token token = state.stop.get_token();
    const std::uint64_t number = ++state.jobs;
    lock.unlock();
    if (previous.joinable())
        previous.join(); // It finished (busy was false); only its thread's end is left.
    lock.lock();
    state.worker = std::jthread([keep = std::move(keep), job = std::move(job), token, number]() mutable {
        auto finish = [&keep] {
            std::lock_guard guard(keep->mutex);
            keep->busy = false;
            keep->idleChanged.notify_all();
        };
        job(*keep, token, number, finish);
    });
    return {};
}

} // namespace

Result<void> TestRunner::discover(const CTestSetup& setup,
                                  std::function<void(Result<std::vector<DiscoveredTest>>)> done) {
    if (auto valid = checkSetup(setup); !valid)
        return valid;
    return startJob(*m_state, m_state,
                    [setup, done = std::move(done)](State& state, std::stop_token token, std::uint64_t,
                                                    const std::function<void()>& finish) {
                        Result<std::vector<DiscoveredTest>> result = fail(ErrorCode::Cancelled, "stopped");
                        auto outcome = state.runProcess(ctestSpec(setup, ctestDiscoveryArguments(setup)), token, {});
                        if (!outcome) {
                            result = std::unexpected(std::move(outcome.error()));
                        } else if (outcome->exit.stopped || token.stop_requested()) {
                            result = fail(ErrorCode::Cancelled, "test discovery was stopped");
                        } else if (outcome->exit.exitCode != 0 || outcome->exit.waitFailed) {
                            result = fail(ErrorCode::IoError,
                                          toolFailure("ctest", outcome->exit.exitCode, outcome->out, outcome->err));
                        } else if (outcome->out.empty()) {
                            result = fail(ErrorCode::Corrupt, toolFailure("ctest", 0, outcome->out, outcome->err));
                        } else {
                            result = parseCTestShowOnly(outcome->out);
                        }
                        finish();
                        if (done)
                            done(std::move(result));
                    });
}

Result<void> TestRunner::run(const CTestSetup& setup, std::vector<DiscoveredTest> tests, std::vector<std::string> names,
                             TestRunHandlers handlers) {
    if (auto valid = checkSetup(setup); !valid)
        return valid;
    return startJob(
        *m_state, m_state,
        [setup, tests = std::move(tests), names = std::move(names), handlers = std::move(handlers)](
            State& state, std::stop_token token, std::uint64_t job, const std::function<void()>& finish) {
            TestRunReport report;
            const std::string reportPath = state.scratchPath(setup, job, "junit.xml");
            const std::string listPath = state.scratchPath(setup, job, "tests.txt");
            const std::string gtestDir = state.scratchPath(setup, job, "gtest");
            const auto cleanUp = [&] {
                static_cast<void>(state.fs.remove(reportPath));
                static_cast<void>(state.fs.remove(listPath));
                static_cast<void>(state.fs.removeAll(gtestDir));
            };
            const auto end = [&] {
                cleanUp();
                finish();
                if (handlers.onFinished)
                    handlers.onFinished(report);
            };
            if (reportPath.empty() || gtestDir.empty()) {
                report.error = Error{ErrorCode::InvalidArgument, "the build directory cannot hold ctest's report"};
                end();
                return;
            }
            if (auto made = state.fs.createDirectories(gtestDir); !made) {
                report.error = made.error();
                end();
                return;
            }

            std::vector<std::string> arguments = {"--test-dir", setup.buildDirectory};
            appendSelection(setup, arguments);
            if (setup.jobs > 0)
                arguments.insert(arguments.end(), {"-j", std::to_string(setup.jobs)});
            if (setup.timeout)
                arguments.insert(arguments.end(), {"--timeout", std::to_string(*setup.timeout)});
            if (setup.stopOnFailure)
                arguments.emplace_back("--stop-on-failure");
            arguments.insert(arguments.end(), {"--no-tests=error", "--output-junit", reportPath});
            if (!names.empty()) {
                if (setup.ctestVersion.empty() || build::versionAtLeast(setup.ctestVersion, 3, 29)) {
                    std::string list;
                    for (const std::string& name : names)
                        list += name + "\n";
                    if (auto written = state.fs.writeFileAtomic(listPath, list); !written) {
                        report.error = written.error();
                        end();
                        return;
                    }
                    arguments.insert(arguments.end(), {"--tests-from-file", listPath});
                } else {
                    arguments.insert(arguments.end(), {"-R", ctestExactNameRegex(names)});
                }
            }
            io::ProcessSpec spec = ctestSpec(setup, std::move(arguments));
            // Every Google Test program ctest starts writes its JSON report into gtestDir (gtest_parsers.hpp).
            spec.environment.emplace_back("GTEST_OUTPUT", "json:" + gtestDir + "/");
            report.commandLine = programDisplay(spec);

            // ctest's progress lines, split from its stdout.
            std::map<std::size_t, std::string> startedNames;
            std::map<std::string, TestResult, std::less<>> interim;
            std::string pending;
            const auto onLine = [&](std::string_view line) {
                const auto progress = parseCTestProgressLine(line);
                if (!progress)
                    return;
                if (progress->kind == CTestProgressLine::Kind::Started) {
                    startedNames[progress->number] = progress->text;
                    if (handlers.onTestStarted)
                        handlers.onTestStarted(progress->text);
                    return;
                }
                const auto started = startedNames.find(progress->number);
                if (started == startedNames.end())
                    return;
                if (auto result = ctestProgressResult(*progress, started->second)) {
                    if (handlers.onTestFinished)
                        handlers.onTestFinished(*result);
                    interim.insert_or_assign(result->name, std::move(*result));
                }
            };
            const std::function<void(std::string_view)> onStdout = [&](std::string_view text) {
                if (handlers.onOutput)
                    handlers.onOutput(text);
                pending += text;
                std::size_t start = 0;
                for (auto newline = pending.find('\n'); newline != std::string::npos;
                     newline = pending.find('\n', start)) {
                    onLine(std::string_view(pending).substr(start, newline - start));
                    start = newline + 1;
                }
                pending.erase(0, start);
            };
            auto outcome = state.runProcess(std::move(spec), token, onStdout);
            if (!pending.empty())
                onLine(pending);
            if (!outcome) {
                report.error = outcome.error();
                end();
                return;
            }
            if (outcome->exit.stopped || token.stop_requested()) {
                report.stopped = true;
                for (const DiscoveredTest& test : tests) {
                    if (auto found = interim.find(test.name); found != interim.end())
                        report.results.push_back(found->second);
                }
                end();
                return;
            }
            report.exitCode = outcome->exit.exitCode;
            auto reportText = state.fs.readFile(reportPath);
            if (!reportText) {
                report.error =
                    Error{ErrorCode::IoError, toolFailure("ctest", outcome->exit.exitCode, outcome->out, outcome->err) +
                                                  "\n(no JUnit report was written)"};
                end();
                return;
            }
            auto results = parseCTestJUnit(*reportText);
            if (!results) {
                report.error = results.error();
                end();
                return;
            }
            // The Google Test reports the programs wrote.
            std::vector<GTestReport> gtestReports;
            if (auto entries = state.fs.listDirectory(gtestDir)) {
                for (const auto& entry : *entries) {
                    GTestReport gtestReport;
                    gtestReport.fileStem = gtestReportStem(entry.name);
                    auto path = io::joinPath(gtestDir, entry.name);
                    if (gtestReport.fileStem.empty() || !path)
                        continue;
                    auto text = state.fs.readFile(*path);
                    if (!text)
                        continue;
                    auto cases = parseGTestJsonReport(*text);
                    if (!cases) {
                        // Two programs racing for one unique name (ctest -j) can leave a file that does
                        // not parse; the ctest results stand without it.
                        log::warn(kLog, "{}: {}", *path, cases.error().describe());
                        continue;
                    }
                    gtestReport.cases = std::move(*cases);
                    gtestReports.push_back(std::move(gtestReport));
                }
            }
            attachGTestReports(*results, tests, gtestReports);
            report.results = std::move(*results);
            end();
        });
}

Result<void> TestRunner::listGTestCases(const CTestSetup& setup, const DiscoveredTest& test,
                                        std::function<void(Result<std::vector<GTestCase>>)> done) {
    if (test.command.empty() || !io::isAbsolutePath(test.command.front()))
        return fail(ErrorCode::InvalidArgument, std::format("the test '{}' has no program to list", test.name));
    io::ProcessSpec spec;
    spec.executable = test.command.front();
    spec.arguments = {"--gtest_list_tests"};
    if (!test.workingDirectory.empty() && io::isAbsolutePath(test.workingDirectory))
        spec.workingDirectory = test.workingDirectory;
    spec.environment = setup.environment;
    spec.environment.emplace_back("GTEST_OUTPUT", std::nullopt); // The listing goes to stdout only.
    return startJob(*m_state, m_state,
                    [spec = std::move(spec), done = std::move(done)](State& state, std::stop_token token, std::uint64_t,
                                                                     const std::function<void()>& finish) {
                        Result<std::vector<GTestCase>> result = fail(ErrorCode::Cancelled, "stopped");
                        const std::string program = spec.executable;
                        auto outcome = state.runProcess(spec, token, {});
                        if (!outcome) {
                            result = std::unexpected(std::move(outcome.error()));
                        } else if (outcome->exit.stopped || token.stop_requested()) {
                            result = fail(ErrorCode::Cancelled, "listing the Google Test cases was stopped");
                        } else if (outcome->exit.exitCode != 0 || outcome->exit.waitFailed) {
                            result = fail(ErrorCode::IoError,
                                          toolFailure(program, outcome->exit.exitCode, outcome->out, outcome->err));
                        } else {
                            result = parseGTestList(outcome->out);
                        }
                        finish();
                        if (done)
                            done(std::move(result));
                    });
}

void TestRunner::cancel() {
    std::lock_guard lock(m_state->mutex);
    if (m_state->busy)
        m_state->stop.request_stop();
}

bool TestRunner::busy() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->busy;
}

bool TestRunner::waitForIdle(IClock::Duration timeout) const {
    std::unique_lock lock(m_state->mutex);
    return m_state->idleChanged.wait_for(lock, timeout, [this] { return !m_state->busy; });
}

} // namespace frost::run
