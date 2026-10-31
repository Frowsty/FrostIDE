// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the real IProcessLauncher, driving io_test_child (see
// io_test_child.cpp): exact argv round trips (spaces, quotes, backslashes,
// empty and non-ASCII arguments), exit codes, stdin piping, more than 1 MB on
// both output pipes at once, timeouts with termination, environment overrides,
// working directories, handle inheritance and the absolute-path rule.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>

#include <csignal>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;
using frost::ErrorCode;
using frost::io::ProcessResult;
using frost::io::ProcessSpec;
using frost::io::Stdio;
using frost::test::ScratchDir;

// The helper's absolute path, normalised (CMake hands it over with forward slashes).
std::string childPath() { return frost::io::normalizePath(FROST_IO_TEST_CHILD).value(); }

ProcessSpec childSpec(std::vector<std::string> arguments) {
    ProcessSpec spec;
    spec.executable = childPath();
    spec.arguments = std::move(arguments);
    return spec;
}

frost::Result<ProcessResult> run(const ProcessSpec& spec, std::string_view stdinData = {},
                                 frost::IClock::Duration timeout = 120s) {
    return frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, stdinData, timeout);
}

std::string unhex(std::string_view hex) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out += static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1]));
    }
    return out;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        out.push_back(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
    return out;
}

// The arguments the child reported under `header` ("crt" or "shell").
std::vector<std::string> reportedArguments(std::string_view output, std::string_view header) {
    const std::vector<std::string_view> all = lines(output);
    std::vector<std::string> args;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].starts_with(header) && all[i].size() > header.size() && all[i][header.size()] == ' ') {
            const std::size_t count = std::stoul(std::string(all[i].substr(header.size() + 1)));
            for (std::size_t k = 1; k <= count && i + k < all.size(); ++k) {
                args.push_back(unhex(all[i + k].substr(4))); // Strip "arg:".
            }
        }
    }
    return args;
}

// The single "<prefix>:<hex>" value the child printed, decoded; "<unset>" for "unset".
std::string reportedValue(std::string_view output, std::string_view prefix) {
    for (const std::string_view line : lines(output)) {
        if (line == "unset") {
            return "<unset>";
        }
        if (line.starts_with(prefix) && line.size() > prefix.size() && line[prefix.size()] == ':') {
            return unhex(line.substr(prefix.size() + 1));
        }
    }
    return "<missing>";
}

std::string pattern(std::size_t size) {
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<char>(i % 251);
    }
    return out;
}

void setParentVariable(const char* name, const char* value) {
#ifdef _WIN32
    const std::wstring wideName = frost::io::toWide(name).value();
    const std::wstring wideValue = frost::io::toWide(value).value();
    ASSERT_NE(SetEnvironmentVariableW(wideName.c_str(), wideValue.c_str()), FALSE);
#else
    ASSERT_EQ(setenv(name, value, 1), 0);
#endif
}

TEST(RealProcessLauncher, RelativeExecutablesAreRejected) {
    for (const char* relative : {"io_test_child", "./io_test_child", "bin/io_test_child"}) {
        ProcessSpec spec;
        spec.executable = relative;
        const auto result = frost::io::realProcessLauncher().launch(spec);
        ASSERT_FALSE(result.has_value()) << relative;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(RealProcessLauncher, MissingExecutableIsNotFound) {
    const ScratchDir dir("proc-missing");
    ProcessSpec spec;
    spec.executable = dir.join("no-such-tool.exe");
    const auto result = run(spec);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound) << result.error().describe();
}

TEST(RealProcessLauncher, ArgumentsRoundTripExactly) {
    const std::vector<std::string> tricky = {
        "plain",
        "with space",
        "tab\there",
        "quote\"inside",
        "\"",
        "\"\"",
        "\\\"",
        "trailing\\",
        "trailing two\\\\",
        "C:\\Program Files\\dir\\",
        "\\\\server\\share\\",
        "",
        "  ",
        "a\\\\\\\"b",
        "new\nline",
        "ünïcødé 测试 😀",
        "%PATH% $HOME `x` & | < > ^ ! ' ;",
        "*?[]{}",
        "",
    };
    std::vector<std::string> arguments{"argv"};
    arguments.insert(arguments.end(), tricky.begin(), tricky.end());
    const auto result = run(childSpec(arguments));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), tricky) << result->stdoutBytes;
#ifdef _WIN32
    // The same command line read by CommandLineToArgvW, the other common Windows parser.
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "shell"), tricky) << result->stdoutBytes;
#endif
}

