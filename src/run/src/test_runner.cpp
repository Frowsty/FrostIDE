// SPDX-License-Identifier: Apache-2.0
//
// TestRunner (test_runner.hpp): ctest and Google Test programs run through build::RunSession (the
// process tree contained and ended on cancel), their output handed to the parsers in
// ctest_parsers.cpp, gtest_parsers.cpp and doctest_parsers.cpp. This file builds no result itself.

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
    Result<ProcessOutcome> runProcess(io::ProcessSpec spec, const std::stop_token& token,
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

    // Creates <build>/Testing/Temporary (a doctest program cannot write its --out file into a
    // directory that does not exist; ctest creates it for its own runs only).
    Result<void> makeScratchDirectory(const CTestSetup& setup) const {
        auto dir = io::joinPath(setup.buildDirectory, "Testing");
        auto temporary = dir ? io::joinPath(*dir, "Temporary") : dir;
        if (!temporary)
            return std::unexpected(temporary.error());
        return fs.createDirectories(*temporary);
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
    // The worker owns token; synchronous jobs and their joined subworkers borrow it for this call.
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
                    [setup, done = std::move(done)](State& state, const std::stop_token& token, std::uint64_t,
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
            State& state, const std::stop_token& token, std::uint64_t job, const std::function<void()>& finish) {
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
            // A failed doctest program's failing cases, from doctest's console output (header comment).
            for (TestResult& result : *results) {
                if (result.outcome != TestOutcome::Failed || !result.cases.empty())
                    continue;
                const auto test = std::ranges::find(tests, result.name, &DiscoveredTest::name);
                if (test != tests.end() && isDoctestCommand(test->command))
                    result.cases = parseDoctestConsoleFailures(result.output);
            }
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
    return startJob(
        *m_state, m_state,
        [spec = std::move(spec), done = std::move(done)](State& state, const std::stop_token& token, std::uint64_t,
                                                         const std::function<void()>& finish) {
            Result<std::vector<GTestCase>> result = fail(ErrorCode::Cancelled, "stopped");
            const std::string program = spec.executable;
            auto outcome = state.runProcess(spec, token, {});
            if (!outcome) {
                result = std::unexpected(std::move(outcome.error()));
            } else if (outcome->exit.stopped || token.stop_requested()) {
                result = fail(ErrorCode::Cancelled, "listing the Google Test cases was stopped");
            } else if (outcome->exit.exitCode != 0 || outcome->exit.waitFailed) {
                result =
                    fail(ErrorCode::IoError, toolFailure(program, outcome->exit.exitCode, outcome->out, outcome->err));
            } else {
                result = parseGTestList(outcome->out);
            }
            finish();
            if (done)
                done(std::move(result));
        });
}

namespace {

// The process of a doctest job for `test` (test_runner.hpp, listDoctestCases and runDoctest): its
// program in its working directory (ctest's build directory when it has none), the setup's
// environment and then its ENVIRONMENT property.
io::ProcessSpec doctestSpec(const CTestSetup& setup, const DiscoveredTest& test, std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = test.command.front();
    spec.arguments = std::move(arguments);
    spec.workingDirectory = !test.workingDirectory.empty() && io::isAbsolutePath(test.workingDirectory)
                                ? test.workingDirectory
                                : setup.buildDirectory;
    spec.environment = setup.environment;
    for (const std::string& entry : test.environment) {
        const auto equals = entry.find('=');
        if (equals == std::string::npos || equals == 0)
            continue;
        spec.environment.emplace_back(entry.substr(0, equals), entry.substr(equals + 1));
    }
    return spec;
}

Result<void> checkProgram(const DiscoveredTest& test) {
    if (test.command.empty() || !io::isAbsolutePath(test.command.front()))
        return fail(ErrorCode::InvalidArgument, std::format("the test '{}' has no program to run", test.name));
    return {};
}

} // namespace

Result<void> TestRunner::listDoctestCases(const CTestSetup& setup, std::vector<DiscoveredTest> tests,
                                          std::function<void(std::vector<DoctestListing>)> done) {
    if (auto valid = checkSetup(setup); !valid)
        return valid;
    for (const DiscoveredTest& test : tests)
        if (auto valid = checkProgram(test); !valid)
            return valid;
    return startJob(
        *m_state, m_state,
        [setup, tests = std::move(tests), done = std::move(done)](
            State& state, std::stop_token token, std::uint64_t job, const std::function<void()>& finish) {
            std::vector<DoctestListing> listings(tests.size());
            std::atomic<std::size_t> next{0};
            if (auto made = state.makeScratchDirectory(setup); !made) {
                for (std::size_t i = 0; i < tests.size(); ++i)
                    listings[i] = DoctestListing{tests[i].name, std::unexpected(made.error())};
                next = tests.size();
            }
            // Each worker takes the next test until none is left; a listing is one short process.
            const auto work = [&](std::size_t worker) {
                for (std::size_t i = next++; i < tests.size(); i = next++) {
                    const DiscoveredTest& test = tests[i];
                    DoctestListing& listing = listings[i];
                    listing.test = test.name;
                    if (token.stop_requested()) {
                        listing.cases = fail(ErrorCode::Cancelled, "listing the doctest cases was stopped");
                        continue;
                    }
                    const std::string reportPath =
                        state.scratchPath(setup, job, std::format("doctest-list-{}-{}.xml", worker, i));
                    if (reportPath.empty()) {
                        listing.cases =
                            fail(ErrorCode::InvalidArgument, "the build directory cannot hold doctest's case list");
                        continue;
                    }
                    auto outcome = state.runProcess(
                        doctestSpec(setup, test, doctestListArguments(test.command, reportPath)), token, {});
                    if (!outcome) {
                        listing.cases = std::unexpected(std::move(outcome.error()));
                    } else if (outcome->exit.stopped || token.stop_requested()) {
                        listing.cases = fail(ErrorCode::Cancelled, "listing the doctest cases was stopped");
                    } else if (outcome->exit.exitCode != 0 || outcome->exit.waitFailed) {
                        listing.cases =
                            fail(ErrorCode::IoError,
                                 toolFailure(test.command.front(), outcome->exit.exitCode, outcome->out, outcome->err));
                    } else if (auto text = state.fs.readFile(reportPath); !text) {
                        listing.cases =
                            fail(ErrorCode::Corrupt, toolFailure(test.command.front(), 0, outcome->out, outcome->err) +
                                                         "\n(it wrote no doctest case list)");
                    } else {
                        listing.cases = parseDoctestCaseList(*text);
                    }
                    static_cast<void>(state.fs.remove(reportPath));
                }
            };
            {
                const std::size_t workers = std::min(kDoctestListJobs, std::max<std::size_t>(tests.size(), 1));
                std::vector<std::jthread> pool;
                for (std::size_t w = 1; w < workers; ++w)
                    pool.emplace_back(work, w);
                work(0);
            } // Joins the pool.
            finish();
            if (done)
                done(std::move(listings));
        });
}

namespace {

template<typename Request, typename MakeSpec, typename ParseAndBuild>
Result<void> runDirectRequests(TestRunner::State& state, std::shared_ptr<TestRunner::State> keep,
                               const CTestSetup& setup, std::vector<Request> requests, TestRunHandlers handlers,
                               std::string_view reportTag, std::string_view reportKind, MakeSpec makeSpec,
                               ParseAndBuild parseAndBuild) {
    if (auto valid = checkSetup(setup); !valid)
        return valid;
    for (const Request& request : requests)
        if (auto valid = checkProgram(request.test); !valid)
            return valid;
    return startJob(
        state, std::move(keep),
        [setup, requests = std::move(requests), handlers = std::move(handlers), tag = std::string(reportTag),
         kind = std::string(reportKind), makeSpec = std::move(makeSpec),
         parseAndBuild = std::move(parseAndBuild)](TestRunner::State& st, const std::stop_token& token,
                                                   std::uint64_t job, const std::function<void()>& finish) {
            TestRunReport report;
            std::vector<std::string> errors;
            if (auto made = st.makeScratchDirectory(setup); !made) {
                report.error = made.error();
                finish();
                if (handlers.onFinished)
                    handlers.onFinished(report);
                return;
            }
            for (std::size_t i = 0; i < requests.size(); ++i) {
                const Request& request = requests[i];
                if (token.stop_requested()) {
                    report.stopped = true;
                    break;
                }
                const std::string reportPath = st.scratchPath(setup, job, std::format("{}-{}", i, tag));
                if (reportPath.empty()) {
                    errors.emplace_back(std::format("the build directory cannot hold {}'s report", kind));
                    break;
                }
                io::ProcessSpec spec = makeSpec(setup, request, reportPath);
                if (i == 0 || report.commandLine.empty())
                    report.commandLine = programDisplay(spec);
                if (handlers.onTestStarted)
                    handlers.onTestStarted(request.test.name);
                auto outcome = st.runProcess(std::move(spec), token, handlers.onOutput);
                if (!outcome) {
                    errors.push_back(std::format("{}: {}", request.test.name, outcome.error().describe()));
                    continue;
                }
                const bool stopped = outcome->exit.stopped || token.stop_requested();
                auto text = st.fs.readFile(reportPath);
                static_cast<void>(st.fs.remove(reportPath));
                std::string parseError;
                if (auto result = parseAndBuild(request, request.test.name,
                                                text ? std::optional<std::string_view>(*text) : std::nullopt, *outcome,
                                                stopped, parseError)) {
                    if (handlers.onTestFinished)
                        handlers.onTestFinished(*result);
                    report.results.push_back(std::move(*result));
                } else if (!stopped) {
                    errors.push_back(std::format(
                        "{}: {}", request.test.name,
                        toolFailure(request.test.command.front(), outcome->exit.exitCode, outcome->out, outcome->err) +
                            (text ? "\n(" + parseError + ")" : std::format("\n(it wrote no {} report)", kind))));
                }
                if (stopped) {
                    report.stopped = true;
                    break;
                }
            }
            if (!errors.empty()) {
                std::string message;
                for (const std::string& error : errors)
                    message += (message.empty() ? "" : "\n") + error;
                report.error = Error{ErrorCode::IoError, std::move(message)};
            }
            finish();
            if (handlers.onFinished)
                handlers.onFinished(report);
        });
}

} // namespace

Result<std::vector<std::string>> doctestCaseSelectionCommand(const std::vector<std::string>& command,
                                                             const std::vector<std::string>& cases) {
    if (command.empty())
        return fail(ErrorCode::InvalidArgument, "the doctest command has no program");
    if (cases.empty())
        return command;
    for (const std::string& name : cases) {
        if (name.empty())
            return fail(ErrorCode::InvalidArgument, "doctest cannot select an empty case name");
        // doctest 2.5.3 wildcmp always treats these as wildcards, even after a backslash.
        if (name.find_first_of("*?") != std::string::npos)
            return fail(ErrorCode::Unsupported,
                        std::format("doctest cannot select the case '{}' literally: '*' and '?' are wildcards", name));
    }
    bool prefixed = false;
    std::vector<std::string> selected{command.front()};
    for (std::size_t i = 1; i < command.size(); ++i) {
        std::string_view key = command[i];
        const auto first = key.find_first_not_of('-');
        if (first != 0 && first != std::string_view::npos) {
            key.remove_prefix(first);
            if (key.starts_with("dt-")) {
                prefixed = true;
                key.remove_prefix(3);
            }
            key = key.substr(0, key.find('='));
            // doctest parses the short and long aliases independently. Retaining either can
            // add cases or override case sensitivity even when a new long option is appended.
            if (key == "test-case" || key == "tc" || key == "case-sensitive" || key == "cs")
                continue;
        }
        selected.push_back(command[i]);
    }
    const std::string_view prefix = prefixed ? "dt-" : "";
    selected.push_back(std::format("--{}case-sensitive=true", prefix));
    selected.push_back(std::format("--{}test-case={}", prefix, doctestFilterValue(cases)));
    return selected;
}

Result<void> TestRunner::runDoctest(const CTestSetup& setup, std::vector<DoctestRunRequest> requests,
                                    TestRunHandlers handlers) {
    for (DoctestRunRequest& request : requests) {
        if (request.cases.empty())
            continue;
        auto selected = doctestCaseSelectionCommand(request.test.command, request.cases);
        if (!selected)
            return std::unexpected(std::move(selected.error()));
        request.test.command = std::move(*selected);
    }
    return runDirectRequests(
        *m_state, m_state, setup, std::move(requests), std::move(handlers), "doctest-run.xml", "doctest",
        [](const CTestSetup& s, const DoctestRunRequest& req, std::string_view path) {
            return doctestSpec(s, req.test, doctestRunArguments(req.test.command, {}, path));
        },
        [](const DoctestRunRequest& request, const std::string& name, std::optional<std::string_view> text,
           const ProcessOutcome& outcome, bool stopped, std::string& parseError) {
            DoctestProcessEnd end{outcome.exit.exitCode, stopped,
                                  std::chrono::duration_cast<std::chrono::microseconds>(outcome.exit.duration),
                                  outcome.err.empty() ? outcome.out : outcome.out + outcome.err};
            const Result<DoctestReport> parsed =
                text ? parseDoctestXmlReport(*text) : Result<DoctestReport>(fail(ErrorCode::NotFound, "no report"));
            if (!parsed)
                parseError = parsed.error().message();
            if (parsed && !stopped) {
                const std::size_t executed = parsed->passedCases + parsed->failedCases;
                // An interrupted report has no final counts. Completed case records and the
                // unfinished case still carry real execution evidence that must remain visible.
                const bool hasCaseEvidence = !parsed->cases.empty() || parsed->unfinished.has_value();
                if ((parsed->complete && executed == 0) || (!parsed->complete && !hasCaseEvidence)) {
                    parseError = "doctest reported no executed cases";
                    return std::optional<TestResult>{};
                }
                if (!request.cases.empty()) {
                    std::vector<std::string> expected = request.cases;
                    std::vector<std::string> actual;
                    for (const TestResult& item : parsed->cases)
                        actual.push_back(item.name);
                    if (!parsed->complete && parsed->unfinished)
                        actual.push_back(parsed->unfinished->name);
                    std::ranges::sort(expected);
                    std::ranges::sort(actual);
                    // Interrupted runs may execute only a subset. Retain that real evidence as
                    // a failed run, while rejecting any case outside the requested selection.
                    const bool matches =
                        parsed->complete ? actual == expected : std::ranges::includes(expected, actual);
                    if (!matches) {
                        parseError = std::format("doctest reported [{}], expected {} [{}]", gtestFilterValue(actual),
                                                 parsed->complete ? "exactly" : "only selected cases from",
                                                 gtestFilterValue(expected));
                        return std::optional<TestResult>{};
                    }
                }
            }
            return doctestRunResult(name, parsed, end);
        });
}

Result<void> TestRunner::runGTest(const CTestSetup& setup, std::vector<GTestRunRequest> requests,
                                  TestRunHandlers handlers) {
    return runDirectRequests(
        *m_state, m_state, setup, std::move(requests), std::move(handlers), "gtest-run.json", "Google Test",
        [](const CTestSetup& s, const GTestRunRequest& req, std::string_view path) {
            const std::span<const std::string> allArgs(req.test.command);
            const auto baseArgs = allArgs.size() > 1 ? allArgs.subspan(1) : std::span<const std::string>{};
            io::ProcessSpec spec = doctestSpec(s, req.test, gtestRunArguments(baseArgs, req.cases, path));
            spec.environment.emplace_back("GTEST_OUTPUT", std::nullopt);
            return spec;
        },
        [](const GTestRunRequest& request, const std::string& name, std::optional<std::string_view> text,
           const ProcessOutcome& outcome, bool stopped, std::string& parseError) {
            GTestProcessEnd end{outcome.exit.exitCode, stopped,
                                std::chrono::duration_cast<std::chrono::microseconds>(outcome.exit.duration),
                                outcome.err.empty() ? outcome.out : outcome.out + outcome.err};
            const Result<std::vector<TestResult>> parsed =
                text ? parseGTestJsonReport(*text)
                     : Result<std::vector<TestResult>>(fail(ErrorCode::NotFound, "no report"));
            if (!parsed)
                parseError = parsed.error().message();
            if (parsed && !stopped) {
                std::vector<std::string> actual;
                actual.reserve(parsed->size());
                for (const TestResult& testCase : *parsed)
                    actual.push_back(testCase.name);
                if (actual.empty()) {
                    parseError = "Google Test reported no executed cases";
                    return std::optional<TestResult>{};
                }
                if (!request.cases.empty()) {
                    auto expected = request.cases;
                    std::ranges::sort(expected);
                    auto sortedActual = actual;
                    std::ranges::sort(sortedActual);
                    if (expected != sortedActual) {
                        parseError = std::format("Google Test reported [{}], expected exactly [{}]",
                                                 gtestFilterValue(actual), gtestFilterValue(request.cases));
                        return std::optional<TestResult>{};
                    }
                }
            }
            return gtestRunResult(name, parsed, end);
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

Result<void> TestRunner::listFrameworkCases(const CTestSetup& setup, const DiscoveredTest& test,
                                            FrameworkProfile profile,
                                            std::function<void(Result<std::vector<FrameworkCase>>)> done) {
    if (auto valid = checkSetup(setup); !valid)
        return valid;
    if (auto valid = checkProgram(test); !valid)
        return valid;
    if (auto arguments = frameworkArguments(profile, test.command, {}, "report", true); !arguments)
        return std::unexpected(arguments.error());
    return startJob(
        *m_state, m_state,
        [setup, test, profile, done = std::move(done)](State& state, std::stop_token token, std::uint64_t job,
                                                       const std::function<void()>& finish) {
            Result<std::vector<FrameworkCase>> result = fail(ErrorCode::Cancelled, "Framework listing stopped");
            const auto path = state.scratchPath(setup, job, "framework-list.txt");
            if (auto made = state.makeScratchDirectory(setup); !made)
                result = std::unexpected(made.error());
            else {
                auto arguments = frameworkArguments(profile, test.command, {}, path, true);
                auto outcome = state.runProcess(doctestSpec(setup, test, std::move(*arguments)), token, {});
                if (!outcome)
                    result = std::unexpected(outcome.error());
                else if (outcome->exit.stopped || token.stop_requested())
                    result = fail(ErrorCode::Cancelled, "Framework listing stopped");
                else if (outcome->exit.waitFailed || outcome->exit.exitCode != 0)
                    result = fail(ErrorCode::IoError, toolFailure(test.command.front(), outcome->exit.exitCode,
                                                                  outcome->out, outcome->err));
                else if (auto text = state.fs.readFile(path); !text)
                    result = fail(ErrorCode::Corrupt, "Framework wrote no case listing");
                else
                    result = parseFrameworkList(profile, *text);
            }
            static_cast<void>(state.fs.remove(path));
            finish();
            if (done)
                done(std::move(result));
        });
}

Result<void> TestRunner::runFramework(const CTestSetup& setup, std::vector<FrameworkRunRequest> requests,
                                      TestRunHandlers handlers) {
    for (const auto& request : requests)
        if (auto arguments = frameworkArguments(request.profile, request.test.command, request.cases, "report", false);
            !arguments)
            return std::unexpected(arguments.error());
    auto logs = std::make_shared<std::map<std::string, std::string>>();
    auto state = m_state;
    return runDirectRequests(
        *m_state, m_state, setup, std::move(requests), std::move(handlers), "framework-run.xml", "framework",
        [logs](const CTestSetup& setup, const FrameworkRunRequest& request, std::string_view path) {
            (*logs)[request.test.name] = std::string(path) + ".log";
            auto arguments = frameworkArguments(request.profile, request.test.command, request.cases, path, false);
            auto spec = doctestSpec(setup, request.test, std::move(*arguments));
            // Inherited runtime controls must not override the explicitly selected protocol/filter.
            if (!request.cases.empty())
                spec.environment.emplace_back("BOOST_TEST_RUN_FILTERS", std::nullopt);
            for (const auto* key : {"BOOST_TEST_REPORT_FORMAT", "BOOST_TEST_REPORT_SINK", "BOOST_TEST_LOG_FORMAT",
                                    "BOOST_TEST_LOG_SINK", "BOOST_TEST_LOGGER", "BOOST_TEST_LIST_CONTENT"})
                spec.environment.emplace_back(key, std::nullopt);
            return spec;
        },
        [logs, state](const FrameworkRunRequest& request, const std::string& name, std::optional<std::string_view> text,
                      const ProcessOutcome& outcome, bool, std::string& error) -> std::optional<TestResult> {
            std::string log;
            if (request.profile == FrameworkProfile::BoostTest185) {
                const auto path = logs->at(name);
                if (auto bytes = state->fs.readFile(path))
                    log = std::move(*bytes);
                static_cast<void>(state->fs.remove(path));
            }
            if (!text || outcome.exit.waitFailed) {
                error = "Missing framework report or failed process wait";
                return {};
            }
            auto parsed = parseFrameworkRun(request.profile, name, *text, log, request.cases, outcome.exit.exitCode,
                                            outcome.out + outcome.err);
            if (!parsed) {
                error = parsed.error().message();
                return {};
            }
            return std::move(*parsed);
        });
}

} // namespace frost::run
