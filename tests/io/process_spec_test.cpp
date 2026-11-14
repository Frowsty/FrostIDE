// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the platform-independent process rules: ProcessSpec
// validation (absolute executables only, batch files refused, NUL and
// environment checks) and Windows command-line quoting, checked against an
// independent parser written from Microsoft's documented rules and, on Windows,
// against CommandLineToArgvW itself.

#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
// CommandLineToArgvW; shellapi.h needs windows.h first.
#include <shellapi.h>
#endif

namespace {

using frost::ErrorCode;
using frost::io::ProcessSpec;
using frost::io::Stdio;

ProcessSpec specFor(std::string executable) {
    ProcessSpec spec;
    spec.executable = std::move(executable);
    return spec;
}

ErrorCode validate(const ProcessSpec& spec, bool windowsRules) {
    const auto result = frost::io::validateProcessSpec(spec, windowsRules);
    return result ? ErrorCode::Unknown : result.error().code();
}

TEST(ProcessSpec, AbsoluteExecutablesAreAccepted) {
    EXPECT_TRUE(frost::io::validateProcessSpec(specFor("/usr/bin/git"), false).has_value());
    EXPECT_TRUE(frost::io::validateProcessSpec(specFor("C:\\Program Files\\Git\\cmd\\git.exe"), true).has_value());
    EXPECT_TRUE(frost::io::validateProcessSpec(specFor("C:/tools/cmake.exe"), true).has_value());
    EXPECT_TRUE(frost::io::validateProcessSpec(specFor("\\\\server\\share\\tool.exe"), true).has_value());
}

TEST(ProcessSpec, RelativeExecutablesAreRejected) {
    // PLAN.md 3.5: a bare name would be resolved through PATH or the current directory.
    for (const char* exe : {"git", "git.exe", "./git", "bin/git", "..\\git.exe", ""}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::InvalidArgument) << exe;
        EXPECT_EQ(validate(specFor(exe), false), ErrorCode::InvalidArgument) << exe;
    }
    EXPECT_EQ(validate(specFor("C:git.exe"), true), ErrorCode::InvalidArgument);           // Drive-relative.
    EXPECT_EQ(validate(specFor("\\tools\\git.exe"), true), ErrorCode::InvalidArgument);    // Current-drive-relative.
    EXPECT_EQ(validate(specFor("C:\\tools\\git.exe"), false), ErrorCode::InvalidArgument); // Not absolute on POSIX.
    const auto message = frost::io::validateProcessSpec(specFor("git"), true).error().message();
    EXPECT_NE(message.find("absolute"), std::string::npos) << message;
}

TEST(ProcessSpec, BatchFilesAreRefusedOnWindows) {
    // Every spelling that Win32 resolves to a .bat or .cmd file: the launcher starts the
    // normalised path, so trailing separators, "." and ".." must not hide the extension.
    for (const char* exe : {"C:\\x\\build.bat", "C:\\x\\BUILD.CMD", "C:\\x\\a.Bat", "C:\\x\\run.cmd. ",
                            "C:\\x\\run.bat...", "C:\\d\\t.bat\\", "C:/d/t.bat/", "C:\\d\\t.bat\\.",
                            "C:\\d\\t.bat\\x\\..", "C:\\d\\t.bat\\\\", "C:\\d\\t.bat\\.\\x\\..\\."}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::Unsupported) << exe;
    }
    // Device-namespace spellings are refused outright (the OS would normalise them), as a
    // batch file when the given spelling already says so.
    for (const char* exe : {"\\\\.\\C:\\d\\t.bat", "//./C:/d/t.bat"}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::Unsupported) << exe;
    }
    for (const char* exe : {"\\\\.\\C:\\d\\t.bat\\.", "\\\\.\\C:\\tools\\git.exe", "//./C:/tools/git.exe"}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::InvalidArgument) << exe;
    }
    const auto device = frost::io::validateProcessSpec(specFor("\\\\.\\C:\\d\\t.bat\\."), true).error().message();
    EXPECT_NE(device.find("device namespace"), std::string::npos) << device;
    // Not batch files: other extensions and extensionless names (CreateProcessW runs only
    // .bat and .cmd through cmd.exe; the integration test proves it).
    for (const char* exe : {"C:\\d\\t.batx", "C:\\d\\t.cmdline", "C:\\d\\tbat", "C:\\x\\batman.exe"}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::Unknown) << exe;
    }
    // Stream syntax names t.bat's own data stream; rather than relying on what CreateProcessW
    // makes of it, ':' is refused in every name, in either spelling.
    for (const char* exe : {"C:\\d\\t.bat::$DATA", "C:\\d\\t.bat:", "\\\\?\\C:\\d\\t.bat::$DATA", "C:\\d:x\\t.exe"}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::InvalidArgument) << exe;
    }
    EXPECT_EQ(validate(specFor("/x/build.bat"), false), ErrorCode::Unknown); // An ordinary file on POSIX.
}

