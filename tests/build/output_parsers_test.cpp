// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the output parsers (output_parsers.hpp): GCC/Clang's GNU format with columns,
// flags, Windows drive paths, the tool form and the include chain; the compiler parser's MSVC and
// clang-cl lines; and CMake's message blocks (errors with a location, warnings, one-line errors,
// call stacks, the end of the stream).

#include "frost/build/output_parsers.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace frost::build {
namespace {

std::vector<Diagnostic> run(IOutputParser& parser, const std::vector<std::string_view>& lines) {
    std::vector<Diagnostic> out;
    const DiagnosticSink sink = [&](Diagnostic diagnostic) { out.push_back(std::move(diagnostic)); };
    for (std::string_view line : lines) {
        parser.feed(line, sink);
    }
    parser.finish(sink);
    return out;
}

TEST(StructuredMsBuildDiagnostics, PreservesLocalizedMultilineMessagesAndProjectIdentity) {
    auto diagnostic = parseStructuredMsBuildLine(R"(FROSTBUILD1 {"Severity":"error","Code":"C2065","Message":"okänd symbol\nsecond line","File":"C:\\src\\main.cpp","Line":17,"Column":4,"Project":"C:\\src\\App.vcxproj"})");
    ASSERT_TRUE(diagnostic);
    EXPECT_EQ(diagnostic->message, "okänd symbol\nsecond line");
    EXPECT_EQ(diagnostic->line, 17);
    EXPECT_EQ(diagnostic->column, 4);
    EXPECT_EQ(diagnostic->file, R"(C:\src\main.cpp)");
    EXPECT_EQ(diagnostic->project, R"(C:\src\App.vcxproj)");
}

TEST(StructuredMsBuildDiagnostics, ConsoleDuplicatesAndMalformedEventsAreIgnored) {
    auto parser = makeStructuredMsBuildOutputParser();
    const auto diagnostics = run(*parser, {
        "main.cpp(17): error C2065: unknown symbol",
        R"(FROSTBUILD1 {"Severity":"warning","Code":"C4100","Message":"unused","File":"a.cpp","Line":2,"Column":0,"Project":"App.vcxproj"})",
        "FROSTBUILD1 {broken",
        R"(FROSTBUILD1 {"Severity":"message"})"
    });
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().severity, Severity::Warning);
    EXPECT_EQ(diagnostics.front().code, "C4100");
}

TEST(GnuDiagnostics, ParsesLocationsSeveritiesAndFlags) {
    auto warning =
        parseGnuDiagnosticLine("/work/tiny/src/core.c:7:9: warning: unused variable 'x' [-Wunused-variable]");
    ASSERT_TRUE(warning.has_value());
    EXPECT_EQ(warning->severity, Severity::Warning);
    EXPECT_EQ(warning->file, "/work/tiny/src/core.c");
    EXPECT_EQ(warning->line, 7);
    EXPECT_EQ(warning->column, 9);
    EXPECT_EQ(warning->code, "-Wunused-variable");
    EXPECT_EQ(warning->message, "unused variable 'x'");

    auto error = parseGnuDiagnosticLine("src/core.c:12: error: 'y' undeclared (first use in this function)");
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->severity, Severity::Error);
    EXPECT_EQ(error->line, 12);
    EXPECT_EQ(error->column, 0);
    EXPECT_EQ(error->code, "");

    auto werror = parseGnuDiagnosticLine("C:/w/a.c:3:1: error: no return [-Werror,-Wreturn-type]");
    ASSERT_TRUE(werror.has_value());
    EXPECT_EQ(werror->file, "C:/w/a.c"); // The drive colon is part of the path.
    EXPECT_EQ(werror->code, "-Wreturn-type");
    auto fatal = parseGnuDiagnosticLine("C:\\w\\a.c:1:10: fatal error: 'missing.h' file not found");
    ASSERT_TRUE(fatal.has_value());
    EXPECT_EQ(fatal->severity, Severity::Error);
    EXPECT_EQ(fatal->file, "C:\\w\\a.c");
    EXPECT_EQ(fatal->column, 10);
    auto note = parseGnuDiagnosticLine("/w/a.h:2:6: note: previous declaration is here");
    ASSERT_TRUE(note.has_value());
    EXPECT_EQ(note->severity, Severity::Message);

    auto tool = parseGnuDiagnosticLine("ld.lld: error: undefined symbol: tiny_missing");
    ASSERT_TRUE(tool.has_value());
    EXPECT_EQ(tool->file, "ld.lld");
    EXPECT_EQ(tool->line, 0);
    EXPECT_EQ(tool->message, "undefined symbol: tiny_missing");
    auto coloured = parseGnuDiagnosticLine("\x1b[1m/w/a.c:4:2: \x1b[0;1;35mwarning: \x1b[0mtext\x1b[0m");
    ASSERT_TRUE(coloured.has_value());
    EXPECT_EQ(coloured->line, 4);
    EXPECT_EQ(coloured->message, "text");

    // Not diagnostics.
    for (std::string_view line :
         {"[2/4] Building C object CMakeFiles/core.dir/src/core.c.o", "FAILED: CMakeFiles/core.dir/src/core.c.o",
          "    7 |     int x = 1;", "      |         ^", "1 warning and 1 error generated.", "a sentence: with a colon",
          "some path/x: error: has a separator", ""}) {
        EXPECT_FALSE(parseGnuDiagnosticLine(line).has_value()) << line;
    }
}