TEST(RealProcessLauncher, ExecutableInADirectoryWithSpacesAndNonAscii) {
    const ScratchDir dir("proc-exe ünï 测试");
    std::error_code error;
    const std::filesystem::path source = frost::io::toStdPath(childPath()).value();
    const std::filesystem::path target =
        frost::io::toStdPath(dir.join("child copy ✓" + source.extension().string())).value();
    std::filesystem::copy_file(source, target, error);
    ASSERT_FALSE(error) << error.message();
    ProcessSpec spec = childSpec({"argv", "x y"});
    spec.executable = frost::io::fromStdPath(target).value();
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), (std::vector<std::string>{"x y"}));
}

// A directory under `root` whose path is longer than Windows' MAX_PATH (260 UTF-16 units).
std::string deepDirectory(const std::string& root) {
    std::string deep = root;
    while (deep.size() < 300) {
        deep = frost::io::joinPath(deep, std::string(40, 'x') + " segment").value();
    }
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(deep).has_value());
    return deep;
}

TEST(RealProcessLauncher, ExecutableBeyondMaxPath) {
    // Must work even with LongPathsEnabled = 0, as on the reference machine.
    const ScratchDir dir("proc-long-exe");
    const std::string deep = deepDirectory(dir.path());
#ifdef _WIN32
    const std::string copy = frost::io::joinPath(deep, "child ✓.exe").value();
#else
    const std::string copy = frost::io::joinPath(deep, "child ✓").value();
#endif
    const auto image = frost::io::realFileSystem().readFile(childPath());
    ASSERT_TRUE(image.has_value()) << image.error().describe();
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(copy, *image).has_value());
#ifndef _WIN32
    ASSERT_EQ(chmod(copy.c_str(), 0755), 0);
#endif
    ASSERT_GT(copy.size(), 300u);
    ProcessSpec spec = childSpec({"argv", "long"});
    spec.executable = copy;
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), (std::vector<std::string>{"long"}));
}

TEST(RealProcessLauncher, WorkingDirectoryBeyondMaxPath) {
    const ScratchDir dir("proc-long-cwd");
    const std::string deep = deepDirectory(dir.path());
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = deep;
    const auto result = run(spec);
#ifdef _WIN32
    // Windows refuses such a current directory unless long paths are enabled system-wide
    // (see workingDirectoryError in process_win.cpp); the error must say so, not claim
    // the directory is missing.
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
        EXPECT_NE(result.error().message().find("MAX_PATH"), std::string::npos) << result.error().describe();
        return;
    }
#endif
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(frost::io::normalizePath(reportedValue(result->stdoutBytes, "cwd")).value(), deep) << result->stdoutBytes;
}

TEST(RealProcessLauncher, WorkingDirectoryThatIsAFile) {
    const ScratchDir dir("proc-cwd-file");
    const std::string file = dir.join("not-a-dir");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(file, "x").has_value());
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = file;
    const auto result = run(spec);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    EXPECT_NE(result.error().message().find("is not a directory"), std::string::npos) << result.error().describe();
}

TEST(RealProcessLauncher, ExitCodes) {
    for (const int code : {0, 1, 3, 42, 255}) {
        const auto result = run(childSpec({"exit", std::to_string(code)}));
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_EQ(result->exitCode, code);
        EXPECT_EQ(result->signal, 0);
    }
}

TEST(RealProcessLauncher, StdinIsPiped) {
    ProcessSpec spec = childSpec({"cat"});
    spec.stdinMode = Stdio::Pipe;
    const std::string input = "first line\nsecond line\r\n\0\xff end"s; // The s suffix keeps the NUL.
    const auto result = run(spec, input);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, input);
    EXPECT_EQ(result->exitCode, 0);
}

TEST(RealProcessLauncher, MoreThanOneMegabyteOnBothPipes) {
    // The child alternates 4 KiB chunks between stdout and stderr: draining one pipe at a
    // time would deadlock as soon as the other pipe's buffer fills.
    constexpr std::size_t kSize = 3 * 1024 * 1024;
    const auto result = run(childSpec({"flood", std::to_string(kSize)}));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    ASSERT_EQ(result->stdoutBytes.size(), kSize);
    ASSERT_EQ(result->stderrBytes.size(), kSize);
    const std::string expected = pattern(kSize);
    EXPECT_TRUE(result->stdoutBytes == expected);
    EXPECT_TRUE(result->stderrBytes == expected);
}

