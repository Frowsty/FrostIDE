// SPDX-License-Identifier: Apache-2.0
//
// Tests for the canonical-format diagnostic parser and DiagnosticSet (frost/build/diagnostic.hpp):
// one case per producer (cl, link, MSBuild, rc, clang-cl) and the edge cases (no file, spaces,
// non-ASCII, the project suffix, lines that are not diagnostics).

#include "frost/build/diagnostic.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using frost::build::Diagnostic;
using frost::build::DiagnosticSet;
using frost::build::parseDiagnosticLine;
using frost::build::Severity;

Diagnostic parsed(std::string_view line) {
    const std::optional<Diagnostic> diagnostic = parseDiagnosticLine(line);
    EXPECT_TRUE(diagnostic.has_value()) << line;
    return diagnostic.value_or(Diagnostic{});
}

TEST(DiagnosticParser, ClErrorWithColumnAndProject) {
    const Diagnostic d = parsed(R"(C:\src\App\main.cpp(7,27): error C2065: 'undeclared_thing': undeclared identifier )"
                                R"([C:\src\App\App.vcxproj])");
    EXPECT_EQ(d.severity, Severity::Error);
    EXPECT_EQ(d.code, "C2065");
    EXPECT_EQ(d.message, "'undeclared_thing': undeclared identifier");
    EXPECT_EQ(d.file, R"(C:\src\App\main.cpp)");
    EXPECT_EQ(d.line, 7);
    EXPECT_EQ(d.column, 27);
    EXPECT_EQ(d.project, R"(C:\src\App\App.vcxproj)");
}

TEST(DiagnosticParser, ClWarningWithLineOnly) {
    const Diagnostic d = parsed(R"(C:\src\lib.cpp(12): warning C4996: 'strcpy': This function may be unsafe.)");
    EXPECT_EQ(d.severity, Severity::Warning);
    EXPECT_EQ(d.code, "C4996");
    EXPECT_EQ(d.line, 12);
    EXPECT_EQ(d.column, 0);
    EXPECT_EQ(d.project, "");
    EXPECT_EQ(d.message, "'strcpy': This function may be unsafe.");
}

TEST(DiagnosticParser, ClFatalErrorAndMessage) {
    const Diagnostic fatal = parsed(R"(C:\src\a.cpp(1,10): fatal error C1083: Cannot open include file: 'x.h': )"
                                    R"(No such file or directory [C:\src\a.vcxproj])");
    EXPECT_EQ(fatal.severity, Severity::Error);
    EXPECT_EQ(fatal.code, "C1083");
    EXPECT_EQ(fatal.message, "Cannot open include file: 'x.h': No such file or directory");

    const Diagnostic note = parsed(R"(C:\src\a.cpp(3,6): message : see declaration of 'old' [C:\src\a.vcxproj])");
    EXPECT_EQ(note.severity, Severity::Message);
    EXPECT_EQ(note.code, "");
    EXPECT_EQ(note.message, "see declaration of 'old'");
    EXPECT_EQ(note.line, 3);
}

TEST(DiagnosticParser, ClCommandLineWarningHasATwoWordSubcategory) {
    const Diagnostic d = parsed("cl : command line warning D9002: ignoring unknown option '/foo' [C:\\p\\a.vcxproj]");
    EXPECT_EQ(d.severity, Severity::Warning);
    EXPECT_EQ(d.code, "D9002");
    EXPECT_EQ(d.file, "cl");
    EXPECT_EQ(d.line, 0);
    EXPECT_EQ(d.message, "ignoring unknown option '/foo'");
}

TEST(DiagnosticParser, LinkerErrors) {
    const Diagnostic unresolved = parsed(
        R"x(main.obj : error LNK2019: unresolved external symbol "int __cdecl missing(void)" (?missing@@YAHXZ) )x"
        R"(referenced in function main [C:\src\App\App.vcxproj])");
    EXPECT_EQ(unresolved.severity, Severity::Error);
    EXPECT_EQ(unresolved.code, "LNK2019");
    EXPECT_EQ(unresolved.file, "main.obj");
    EXPECT_EQ(unresolved.line, 0);
    EXPECT_EQ(
        unresolved.message,
        R"x(unresolved external symbol "int __cdecl missing(void)" (?missing@@YAHXZ) referenced in function main)x");

    const Diagnostic fatal =
        parsed(R"(LINK : fatal error LNK1104: cannot open file 'C:\lib\x.lib' [C:\src\a.vcxproj])");
    EXPECT_EQ(fatal.severity, Severity::Error);
    EXPECT_EQ(fatal.code, "LNK1104");
    EXPECT_EQ(fatal.file, "LINK");
    EXPECT_EQ(fatal.message, R"(cannot open file 'C:\lib\x.lib')");

    const Diagnostic exe =
        parsed(R"(C:\out dir\App.exe : fatal error LNK1120: 1 unresolved externals [C:\s\a.vcxproj])");
    EXPECT_EQ(exe.file, R"(C:\out dir\App.exe)");
    EXPECT_EQ(exe.code, "LNK1120");
}