TEST(GnuDiagnostics, IncludeChainPointsAtTheIncludingFile) {
    auto first = parseGnuDiagnosticLine("In file included from /w/src/main.c:3:");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->severity, Severity::Message);
    EXPECT_EQ(first->file, "/w/src/main.c");
    EXPECT_EQ(first->line, 3);
    EXPECT_EQ(first->message, "In file included from here");
    auto next = parseGnuDiagnosticLine("                 from C:/w/src/other.c:5:10,");
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->file, "C:/w/src/other.c");
    EXPECT_EQ(next->line, 5);
    EXPECT_EQ(next->column, 10);
}

TEST(CompilerDiagnostics, AcceptsMsvcAndGnuLines) {
    auto parser = makeCompilerOutputParser();
    const auto found = run(*parser, {
                                        "C:\\w\\src\\core.c(7,9): warning: unused variable 'x' [-Wunused-variable]",
                                        "C:\\w\\src\\core.c(12): error C2065: 'y': undeclared identifier",
                                        "/w/src/core.c:12:5: error: use of undeclared identifier 'y'",
                                        "ninja: build stopped: subcommand failed.",
                                    });
    ASSERT_EQ(found.size(), 3U);
    EXPECT_EQ(found[0].file, "C:\\w\\src\\core.c");
    EXPECT_EQ(found[0].line, 7);
    EXPECT_EQ(found[0].severity, Severity::Warning);
    EXPECT_EQ(found[1].code, "C2065");
    EXPECT_EQ(found[1].line, 12);
    EXPECT_EQ(found[2].file, "/w/src/core.c");
    EXPECT_EQ(found[2].column, 5);
    EXPECT_EQ(found[2].severity, Severity::Error);
}

TEST(CMakeDiagnostics, ReadsMessageBlocks) {
    auto parser = makeCMakeOutputParser();
    const auto found = run(*parser, {
                                        "-- The C compiler identification is Clang 18.1.3",
                                        "CMake Error at CMakeLists.txt:12 (message):",
                                        "  The tiny project is broken",
                                        "  on purpose.",
                                        "Call Stack (most recent call first):",
                                        "  sub/CMakeLists.txt:3 (include)",
                                        "",
                                        "",
                                        "CMake Warning (dev) at cmake/warnings.cmake:4 (message):",
                                        "  Be careful.",
                                        "This warning is for project developers.  Use -Wno-dev to suppress it.",
                                        "CMake Error: The source directory \"/nowhere\" does not exist.",
                                        "CMake Deprecation Warning at CMakeLists.txt:1 (cmake_minimum_required):",
                                        "  Compatibility with CMake < 3.10 will be removed.",
                                        "",
                                        "-- Configuring incomplete, errors occurred!",
                                        "CMake Error in CMakeLists.txt:",
                                        "  No SOURCES given to target: empty",
                                    });
    ASSERT_EQ(found.size(), 5U);
    EXPECT_EQ(found[0].severity, Severity::Error);
    EXPECT_EQ(found[0].file, "CMakeLists.txt");
    EXPECT_EQ(found[0].line, 12);
    EXPECT_EQ(found[0].message, "The tiny project is broken on purpose.");
    EXPECT_EQ(found[1].severity, Severity::Warning);
    EXPECT_EQ(found[1].file, "cmake/warnings.cmake");
    EXPECT_EQ(found[1].line, 4);
    EXPECT_EQ(found[1].message, "Be careful.");
    EXPECT_EQ(found[2].severity, Severity::Error);
    EXPECT_EQ(found[2].file, "");
    EXPECT_EQ(found[2].message, "The source directory \"/nowhere\" does not exist.");
    EXPECT_EQ(found[3].severity, Severity::Warning);
    EXPECT_EQ(found[3].line, 1);
    EXPECT_EQ(found[4].file, "CMakeLists.txt"); // Ended by the end of the stream.
    EXPECT_EQ(found[4].line, 0);
    EXPECT_EQ(found[4].message, "No SOURCES given to target: empty");

    // A project's own message() output is not a diagnostic.
    auto plain = makeCMakeOutputParser();
    EXPECT_TRUE(run(*plain, {"-- CMake Error at nothing", "  CMake Error at indented.txt:1 (x):"}).empty());
}

} // namespace
} // namespace frost::build
