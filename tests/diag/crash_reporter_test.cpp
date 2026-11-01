// SPDX-License-Identifier: Apache-2.0
//
// The crashpad reporter's start-up logic (frost/diag/crash_reporter.hpp) with
// io::ScriptedProcessLauncher, a connector double and ManualClock: the handler's command line
// and environment goldens (no --url, --no-periodic-tasks, the \\?\ database, the pipe name
// from 128 random bits, fd 3 on Linux), the launch spec (killOnClose, the extended image path,
// null standard streams, a clean environment), the readiness wait's backoff, and every error:
// no handler (NotFound), a handler that exits early (IoError with its status), a pipe that
// never appears (Timeout, bounded by the virtual clock; the handler is terminated), a failed
// registration, an elevated process (Unsupported, nothing launched). Also the in-memory double
// and the contract violation handler's routing, as death tests.

#include "frost/core/assert.hpp"
#include "frost/core/clock.hpp"
#include "frost/diag/crash_reporter.hpp"
#include "frost/diag/fatal_handlers.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/io/utf.hpp"

#include "support/crash_dialogs.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <regex>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::diag::CrashReporterConfig;

constexpr bool kWindows = frost::io::kWindowsProcessRules;

class FakeConnector final : public frost::diag::ICrashpadConnector {
public:
    explicit FakeConnector(frost::ManualClock& clock) : m_clock(clock) {}

    int readyAfter = 0; // pipeReady() calls that fail first; < 0: never ready.
    bool registers = true;
    bool elevated = false;
    int probes = 0;
    std::vector<frost::IClock::Duration> sleeps;
    std::wstring registeredPipe;
    frost::io::ProcessId registeredPid = 0;

    bool pipeReady(const std::wstring&) noexcept override { return readyAfter >= 0 && probes++ >= readyAfter; }
    bool registerPipe(const std::wstring& pipeName) noexcept override {
        registeredPipe = pipeName;
        return registers;
    }
    bool registerSocket(int socket, frost::io::ProcessId handlerPid) noexcept override {
#ifndef _WIN32
        ::close(socket); // The real connector hands it to crashpad.
#else
        static_cast<void>(socket);
#endif
        registeredPid = handlerPid;
        return registers;
    }
    void sleepFor(frost::IClock::Duration delay) noexcept override {
        sleeps.push_back(delay);
        m_clock.advance(delay);
    }
    bool processElevated() noexcept override { return elevated; }

private:
    frost::ManualClock& m_clock;
};

CrashReporterConfig config() {
    CrashReporterConfig config;
    config.handlerPath = kWindows ? "C:\\frost\\crashpad_handler.exe" : "/opt/frost/crashpad_handler";
    config.databasePath = kWindows ? "C:\\cache\\crashes\\crashpad" : "/cache/crashes/crashpad";
    config.version = "1.2.3";
    return config;
}

struct Rig {
    frost::ManualClock clock;
    frost::io::ScriptedProcessLauncher launcher;
    FakeConnector connector{clock};
    std::unique_ptr<frost::diag::ICrashReporter> reporter =
        frost::diag::createCrashpadReporter(launcher, clock, connector);
};

TEST(CrashpadReporter, HandlerArgumentsGolden) {
    const CrashReporterConfig windows{"C:\\f\\crashpad_handler.exe", "C:\\c\\crashes\\crashpad", "1.2.3"};
    EXPECT_EQ(
        frost::diag::crashpadHandlerArguments(windows, "\\\\.\\pipe\\frost-crashpad-1-ab", true),
        (std::vector<std::string>{"--database=\\\\?\\C:\\c\\crashes\\crashpad",
                                  "--pipe-name=\\\\.\\pipe\\frost-crashpad-1-ab", "--no-periodic-tasks",
                                  "--annotation=prod=FrostIDE", "--annotation=ver=1.2.3", "--annotation=plat=win"}));
    const CrashReporterConfig linux{"/f/crashpad_handler", "/c/crashes/crashpad", "1.2.3"};
    EXPECT_EQ(
        frost::diag::crashpadHandlerArguments(linux, "", false),
        (std::vector<std::string>{"--database=/c/crashes/crashpad", "--initial-client-fd=3",
                                  "--shared-client-connection", "--no-periodic-tasks", "--annotation=prod=FrostIDE",
                                  "--annotation=ver=1.2.3", "--annotation=plat=linux"}));
}