TEST(ProcessSpec, WindowsNamesAreCheckedOnTheExecutableAndDirectory) {
    EXPECT_EQ(validate(specFor("C:\\tools\\CON.exe"), true), ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(specFor("C:\\tools.\\x.exe"), true), ErrorCode::InvalidArgument);
    ProcessSpec spec = specFor("C:\\tools\\x.exe");
    spec.workingDirectory = "C:\\work \\dir";
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
    spec.workingDirectory = "\\\\.\\C:\\work";
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
    spec.workingDirectory = "C:\\work\\dir";
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown);
}

// The run service's external console (ADR 0040): a verbatim command tail and a new console window.
TEST(ProcessSpec, RawArgumentsAndNewConsoleAreWindowsOnly) {
    ProcessSpec spec = specFor("C:\\Windows\\System32\\cmd.exe");
    spec.rawArguments = "/d /s /c \"\"C:\\a b\\x.exe\" 1 & pause\"";
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown);
    ProcessSpec posix = specFor("/bin/sh");
    posix.rawArguments = "-c true";
    EXPECT_EQ(validate(posix, false), ErrorCode::Unsupported);
    spec.arguments = {"/c"};
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument) << "rawArguments replaces arguments";
    spec.arguments.clear();
    spec.rawArguments = std::string("a\0b", 3);
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
    spec.rawArguments = std::string("\xFF");
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);

    // The tail counts towards CreateProcessW's limit (process_test.cpp checks what the child sees).
    spec.rawArguments = std::string(32767, 'a');
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
    spec.rawArguments.reset();

    // A new console takes the console's own streams: every Stdio must be Inherit.
    spec.newConsole = true;
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument) << "the defaults pipe stdout and stderr";
    spec.stdinMode = Stdio::Inherit;
    spec.stdoutMode = Stdio::Inherit;
    spec.stderrMode = Stdio::Inherit;
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown);
    posix.rawArguments.reset();
    posix.newConsole = true;
    posix.stdinMode = Stdio::Inherit;
    posix.stdoutMode = Stdio::Inherit;
    posix.stderrMode = Stdio::Inherit;
    EXPECT_EQ(validate(posix, false), ErrorCode::Unsupported);
    spec.stdoutMode = Stdio::Pipe;
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
    spec.stdoutMode = Stdio::Inherit;
    spec.stdinMode = Stdio::Null;
    EXPECT_EQ(validate(spec, true), ErrorCode::InvalidArgument);
}

TEST(ProcessSpec, WindowsCommandLineMustBeBuildable) {
    // argv[0] has no escape syntax, so a quote in the executable cannot be represented.
    EXPECT_EQ(validate(specFor("C:\\a\"b\\x.exe"), true), ErrorCode::InvalidArgument);
    EXPECT_EQ(validate(specFor("/a\"b/x"), false), ErrorCode::Unknown); // Just a character on POSIX.

    // CreateProcessW takes at most 32,767 UTF-16 units including the terminator.
    ProcessSpec spec = specFor("C:\\x.exe"); // Command line: "C:\x.exe" plus " " and the argument.
    const std::size_t fixed = std::string_view("\"C:\\x.exe\" ").size();
    spec.arguments = {std::string(32766 - fixed, 'a')};
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown); // 32,766 units: the longest allowed.
    spec.arguments = {std::string(32767 - fixed, 'a')};
    const auto tooLong = frost::io::validateProcessSpec(spec, true);
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_EQ(tooLong.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(tooLong.error().message().find("32766"), std::string::npos) << tooLong.error().message();
    // Units, not bytes: 16,000 three-byte characters (48,000 bytes) are 16,000 units.
    std::string wide;
    for (int i = 0; i < 16000; ++i) {
        wide += "\xE6\xB5\x8B"; // U+6D4B, one UTF-16 unit.
    }
    spec.arguments = {wide};
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown);
    // POSIX passes argv directly; CreateProcessW's limit does not apply.
    ProcessSpec posix = specFor("/bin/x");
    posix.arguments = {std::string(40000, 'a')};
    EXPECT_EQ(validate(posix, false), ErrorCode::Unknown);
}