TEST(DiagnosticParser, MsBuildErrorsWithAndWithoutOrigin) {
    const Diagnostic targets = parsed(
        R"(C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Microsoft\VC\v170\Microsoft.CppBuild.targets(548,5): )"
        R"(warning MSB8029: The Intermediate directory or Output directory cannot reside under the Temporary directory. )"
        R"([C:\Users\me\Lib\Lib.vcxproj])");
    EXPECT_EQ(targets.severity, Severity::Warning);
    EXPECT_EQ(targets.code, "MSB8029");
    EXPECT_EQ(
        targets.file,
        R"(C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Microsoft\VC\v170\Microsoft.CppBuild.targets)");
    EXPECT_EQ(targets.line, 548);
    EXPECT_EQ(targets.column, 5);
    EXPECT_EQ(targets.project, R"(C:\Users\me\Lib\Lib.vcxproj)");

    const Diagnostic bare = parsed("MSBUILD : error MSB1009: Project file does not exist.");
    EXPECT_EQ(bare.file, "MSBUILD");
    EXPECT_EQ(bare.code, "MSB1009");

    const Diagnostic noOrigin = parsed("error MSB4126: The specified solution configuration \"Debug|x86\" is invalid.");
    EXPECT_EQ(noOrigin.severity, Severity::Error);
    EXPECT_EQ(noOrigin.file, "");
    EXPECT_EQ(noOrigin.code, "MSB4126");
    EXPECT_EQ(noOrigin.message, "The specified solution configuration \"Debug|x86\" is invalid.");

    const Diagnostic metaproj = parsed(R"(C:\s\Tiny.sln.metaproj : error MSB4057: The target "Nope:Rebuild" does not )"
                                       R"(exist in the project. [C:\s\Tiny.sln])");
    EXPECT_EQ(metaproj.file, R"(C:\s\Tiny.sln.metaproj)");
    EXPECT_EQ(metaproj.message, R"(The target "Nope:Rebuild" does not exist in the project.)");
    EXPECT_EQ(metaproj.project, R"(C:\s\Tiny.sln)");
}

TEST(DiagnosticParser, ResourceCompiler) {
    const Diagnostic d = parsed(R"(C:\src\app.rc(10): error RC2135: file not found: icon.ico [C:\src\app.vcxproj])");
    EXPECT_EQ(d.severity, Severity::Error);
    EXPECT_EQ(d.code, "RC2135");
    EXPECT_EQ(d.line, 10);
    EXPECT_EQ(d.message, "file not found: icon.ico");
}

TEST(DiagnosticParser, ClangClFormat) {
    const Diagnostic error =
        parsed(R"(C:\src\main.cpp(7,5): error: use of undeclared identifier 'x' [C:\src\a.vcxproj])");
    EXPECT_EQ(error.severity, Severity::Error);
    EXPECT_EQ(error.code, "");
    EXPECT_EQ(error.message, "use of undeclared identifier 'x'");
    EXPECT_EQ(error.line, 7);
    EXPECT_EQ(error.column, 5);
    EXPECT_EQ(error.project, R"(C:\src\a.vcxproj)");

    const Diagnostic warning =
        parsed(R"(C:\src\main.cpp(4,9): warning: unused variable 'a' [-Wunused-variable] [C:\src\a.vcxproj])");
    EXPECT_EQ(warning.severity, Severity::Warning);
    EXPECT_EQ(warning.code, "-Wunused-variable");
    EXPECT_EQ(warning.message, "unused variable 'a'");

    const Diagnostic note = parsed(R"(C:\src\main.cpp(2,6): note: 'old' has been explicitly marked deprecated here)");
    EXPECT_EQ(note.severity, Severity::Message);

    const Diagnostic fatal = parsed(R"(C:\src\main.cpp(1,10): fatal error: 'missing.h' file not found)");
    EXPECT_EQ(fatal.severity, Severity::Error);
    EXPECT_EQ(fatal.message, "'missing.h' file not found");
}

