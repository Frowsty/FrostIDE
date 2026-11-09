// SPDX-License-Identifier: Apache-2.0
//
// GetClCommandLines (SLN-5): the IntelliSense command lines that MSBuild's own Visual C++ targets
// compute for one project configuration, as ProjectHost returns them (project_host.hpp, the
// ClCommandLines request), and their translation into clangd's compilation-database entries.
/*
 * Source. ProjectHost builds the target GetClCommandLines (Microsoft.Cpp.DesignTime.targets) in a
 * design-time build: one ClCommandLines item per batch of sources that share their options. Its
 * item spec is cl.exe's command line without the sources ("/c /I"inc" /W3 /D X=1 /EHsc /MDd ...");
 * Files lists the sources it applies to. The IncludePath and ExternalIncludePath properties name
 * the directories cl.exe reads from INCLUDE and EXTERNAL_INCLUDE, which are not on the command line.
 *
 * Translation (clangdCommands), per source file of each command line, in clang-cl syntax:
 *
 *     <compiler> --target=<triple> <every command-line argument, in order, except the dropped
 *     ones> /imsvc<dir> (IncludePath, then ExternalIncludePath, each directory once) <file>
 *
 * The arguments are split with the MSVC runtime's rules (splitMsvcCommandLine). Dropped are only
 * the switches that name outputs or drive the build rather than the compilation, which clangd must
 * not act on (isDroppedClSwitch): /Fo /Fd /Fp /Fa /FA /Fe /Fm /FR /Fr /Fi (output files), /Y*
 * (precompiled-header creation and use: the source includes its PCH header itself, as /Yu
 * requires), /MP (parallel build) and /errorReport. Everything else, defines, include directories,
 * /std, /permissive-, /Zc:*, /EH*, /MD*, AdditionalOptions, reaches clangd exactly as MSBuild wrote
 * it. --target follows the platform (x64 x86_64, Win32 and x86 i686, ARM64 aarch64, ARM armv7);
 * another platform adds no --target.
 *
 * Every function here is pure and thread-safe.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

// One ClCommandLines item of GetClCommandLines.
struct ClCommandLine {
    std::string commandLine;           // The item spec, as MSBuild wrote it.
    std::vector<std::string> files;    // The sources it applies to, absolute and normalised, in MSBuild's order.
    std::string workingDirectory;      // Absolute; "" when MSBuild gave none (the project directory then).
    bool configurationOptions = false; // The batch that also carries the project-wide options.

    friend bool operator==(const ClCommandLine&, const ClCommandLine&) = default;
};

// GetClCommandLines of one project configuration.
struct ProjectClCommandLines {
    std::string projectPath;   // Absolute .vcxproj.
    std::string configuration; // The project configuration, "Debug".
    std::string platform;      // The project platform, "x64".
    std::vector<ClCommandLine> commandLines;
    // The IncludePath property's directories, in order, absolute (a relative one is resolved
    // against the project's directory, cl.exe's working directory).
    std::vector<std::string> includePath;
    std::vector<std::string> externalIncludePath; // ExternalIncludePath's, the same way.

    friend bool operator==(const ProjectClCommandLines&, const ProjectClCommandLines&) = default;
};

// One compilation-database entry for clangd.
struct ClangdCompileCommand {
    std::string directory;              // Absolute working directory.
    std::string file;                   // Absolute source.
    std::vector<std::string> arguments; // argv, compiler first, the file last.

    friend bool operator==(const ClangdCompileCommand&, const ClangdCompileCommand&) = default;
};

// The arguments of `commandLine` with the MSVC runtime's rules: blanks separate outside quotes;
// 2n backslashes before '"' give n and toggle quoting, 2n+1 give n and a literal '"'; '""' inside
// quotes is a literal '"'; other backslashes are literal. O(size).
[[nodiscard]] std::vector<std::string> splitMsvcCommandLine(std::string_view commandLine);

// True for a switch the translation drops (see the header comment); '-' and '/' both start one.
[[nodiscard]] bool isDroppedClSwitch(std::string_view argument) noexcept;

// --target's value for an MSBuild platform (ASCII case ignored), or "" (see the header comment).
[[nodiscard]] std::string clangTargetForPlatform(std::string_view platform);

// The clangd entries of every source of `lines` (see the header comment); a source listed by two
// command lines keeps the first. O(sources x arguments).
[[nodiscard]] std::vector<ClangdCompileCommand> clangdCommands(const ProjectClCommandLines& lines,
                                                               std::string_view compiler = "clang-cl");

// The arguments of one command line after the translation, without compiler, --target, /imsvc
// and file: what a test compares with GetClCommandLines' own argument list.
[[nodiscard]] std::vector<std::string> translatedClArguments(std::string_view commandLine);

} // namespace frost::project