TEST(ProcessSpec, OtherFieldsAreChecked) {
    ProcessSpec spec = specFor("/bin/tool");
    spec.workingDirectory = "relative/dir";
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.workingDirectory = "/abs/dir";
    EXPECT_EQ(validate(spec, false), ErrorCode::Unknown);

    spec.arguments = {"ok", std::string("bad\0arg", 7)};
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.arguments = {};

    for (const std::string& name : {std::string(), std::string("A=B"), std::string("A\0B", 3)}) {
        spec.environment = {{name, "v"}};
        EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    }
    spec.environment = {{"NAME", std::string("v\0", 2)}};
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.environment = {{"NAME", "value=with=equals"}, {"GONE", std::nullopt}};
    EXPECT_EQ(validate(spec, false), ErrorCode::Unknown);

    spec.stdinMode = Stdio::MergeIntoStdout;
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.stdinMode = Stdio::Null;
    spec.stdoutMode = Stdio::MergeIntoStdout;
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.stdoutMode = Stdio::Pipe;
    spec.stderrMode = Stdio::MergeIntoStdout;
    EXPECT_EQ(validate(spec, false), ErrorCode::Unknown);
}

TEST(ProcessSpec, ValidatesPosixInheritedFds) {
    ProcessSpec spec = specFor("/bin/tool");
    spec.posixInheritedFds = {7, 3, 0};
    EXPECT_EQ(validate(spec, false), ErrorCode::Unknown);
    spec.posixInheritedFds = {5, -1};
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.posixInheritedFds = {5, 6, 5};
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
    spec.posixInheritedFds.clear();
    for (int fd = 0; fd < static_cast<int>(frost::io::kMaxInheritedFds); ++fd) {
        spec.posixInheritedFds.push_back(100 + fd);
    }
    EXPECT_EQ(validate(spec, false), ErrorCode::Unknown);
    spec.posixInheritedFds.push_back(99);
    EXPECT_EQ(validate(spec, false), ErrorCode::InvalidArgument);
}

TEST(ProcessSpec, WindowsRejectsPosixInheritedFds) {
    ProcessSpec spec = specFor("C:\\tools\\tool.exe");
    EXPECT_EQ(validate(spec, true), ErrorCode::Unknown);
    spec.posixInheritedFds = {3};
    EXPECT_EQ(validate(spec, true), ErrorCode::Unsupported);
}

TEST(ProcessSpec, PseudoTerminalNeedsInheritedStreamsAndADevice) {
    // frost::term (ADR 0051): the child's streams are the terminal's.
    const auto onTerminal = [](std::string executable) {
        ProcessSpec spec = specFor(std::move(executable));
        spec.stdinMode = Stdio::Inherit;
        spec.stdoutMode = Stdio::Inherit;
        spec.stderrMode = Stdio::Inherit;
        spec.pseudoTerminal = frost::io::PseudoTerminal{};
        return spec;
    };
    ProcessSpec posix = onTerminal("/bin/sh");
    EXPECT_EQ(validate(posix, false), ErrorCode::InvalidArgument); // No device.
    posix.pseudoTerminal->posixTerminalPath = "/dev/pts/3";
    EXPECT_EQ(validate(posix, false), ErrorCode::Unknown);
    posix.pseudoTerminal->posixTerminalPath = "pts/3";
    EXPECT_EQ(validate(posix, false), ErrorCode::InvalidArgument);
    posix.pseudoTerminal->posixTerminalPath = "/dev/pts/3";
    posix.stdoutMode = Stdio::Pipe;
    EXPECT_EQ(validate(posix, false), ErrorCode::InvalidArgument);
    posix.stdoutMode = Stdio::Inherit;
    posix.killOnClose = false;
    EXPECT_EQ(validate(posix, false), ErrorCode::InvalidArgument);

    ProcessSpec windows = onTerminal("C:\\Windows\\System32\\cmd.exe");
    EXPECT_EQ(validate(windows, true), ErrorCode::InvalidArgument); // No pseudo console.
    int console = 0;
    windows.pseudoTerminal->windowsPseudoConsole = &console;
    EXPECT_EQ(validate(windows, true), ErrorCode::Unknown);
    windows.newConsole = true;
    EXPECT_EQ(validate(windows, true), ErrorCode::InvalidArgument);
}