TEST(RealProcessLauncher, LargeStdinEchoedOnBothPipes) {
    // Feeding stdin while both outputs fill: stdin, stdout and stderr all exceed the pipe buffers.
    ProcessSpec spec = childSpec({"tee"});
    spec.stdinMode = Stdio::Pipe;
    const std::string input = pattern(2 * 1024 * 1024 + 17);
    const auto result = run(spec, input);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_TRUE(result->stdoutBytes == input);
    EXPECT_TRUE(result->stderrBytes == input);
}

TEST(RealProcessLauncher, ChildThatIgnoresStdinDoesNotBreakCommunicate) {
    // The child exits without reading; the broken pipe must not fail the call (or raise SIGPIPE).
    ProcessSpec spec = childSpec({"exit", "7"});
    spec.stdinMode = Stdio::Pipe;
    const auto result = run(spec, pattern(4 * 1024 * 1024));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 7);
}

TEST(RealProcessLauncher, TimeoutTerminatesTheProcess) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = run(childSpec({"sleep", "60000"}), {}, 300ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout) << result.error().describe();
    EXPECT_LT(elapsed, 20s);
}

TEST(RealProcessLauncher, WaitTimeoutThenTerminate) {
    auto process = frost::io::realProcessLauncher().launch(childSpec({"sleep", "60000"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    EXPECT_GT((*process)->pid(), 0u);
    const auto early = (*process)->wait(50ms);
    ASSERT_FALSE(early.has_value());
    EXPECT_EQ(early.error().code(), ErrorCode::Timeout);
    ASSERT_TRUE((*process)->terminate().has_value());
    const auto status = (*process)->wait(30s);
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
#ifndef _WIN32
    EXPECT_EQ(status->signal, SIGKILL);
#endif
    // terminate() after exit is harmless, and wait() keeps answering.
    EXPECT_TRUE((*process)->terminate().has_value());
    EXPECT_EQ((*process)->wait().value(), *status);
}

TEST(RealProcessLauncher, StreamingPipes) {
    ProcessSpec spec = childSpec({"cat"});
    spec.stdinMode = Stdio::Pipe;
    auto process = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    ASSERT_TRUE((*process)->writeStdin("hello ").has_value());
    ASSERT_TRUE((*process)->writeStdin("world").has_value());
    (*process)->closeStdin();
    (*process)->closeStdin(); // Idempotent.
    EXPECT_EQ((*process)->writeStdin("late").error().code(), ErrorCode::InvalidArgument);
    std::string out;
    std::array<char, 3> buffer{};
    for (;;) {
        const auto got = (*process)->readStdout(buffer);
        ASSERT_TRUE(got.has_value()) << got.error().describe();
        if (*got == 0) {
            break;
        }
        out.append(buffer.data(), *got);
    }
    EXPECT_EQ(out, "hello world");
    EXPECT_EQ((*process)->readStderr(buffer).value(), 0u);
    EXPECT_EQ((*process)->wait(30s).value().exitCode, 0);
}

TEST(RealProcessLauncher, StdioModes) {
    ProcessSpec merged = childSpec({"print", "OUT", "ERR"});
    merged.stderrMode = Stdio::MergeIntoStdout;
    auto result = run(merged);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "OUTERR");
    EXPECT_EQ(result->stderrBytes, "");

    ProcessSpec nulled = childSpec({"print", "OUT", "ERR"});
    nulled.stdoutMode = Stdio::Null;
    result = run(nulled);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "");
    EXPECT_EQ(result->stderrBytes, "ERR");

    ProcessSpec nullInput = childSpec({"cat"});
    nullInput.stdinMode = Stdio::Null;
    result = run(nullInput);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "");

    ProcessSpec inherited = childSpec({"exit", "0"});
    inherited.stdinMode = Stdio::Inherit;
    inherited.stdoutMode = Stdio::Inherit;
    inherited.stderrMode = Stdio::Inherit;
    result = run(inherited);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);

    // Data for a stdin that is not a pipe is a caller bug.
    result = run(childSpec({"exit", "0"}), "data");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(RealProcessLauncher, EnvironmentOverrides) {
    setParentVariable("FROST_IO_TEST_INHERITED", "from parent");
    ProcessSpec spec = childSpec({"env", "FROST_IO_TEST_INHERITED"});
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "from parent");

    spec.environment = {{"FROST_IO_TEST_INHERITED", "välue ✓ with = sign"}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "välue ✓ with = sign");

    spec.environment = {{"FROST_IO_TEST_INHERITED", std::nullopt}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");

    // Later overrides win.
    spec.environment = {{"FROST_IO_TEST_INHERITED", "first"}, {"FROST_IO_TEST_INHERITED", "second"}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "second");

#ifdef _WIN32
    // Windows treats variable names case-insensitively, so must the overrides.
    spec.environment = {{"frost_io_test_inherited", std::nullopt}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");
#endif
}

TEST(RealProcessLauncher, CleanEnvironment) {
    setParentVariable("FROST_IO_TEST_INHERITED", "from parent");
    ProcessSpec spec = childSpec({"env", "FROST_IO_TEST_INHERITED"});
    spec.cleanEnvironment = true;
    spec.environment = {{"ONLY_THIS", "1"}};
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");

    spec.arguments = {"env", "ONLY_THIS"};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "1");

    spec.arguments = {"env", "PATH"};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");
}