TEST(CrashpadReporter, HandlerEnvironmentKeepsOnlyTheListedVariables) {
    const auto parent = [](std::string_view name) -> std::optional<std::string> {
        if (name == "PATH" || name == "TMPDIR" || name == "SystemRoot" || name == "TEMP" || name == "ASAN_OPTIONS") {
            return "v-" + std::string(name);
        }
        return std::nullopt;
    };
    using Env = std::vector<std::pair<std::string, std::optional<std::string>>>;
    EXPECT_EQ(frost::diag::crashpadHandlerEnvironment(parent, true),
              (Env{{"SystemRoot", "v-SystemRoot"}, {"TEMP", "v-TEMP"}, {"ASAN_OPTIONS", "v-ASAN_OPTIONS"}}));
    EXPECT_EQ(frost::diag::crashpadHandlerEnvironment(parent, false),
              (Env{{"TMPDIR", "v-TMPDIR"}, {"ASAN_OPTIONS", "v-ASAN_OPTIONS"}}));
}

TEST(CrashpadReporter, StartsTheHandlerThroughTheLauncher) {
    Rig rig;
    rig.connector.readyAfter = 4;
    rig.launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    const auto started = rig.reporter->start(config());
    ASSERT_TRUE(started.has_value()) << started.error().describe();
    EXPECT_TRUE(rig.reporter->active());
    const auto launches = rig.launcher.launches();
    ASSERT_EQ(launches.size(), 1u);
    const frost::io::ProcessSpec& spec = launches[0].spec;
    EXPECT_EQ(spec.executable, config().handlerPath);
    EXPECT_TRUE(spec.killOnClose);
    EXPECT_TRUE(spec.cleanEnvironment);
    EXPECT_EQ(spec.stdinMode, frost::io::Stdio::Null);
    EXPECT_EQ(spec.stdoutMode, frost::io::Stdio::Null);
    EXPECT_EQ(spec.stderrMode, frost::io::Stdio::Null);
    for (const std::string& argument : spec.arguments) {
        EXPECT_FALSE(argument.starts_with("--url")) << argument; // D26: nothing is uploaded.
    }
    const frost::diag::ReporterStatus status = rig.reporter->status();
    EXPECT_TRUE(status.active);
    EXPECT_EQ(status.handlerPath, config().handlerPath);
    if constexpr (kWindows) {
        EXPECT_EQ(spec.imagePathForm, frost::io::ImagePathForm::Extended);
        EXPECT_EQ(spec.arguments[0], "--database=\\\\?\\C:\\cache\\crashes\\crashpad");
        const std::regex pipe(R"(\\\\\.\\pipe\\frost-crashpad-\d+-[0-9a-f]{32})");
        EXPECT_TRUE(std::regex_match(status.pipeName, pipe)) << status.pipeName;
        EXPECT_EQ(spec.arguments[1], "--pipe-name=" + status.pipeName);
#ifdef _WIN32 // fromWide is Windows-only; `if constexpr` outside a template still compiles both arms.
        EXPECT_EQ(frost::io::fromWide(rig.connector.registeredPipe).value(), status.pipeName);
#endif
        // Backoff 1, 2, 4, 8 ms between the five probes.
        EXPECT_EQ(rig.connector.sleeps, (std::vector<frost::IClock::Duration>{1ms, 2ms, 4ms, 8ms}));
        // Two starts never share a pipe name (128 random bits each).
        Rig other;
        other.launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
        ASSERT_TRUE(other.reporter->start(config()).has_value());
        EXPECT_NE(other.reporter->status().pipeName, status.pipeName);
    } else {
        ASSERT_EQ(spec.posixInheritedFds.size(), 1u);
        EXPECT_GE(spec.posixInheritedFds[0], 0);
        EXPECT_EQ(rig.connector.registeredPid, status.handlerPid);
        EXPECT_TRUE(rig.connector.sleeps.empty()); // The socket needs no readiness wait.
    }
    // Started once.
    EXPECT_EQ(rig.reporter->start(config()).error().code(), ErrorCode::InvalidArgument);
}

TEST(CrashpadReporter, MissingHandlerIsNotFound) {
    Rig rig;
    rig.launcher.expect(frost::io::ScriptedProcess{.launchError = frost::Error(ErrorCode::NotFound, "no such file")});
    const auto started = rig.reporter->start(config());
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(rig.reporter->active());
}

TEST(CrashpadReporter, HandlerThatExitsEarlyIsAnIoErrorWithItsStatus) {
    Rig rig;
    rig.connector.readyAfter = -1;
    rig.launcher.expect(frost::io::ScriptedProcess{.exitCode = 0x2A});
    const auto started = rig.reporter->start(config());
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::IoError);
    EXPECT_TRUE(started.error().describe().contains(kWindows ? "0x0000002a" : "42")) << started.error().describe();
}