TEST(ProcessSpec, DesktopNameIsCheckedOnWindowsOnly) {
    // STARTUPINFOW::lpDesktop: a NUL would cut the name short, and it must convert to UTF-16.
    // POSIX ignores the field, so it refuses nothing there.
    for (const std::string& name : {std::string("WinSta0\\a\0b", 11), std::string("WinSta0\\\xff")}) {
        ProcessSpec windows = specFor("C:\\tools\\git.exe");
        ProcessSpec posix = specFor("/usr/bin/git");
        windows.desktop = posix.desktop = name;
        EXPECT_EQ(validate(windows, true), ErrorCode::InvalidArgument);
        EXPECT_EQ(validate(posix, false), ErrorCode::Unknown);
    }
    ProcessSpec valid = specFor("C:\\tools\\git.exe");
    valid.desktop = "WinSta0\\frost-test-\xc3\xa4";
    EXPECT_EQ(validate(valid, true), ErrorCode::Unknown);
    // The image-path form needs no validation: every value is a valid request.
    for (const auto form :
         {frost::io::ImagePathForm::Auto, frost::io::ImagePathForm::Plain, frost::io::ImagePathForm::Extended}) {
        valid.imagePathForm = form;
        EXPECT_EQ(validate(valid, true), ErrorCode::Unknown);
    }
}

TEST(ProcessSpec, WindowsNeedsWellFormedUtf8EverywhereItConverts) {
    // CreateProcessW takes UTF-16, so the launcher converts the working directory and the
    // environment too; what it cannot convert is refused up front, as for the command line.
    // POSIX hands the bytes to the kernel unchanged.
    struct Case {
        std::string workingDirectory;
        std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    };
    for (const Case& c : {Case{"\\w\xff", {}}, Case{"", {{"LANG", "\xff"}}}, Case{"", {{"\xff", "v"}}},
                          Case{"", {{"A", "ok"}, {"B", "\xc3"}}}}) {
        ProcessSpec windows = specFor("C:\\tools\\git.exe");
        ProcessSpec posix = specFor("/usr/bin/git");
        if (!c.workingDirectory.empty()) {
            windows.workingDirectory = "C:" + c.workingDirectory;
            posix.workingDirectory = "/w\xff";
        }
        windows.environment = posix.environment = c.environment;
        EXPECT_EQ(validate(windows, true), ErrorCode::InvalidArgument) << windows.workingDirectory;
        EXPECT_EQ(validate(posix, false), ErrorCode::Unknown) << posix.workingDirectory;
    }
    ProcessSpec valid = specFor("C:\\tools\\git.exe");
    valid.workingDirectory = "C:\\w\xc3\xbc"; // U+00FC.
    valid.environment = {{"LANG", "\xe6\xb5\x8b"}, {"\xc3\xbc", std::nullopt}};
    EXPECT_EQ(validate(valid, true), ErrorCode::Unknown);
}

std::string quoted(std::string_view argument) {
    std::string out;
    frost::io::appendWindowsArgument(out, argument);
    return out;
}