TEST(RealProcessLauncher, WorkingDirectory) {
    const ScratchDir dir("proc-cwd ünïcødé 测试");
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = dir.path();
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(frost::io::normalizePath(reportedValue(result->stdoutBytes, "cwd")).value(), dir.path());

    spec.workingDirectory = dir.join("does-not-exist");
    const auto missing = run(spec);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound) << missing.error().describe();

    spec.workingDirectory = "relative";
    EXPECT_EQ(run(spec).error().code(), ErrorCode::InvalidArgument);
}

#ifndef _WIN32
// The probe identity of an open descriptor: "<st_dev>:<st_ino>".
std::string fileIdentity(int fd) {
    struct stat info{};
    EXPECT_EQ(fstat(fd, &info), 0);
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
}
#endif

TEST(RealProcessLauncher, OnlyTheStandardHandlesAreInherited) {
    // The child compares object identity, not just handle validity (see io_test_child.cpp),
    // so its own handles with the same numeric value cannot fake a result.
#ifdef _WIN32
    // An inheritable handle that a plain CreateProcessW(bInheritHandles=TRUE) would pass on,
    // to an event whose unique name lets the child find the same object independently.
    const std::string name = std::format("Local\\frost-io-probe-{}-{}", GetCurrentProcessId(),
                                         std::chrono::steady_clock::now().time_since_epoch().count());
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE event = CreateEventW(&inherit, TRUE, FALSE, std::wstring(name.begin(), name.end()).c_str());
    ASSERT_NE(event, nullptr);
    const auto result = run(childSpec({"probe-handle", std::to_string(reinterpret_cast<std::uintptr_t>(event)), name}));

    // Control: the probe recognises the event when the child really has it, here as its
    // inherited stdin (Stdio::Inherit duplicates the parent's standard input handle).
    const HANDLE savedStdin = GetStdHandle(STD_INPUT_HANDLE);
    ASSERT_NE(SetStdHandle(STD_INPUT_HANDLE, event), FALSE);
    ProcessSpec controlSpec = childSpec({"probe-handle", "stdin", name});
    controlSpec.stdinMode = Stdio::Inherit;
    const auto control = run(controlSpec);
    SetStdHandle(STD_INPUT_HANDLE, savedStdin);
    CloseHandle(event);
    ASSERT_TRUE(control.has_value()) << control.error().describe();
    EXPECT_EQ(control->stdoutBytes, "inherited\n");
#else
    // A descriptor without O_CLOEXEC, as careless code elsewhere in the process might open.
    const ScratchDir dir("proc-probe");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join("probe"), "").has_value());
    const int fd = open(dir.join("probe").c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const auto result = run(childSpec({"probe-handle", std::to_string(fd), fileIdentity(fd)}));
    close(fd);
#endif
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "not inherited\n");
#ifndef _WIN32
    // Control: the probe recognises a descriptor the child really has (stdin = /dev/null).
    const int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(null, 0);
    const std::string nullIdentity = fileIdentity(null);
    close(null);
    const auto control = run(childSpec({"probe-handle", "stdin", nullIdentity}));
    ASSERT_TRUE(control.has_value()) << control.error().describe();
    EXPECT_EQ(control->stdoutBytes, "inherited\n");
#endif
}

#ifdef _WIN32
TEST(RealProcessLauncher, BatchFilesAreRefused) {
    const ScratchDir dir("proc-bat");
    for (const char* name : {"build.bat", "BUILD.CMD", "run.cmd. "}) {
        ProcessSpec spec;
        spec.executable = dir.join(name);
        const auto result = frost::io::realProcessLauncher().launch(spec);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported) << name;
    }
}
#endif

} // namespace