TEST(CrashpadReporter, PipeThatNeverAppearsTimesOutAndStopsTheHandler) {
    if constexpr (!kWindows) {
        GTEST_SKIP() << "Linux registers on a socket and has no readiness wait";
    }
    Rig rig;
    rig.connector.readyAfter = -1;
    rig.launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    CrashReporterConfig slow = config();
    slow.readyTimeout = 200ms;
    const auto begin = rig.clock.steadyNow();
    const auto started = rig.reporter->start(slow);
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::Timeout);
    // Bounded: at most one 50 ms backoff step past the deadline.
    EXPECT_LE(rig.clock.steadyNow() - begin, 250ms);
    EXPECT_GE(rig.clock.steadyNow() - begin, 200ms);
    EXPECT_TRUE(rig.launcher.launches()[0].terminated);
    for (const auto delay : rig.connector.sleeps) {
        EXPECT_LE(delay, 50ms);
    }
}

TEST(CrashpadReporter, FailedRegistrationStopsTheHandler) {
    Rig rig;
    rig.connector.registers = false;
    rig.launcher.expect(frost::io::ScriptedProcess{.neverExits = true});
    const auto started = rig.reporter->start(config());
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::IoError);
    EXPECT_TRUE(rig.launcher.launches()[0].terminated);
}

TEST(CrashpadReporter, ElevatedProcessStartsNoHandler) {
    Rig rig;
    rig.connector.elevated = true;
    const auto started = rig.reporter->start(config());
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(rig.launcher.launches().empty());
}

TEST(CrashpadReporter, RelativePathsAreRefused) {
    Rig rig;
    CrashReporterConfig relative = config();
    relative.handlerPath = "crashpad_handler";
    EXPECT_EQ(rig.reporter->start(relative).error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(rig.reporter->dumpWithoutCrash("x").has_value());
}

TEST(FatalReason, Names) {
    EXPECT_EQ(frost::diag::toString(frost::diag::FatalReason::ContractViolation), "contract-violation");
    EXPECT_EQ(frost::diag::toString(frost::diag::FatalReason::QtFatal), "qt-fatal");
    EXPECT_EQ(frost::diag::toString(frost::diag::FatalReason::PureCall), "purecall");
}

TEST(InMemoryCrashReporter, RecordsStartsAnnotationsAndDumps) {
    frost::diag::InMemoryCrashReporter reporter;
    EXPECT_FALSE(reporter.active());
    ASSERT_TRUE(reporter.start(config()).has_value());
    EXPECT_TRUE(reporter.active());
    reporter.annotate(frost::diag::AnnotationKey::ImagePath, "a");
    reporter.annotate(frost::diag::AnnotationKey::ImagePath, "b");
    EXPECT_EQ(reporter.annotation(frost::diag::AnnotationKey::ImagePath), "b");
    ASSERT_TRUE(reporter.dumpWithoutCrash("why").has_value());
    EXPECT_EQ(reporter.dumps(), std::vector<std::string>{"why"});
    frost::diag::InMemoryCrashReporter failing;
    failing.failWith = ErrorCode::Timeout;
    EXPECT_EQ(failing.start(config()).error().code(), ErrorCode::Timeout);
}

[[clang::noinline]] int violate(int value) {
    FROST_PRE(value >= 0);
    return value;
}

TEST(FatalHandlersDeathTest, EnforcedViolationReachesTheReporter) {
    // installFatalHandlers routes an enforced FROST_PRE through crashNow(ContractViolation);
    // the in-memory double exits with kInMemoryFatalExit, naming the expression.
    EXPECT_EXIT(
        {
            frost::test::disableCrashDialogs();
            static frost::diag::InMemoryCrashReporter reporter;
            frost::diag::installFatalHandlers(reporter);
            (void)violate(-1);
            std::exit(0);
        },
        testing::ExitedWithCode(frost::diag::kInMemoryFatalExit),
        "fatal contract-violation: precondition violated: value >= 0");
}

TEST(FatalHandlersDeathTest, TerminateReachesTheReporter) {
    EXPECT_EXIT(
        {
            frost::test::disableCrashDialogs();
            static frost::diag::InMemoryCrashReporter reporter;
            frost::diag::installFatalHandlers(reporter);
            std::terminate();
        },
        testing::ExitedWithCode(frost::diag::kInMemoryFatalExit), "fatal terminate: std::terminate called");
}

TEST(FatalHandlers, ObservedViolationsOnlyLog) {
    // A handler call with terminating == false returns (observe semantic).
    const frost::contracts::Violation violation{frost::contracts::Kind::Assertion, "x",
                                                std::source_location::current()};
    frost::diag::contractViolationHandler(violation);
    SUCCEED();
}

} // namespace
