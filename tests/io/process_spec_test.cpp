// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the platform-independent process rules: ProcessSpec
// validation (absolute executables only, batch files refused, NUL and
// environment checks) and Windows command-line quoting, checked against an
// independent parser written from Microsoft's documented rules and, on Windows,
// against CommandLineToArgvW itself.

#include "frost/io/process.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <string_view>
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
    for (const char* exe :
         {"C:\\x\\build.bat", "C:\\x\\BUILD.CMD", "C:\\x\\a.Bat", "C:\\x\\run.cmd. ", "C:\\x\\run.bat..."}) {
        EXPECT_EQ(validate(specFor(exe), true), ErrorCode::Unsupported) << exe;
    }
    EXPECT_EQ(validate(specFor("/x/build.bat"), false), ErrorCode::Unknown); // An ordinary file on POSIX.
    EXPECT_EQ(validate(specFor("C:\\x\\batman.exe"), true), ErrorCode::Unknown);
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

} // namespace