TEST(WindowsQuoting, ExactEncodings) {
    EXPECT_EQ(quoted("abc"), "abc");
    EXPECT_EQ(quoted(""), "\"\"");
    EXPECT_EQ(quoted("a b"), "\"a b\"");
    EXPECT_EQ(quoted("a\tb"), "\"a\tb\"");
    EXPECT_EQ(quoted("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(quoted("a\\b"), "a\\b");         // Backslashes not before a quote stay literal.
    EXPECT_EQ(quoted("a\\"), "a\\");           // ... even at the end of an unquoted argument.
    EXPECT_EQ(quoted("a b\\"), "\"a b\\\\\""); // Doubled before the closing quote.
    EXPECT_EQ(quoted("\\\""), "\"\\\\\\\"\""); // 1 backslash + quote -> 3 backslashes + quote.
    EXPECT_EQ(quoted("C:\\Program Files\\"), "\"C:\\Program Files\\\\\"");
    EXPECT_EQ(quoted("ünï"), "ünï");
}

TEST(WindowsQuoting, CommandLineStartsWithTheQuotedExecutable) {
    EXPECT_EQ(frost::io::buildWindowsCommandLine("C:\\a b\\x.exe", {"1", "two words"}).value(),
              "\"C:\\a b\\x.exe\" 1 \"two words\"");
    EXPECT_EQ(frost::io::buildWindowsCommandLine("C:\\x.exe", {}).value(), "\"C:\\x.exe\"");
    EXPECT_EQ(frost::io::buildWindowsCommandLine("C:\\x\".exe", {}).error().code(), ErrorCode::InvalidArgument);
}

/*
 * An independent parser for the arguments after argv[0], written from the rules
 * in Microsoft's "Parsing C++ command-line arguments": whitespace separates
 * arguments outside quotes; 2n backslashes before a quote give n backslashes and
 * toggle quoting; 2n+1 give n backslashes and a literal quote; other backslashes
 * are literal. (The "" -inside-quotes rule is not modelled; the encoder never
 * produces it.)
 */
std::vector<std::string> parseArguments(std::string_view line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const auto skipBlanks = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
    };
    skipBlanks();
    while (i < line.size()) {
        std::string arg;
        bool inQuotes = false;
        while (i < line.size()) {
            const char c = line[i];
            if (!inQuotes && (c == ' ' || c == '\t')) {
                break;
            }
            if (c == '\\') {
                std::size_t count = 0;
                while (i < line.size() && line[i] == '\\') {
                    ++count;
                    ++i;
                }
                if (i < line.size() && line[i] == '"') {
                    arg.append(count / 2, '\\');
                    if (count % 2 == 1) {
                        arg += '"';
                    } else {
                        inQuotes = !inQuotes;
                    }
                    ++i;
                } else {
                    arg.append(count, '\\');
                }
                continue;
            }
            if (c == '"') {
                inQuotes = !inQuotes;
            } else {
                arg += c;
            }
            ++i;
        }
        out.push_back(std::move(arg));
        skipBlanks();
    }
    return out;
}

std::vector<std::string> randomArguments(std::mt19937& rng, std::size_t count) {
    static const std::vector<std::string> alphabet = {"a", "Z", " ", "\t", "\\", "\\\\", "\"", "é", "测", "\n", "'"};
    std::uniform_int_distribution<std::size_t> length(0, 8);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::vector<std::string> out;
    for (std::size_t k = 0; k < count; ++k) {
        std::string arg;
        for (std::size_t n = length(rng); n > 0; --n) {
            arg += alphabet[pick(rng)];
        }
        out.push_back(std::move(arg));
    }
    return out;
}

TEST(WindowsQuoting, RoundTripsThroughTheDocumentedRules) {
    std::mt19937 rng(20261031); // Fixed seed: deterministic.
    for (int round = 0; round < 2000; ++round) {
        const std::vector<std::string> args = randomArguments(rng, 4);
        std::string line;
        for (const std::string& arg : args) {
            if (!line.empty()) {
                line += ' ';
            }
            frost::io::appendWindowsArgument(line, arg);
        }
        ASSERT_EQ(parseArguments(line), args) << line;
    }
}

#ifdef _WIN32
TEST(WindowsQuoting, RoundTripsThroughCommandLineToArgvW) {
    std::mt19937 rng(42);
    for (int round = 0; round < 2000; ++round) {
        const std::vector<std::string> args = randomArguments(rng, 4);
        const auto line = frost::io::buildWindowsCommandLine("C:\\Program Files\\x\\tool.exe", args);
        ASSERT_TRUE(line.has_value());
        const auto wide = frost::io::toWide(*line);
        ASSERT_TRUE(wide.has_value());
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(wide->c_str(), &count);
        ASSERT_NE(parsed, nullptr);
        std::vector<std::string> back;
        for (int i = 1; i < count; ++i) {
            back.push_back(frost::io::fromWide(parsed[i]).value());
        }
        EXPECT_EQ(frost::io::fromWide(parsed[0]).value(), "C:\\Program Files\\x\\tool.exe");
        LocalFree(parsed);
        ASSERT_EQ(back, args) << *line;
    }
}
#endif

TEST(ProcessSpec, ResidentMemoryLimitIsOptionalAndPlatformSpecific) {
    ProcessSpec spec;
    spec.executable = "C:/tools/child.exe";
    EXPECT_FALSE(spec.residentMemoryLimitBytes.has_value());
    EXPECT_TRUE(frost::io::validateProcessSpec(spec, true).has_value());
    spec.residentMemoryLimitBytes = 0;
    const auto invalid = frost::io::validateProcessSpec(spec, true);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    spec.executable = "/bin/child";
    for (const auto requested : {std::uint64_t{0}, std::uint64_t{128 * 1024 * 1024}}) {
        spec.residentMemoryLimitBytes = requested;
        const auto unsupported = frost::io::validateProcessSpec(spec, false);
        ASSERT_FALSE(unsupported.has_value());
        EXPECT_EQ(unsupported.error().code(), ErrorCode::Unsupported);
    }
}

} // namespace
