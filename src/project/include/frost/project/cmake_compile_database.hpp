// SPDX-License-Identifier: Apache-2.0
//
// CMK-4: compile commands from the CMake File API codemodel v2 (cmake_file_api.hpp), for the
// generators that write no compile_commands.json (Visual Studio, Xcode), and the map from a file to
// the targets that list it (the resolve context of a header, CPP-3).
/*
 * Synthesis. Every source of a target that has a compile group in C or C++ becomes one command,
 * built the way CMake 4.4's Ninja and Makefile generators build theirs from the same codemodel data
 * (Modules/Platform/Windows-MSVC.cmake, Windows-Clang.cmake and CMakeCXXInformation.cmake: the
 * compiler, then <DEFINES> <INCLUDES> <FLAGS>, then the source), so a command synthesized from a
 * Ninja configure's reply gives clangd the flags of the compile_commands.json that configure wrote
 * (the acceptance oracle):
 *   MSVC (cl):       <cl> /nologo /TP|/TC -D<def>... -I<dir>|-external:I<dir>... [-external:W0] <fragments> -c <src>
 *   clang-cl:        <clang-cl> /nologo -TP|-TC -D<def>... -I<dir>|-imsvc<dir>... <fragments> -c <src>
 *   GCC and Clang:   <cc> -D<def>... -I<dir>|-isystem <dir>... <fragments> -c <src>
 * Output switches (/Fo, /Fd, /FS, -o) name build artifacts and change no parse; they are left out.
 * cl takes -external:I from 19.29.30036.3 on (CMake's own condition); before that a system include
 * is an ordinary -I. Include directories keep the File API's spelling with native separators.
 * Compile command fragments are split into arguments by the rules of the shell the generator
 * writes them for: the Windows C runtime's (CommandLineToArgvW) on Windows and POSIX sh words
 * elsewhere (splitCompileCommandFragment). The compiler is the toolchains object's path for the
 * group's language, else the cache's CMAKE_<LANG>_COMPILER; a group without either is skipped
 * (nothing is invented). Sources in other languages (RC, ASM, CUDA, Fortran) are skipped.
 *
 * A header has no compile group. Its command in the context of a target (commandInTarget) uses the
 * group of the target's first non-generated compiled source in C++ (else C) with the header as the
 * source, forced to that language (/TP or /TC; -x c++-header or -x c-header), which is how clangd
 * would parse it when included from that target's sources.
 *
 * Thread safety: pure functions over immutable values. Complexity: O(total sources and flags).
 */
#pragma once

#include "frost/project/cmake_file_api.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

// The compiler front end a toolchain's command line is written for.
enum class CMakeCompilerFrontend : std::uint8_t {
    Msvc,    // cl.exe.
    ClangCl, // clang-cl and other MSVC-style drivers (icx-cl).
    Gnu,     // gcc, g++, clang, clang++ and other GNU-style drivers.
};

// MSVC for compiler id "MSVC"; ClangCl for a driver named clang-cl or icx-cl (any case, .exe or
// not); Gnu otherwise.
[[nodiscard]] CMakeCompilerFrontend cmakeCompilerFrontend(const CMakeToolchain& toolchain) noexcept;

// Which shell's quoting a fragment uses.
enum class CMakeShell : std::uint8_t { Windows, Posix };

// The shell of this platform's generators (Windows on Windows, else Posix).
[[nodiscard]] CMakeShell nativeCMakeShell() noexcept;

/*
 * Splits one compileCommandFragment into arguments. Windows: whitespace separates arguments outside
 * double quotes; 2n backslashes before a quote give n backslashes and toggle quoting, 2n+1 give n
 * and a literal quote, other backslashes are literal, and "" inside quotes is a literal quote.
 * Posix: whitespace separates words; single quotes are literal; inside double quotes a backslash
 * escapes $ ` " \ and newline; outside quotes a backslash escapes the next character. No expansion.
 */
[[nodiscard]] std::vector<std::string> splitCompileCommandFragment(std::string_view fragment, CMakeShell shell);

// One synthesized compile command.
struct CMakeCompileCommand {
    std::string directory{};              // The top-level build directory, '/' separators (as CMake writes it).
    std::string file{};                   // Absolute, normalised, native.
    std::vector<std::string> arguments{}; // argv, compiler first.
    std::size_t target = 0;               // Index into CMakeProject::targets.

    friend bool operator==(const CMakeCompileCommand&, const CMakeCompileCommand&) = default;
};

struct CMakeCommandOptions {
    CMakeShell shell = nativeCMakeShell();
    // Path separators of the include directories on the command line ('\\' on Windows).
    char separator = nativeCMakeShell() == CMakeShell::Windows ? '\\' : '/';
};

// The commands of every compiled C and C++ source, in target and source order (header comment).
[[nodiscard]] std::vector<CMakeCompileCommand> synthesizeCMakeCompileCommands(const CMakeProject& project,
                                                                              const CMakeCommandOptions& options = {});

// The indices of the targets whose sources list `file` (compiled sources and headers alike), in model
// order. Paths compare normalised (ASCII case ignored on Windows). O(sources).
[[nodiscard]] std::vector<std::size_t> cmakeTargetsListing(const CMakeProject& project, std::string_view file);

/*
 * The command `file` is parsed with in the context of target `target` (an index): the target's own
 * command for a source it compiles, else the header command of the header comment. nullopt when the
 * index is out of range, the target compiles nothing in C or C++, or no compiler is known.
 */
[[nodiscard]] std::optional<CMakeCompileCommand> cmakeCommandInTarget(const CMakeProject& project, std::size_t target,
                                                                      std::string_view file,
                                                                      const CMakeCommandOptions& options = {});

} // namespace frost::project