TEST(DiagnosticParser, LocationForms) {
    EXPECT_EQ(parsed("a.cpp(3-5): error C1: x").line, 3);
    const Diagnostic range = parsed("a.cpp(3,4-9): error C1: x");
    EXPECT_EQ(range.line, 3);
    EXPECT_EQ(range.column, 4);
    const Diagnostic span = parsed("a.cpp(3,4,5,6): error C1: x");
    EXPECT_EQ(span.line, 3);
    EXPECT_EQ(span.column, 4);
    // Not a location: kept as part of the file name.
    const Diagnostic odd = parsed("weird(name): error C1: x");
    EXPECT_EQ(odd.file, "weird(name)");
    EXPECT_EQ(odd.line, 0);
}

TEST(DiagnosticParser, PathsWithSpacesAndNonAscii) {
    const Diagnostic d = parsed("C:\\Users\\Jörg Müller\\my proj\\src\\é.cpp(3,1): warning C4100: 'x': unreferenced "
                                "parameter [C:\\Users\\Jörg Müller\\my proj\\a [1].vcxproj]");
    EXPECT_EQ(d.file, "C:\\Users\\Jörg Müller\\my proj\\src\\é.cpp");
    EXPECT_EQ(d.line, 3);
    EXPECT_EQ(d.project, "C:\\Users\\Jörg Müller\\my proj\\a [1].vcxproj");
    EXPECT_EQ(d.message, "'x': unreferenced parameter");
}

TEST(DiagnosticParser, PrefixesAndLineEndings) {
    const Diagnostic node = parsed("  12>C:\\a.cpp(1,2): error C2143: syntax error: missing ';'\r");
    EXPECT_EQ(node.file, "C:\\a.cpp");
    EXPECT_EQ(node.code, "C2143");
    EXPECT_EQ(node.message, "syntax error: missing ';'");
    EXPECT_EQ(parsed("1:7>error MSB1009: Project file does not exist.").code, "MSB1009");
    EXPECT_EQ(parsed("Error C2065: x").severity, Severity::Error); // Category words are case-insensitive.
}

TEST(DiagnosticParser, IgnoresOrdinaryOutput) {
    for (const std::string_view line : {
             "",
             "  main.cpp",
             "  Lib.vcxproj -> C:\\Users\\me\\x64\\Debug\\Lib.lib",
             "Build started 11/1/2026 11:00:34 PM.",
             "  Creating library C:\\x\\App.lib and object C:\\x\\App.exp",
             "Note: including file: C:\\x\\lib.h",
             "This is not an error: really",
             "the error handling: works",
             "C:\\x\\y.cpp",
         }) {
        EXPECT_FALSE(parseDiagnosticLine(line).has_value()) << line;
    }
}

TEST(DiagnosticSet, DropsDuplicatesAndCounts) {
    DiagnosticSet set;
    const Diagnostic warning = parsed("C:\\a.h(3): warning C4244: conversion [C:\\p.vcxproj]");
    const Diagnostic error = parsed("C:\\a.cpp(5): error C2065: 'x': undeclared identifier [C:\\p.vcxproj]");
    const Diagnostic note = parsed("C:\\a.cpp(1): message : see declaration [C:\\p.vcxproj]");
    EXPECT_TRUE(set.add(warning));
    EXPECT_FALSE(set.add(warning)); // The same header warning from a second source file.
    EXPECT_TRUE(set.add(error));
    EXPECT_TRUE(set.add(note));
    Diagnostic otherProject = warning;
    otherProject.project = "C:\\q.vcxproj";
    EXPECT_TRUE(set.add(otherProject)); // Another project's copy is a separate entry.
    EXPECT_EQ(set.errorCount(), 1U);
    EXPECT_EQ(set.warningCount(), 2U);
    EXPECT_EQ(set.messageCount(), 1U);
    ASSERT_EQ(set.diagnostics().size(), 4U);
    EXPECT_EQ(set.diagnostics()[0], warning);
    EXPECT_EQ(set.diagnostics()[1], error);
}

TEST(DiagnosticSet, SeverityNames) {
    EXPECT_EQ(frost::build::toString(Severity::Error), "error");
    EXPECT_EQ(frost::build::toString(Severity::Warning), "warning");
    EXPECT_EQ(frost::build::toString(Severity::Message), "message");
}

} // namespace
