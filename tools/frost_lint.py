#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# frost_lint.py: frost-lint v0, FrostIDE's repository convention checker
# (PLAN.md section 6.6 "Enforcement" and the startup-hygiene rules of section 2.1).
#
#   python tools/frost_lint.py [--repo <path>]
#
# It checks every file git would track (`git ls-files --cached --others
# --exclude-standard`, so build trees and the ignored .claude/worktrees are skipped;
# when the tree is not a git checkout, as in the WSL ext4 mirror, a .gitignore-aware
# walk stands in). Rules, each reported under its name:
#   spdx-header   source files (SOURCE_SYNTAX_BY_SUFFIX and SOURCE_SYNTAX_BY_NAME below)
#                 start with a header comment that holds an SPDX-License-Identifier
#                 line and says what the file is for;
#   reuse         every other file is covered by a REUSE.toml annotation or a
#                 `<file>.license` sidecar (a `reuse lint` equivalent);
#   todo          to-do markers in comments name an open milestone M0..M17;
#   budget        every budget reference ("Budget: B5.") exists in tests/perf/budgets.json;
#   narrow-win32  narrow (...A) Win32 APIs are banned in C/C++ (PLAN.md 2.1);
#   exe-search    calls that look up an executable in the current directory or PATH are
#                 banned in C/C++ (PLAN.md 3.5: tools resolve to absolute paths);
#   cpp26         banned C++26 constructs in C/C++ outside the exempt directories;
#   qt-svg        Qt Svg headers are included only under **/icon_baker/** (ADR 0015 decision 7:
#                 the runtime draws baked atlases and never renders SVG);
#   no-ci         there is no .github/workflows directory (D24);
#   dev-path      source files under src/ and tests/ name no developer machine path (DEV_PATH_PATTERNS:
#                 E:\sdk, E:\FrostBuilds, D:\dev\FrostEngine-AI-Claude and their /mnt/<drive> forms;
#                 FND-10): products search beside the executable, then FROST_SDK, then PATH, and
#                 tests take the SDK from FROST_SDK (tools/env.ps1, ~/sdk/env.sh);
# and the truth rule (ADR 0078 decision 1, FND-5), in C, C++ and C# outside tests/**:
#   fabricated-result     values of the result types (RESULT_TYPES: test outcomes, packages,
#                         usages, disassembly, diagnostics) are built only in their allowlisted
#                         parsers, or at an exact SITE_ALLOWANCES site (stale entries fail too);
#   result-type-registry  a type under src/** named like a result is listed in RESULT_TYPES;
#   synthesized-output    no string literal imitates a tool's output (TOOL_OUTPUT_MARKERS);
#   parity-claim          README and the other PARITY_CLAIM_DOC_GLOBS documents, and string
#                         literals under src/ui and src/app, claim no parity or complete
#                         replacement (FND-4).
#
#   python tools/frost_lint.py [--repo <path>] ratchet [--base main]
#
# runs the source-to-test line ratchet of the current slice instead (RATCHET_MIN_SOURCE_LINES
# below; ADR 0078 decision 6) and prints "ratchet ok (...)", "ratchet FAILED (...)" or
# "ratchet not-run (...)" last.
# Byte-exact data (fixtures, goldens, fuzz corpora; DATA_GLOBS) and vendored third-party
# sources (VENDORED_GLOBS) only need REUSE coverage.
#
# Output: one "path:line: rule: message" line per violation on stdout and a one-line
# summary on stderr. Exit status: 0 clean, 1 violations, 2 usage or environment error.
# Python 3.11 or newer (tomllib); standard library only.
#
# TODO(M1): move the C++, C# and CMake rules to tree-sitter queries (PLAN.md 6.6). v0 uses
# a small hand-written lexer to separate code, strings and comments, plus regular expressions.
# TODO(M1): add the function-comment rules for PowerShell, bash, Python and CMake functions.

from __future__ import annotations

import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

if sys.version_info < (3, 11):
    # An environment error, so exit status 2 (sys.exit with a message would exit 1).
    print("frost_lint.py needs Python 3.11 or newer (tomllib)", file=sys.stderr)
    sys.exit(2)

import tomllib  # noqa: E402  (after the version check on purpose)

# ---------------------------------------------------------------------------------------
# Policy constants
# ---------------------------------------------------------------------------------------

# Milestones are M0..M17 (PLAN.md section 7). IDs are identities, not positions: ADR 0029
# added M15-M17 for the deferred halves of M1-M4. Keep in step with featurecheck.py's range.
FIRST_MILESTONE = 0
LAST_MILESTONE = 17
# Milestones whose exit has been signed off. A to-do marker naming one of them is a
# violation, because the work it describes should have landed (PLAN.md 6.6). M0 was
# signed off on 2026-11-01 (PLAN.md section 7); add the number here at each sign-off.
CLOSED_MILESTONES: frozenset[int] = frozenset({0})

# Paths (REUSE glob syntax, relative to the repository root) that hold byte-exact data:
# fixtures, goldens and fuzz corpora (PLAN.md 6.6 "Every other file"). They must stay
# byte-identical, so only the REUSE coverage rule applies to them. Mirrors the `-text`
# entries in .gitattributes.
DATA_GLOBS = ("tests/**/fixtures/**", "tests/**/goldens/**", "tests/fuzz/corpus/**")

# Vendored third-party sources, byte-identical to their pins in third_party/vendor.lock.json
# (ADR 0023; libvterm, ADR 0051): our header and code rules do not apply to them, only REUSE
# coverage (REUSE.toml names their upstream copyright and licence). Mirrors the `-text` entries in
# .gitattributes.
VENDORED_GLOBS = ("third_party/tree-sitter/**", "third_party/grammars/**", "third_party/libvterm/**")

# Directories exempt from the banned-C++26 rule only (every other rule still applies):
#   src/compat/**                   the compat layer may wrap or probe C++26 library
#                                   features behind __cpp_lib_* checks (PLAN.md 2.1);
#   tests/toolchain_probe/banned/** intentional compile-fail fixtures, one banned
#                                   construct each, that toolchain_probe must reject.
CPP26_EXEMPT_GLOBS = ("src/compat/**", "tests/toolchain_probe/banned/**")

# The SPDX line must appear in the header comment within this many lines of the top.
SPDX_MAX_LINE = 10

# Budget IDs live in this file (PLAN.md section 4: "budgets.json is the source of truth").
BUDGETS_FILE = "tests/perf/budgets.json"

# Narrow (ANSI code page) Win32 APIs. FrostIDE uses the wide (...W) APIs only (CLAUDE.md,
# PLAN.md 2.1 and 6.5): the narrow ones mangle non-ASCII paths and ignore long paths.
# Matched as whole identifiers in C/C++ code, never in comments or string literals.
NARROW_WIN32_APIS = frozenset(
    """
    CallNamedPipeA CopyFileA CopyFileExA CreateDirectoryA CreateDirectoryExA CreateEventA
    CreateEventExA CreateFileA CreateFileMappingA CreateHardLinkA CreateMutexA CreateMutexExA
    CreateNamedPipeA CreateProcessA CreateProcessAsUserA CreateSemaphoreA CreateSymbolicLinkA
    CreateWindowExA DefWindowProcA DeleteFileA DispatchMessageA ExpandEnvironmentStringsA
    FindFirstChangeNotificationA FindFirstFileA FindFirstFileExA FindNextFileA FindWindowA
    FindWindowExA FormatMessageA GetCommandLineA GetComputerNameA GetCurrentDirectoryA
    GetDiskFreeSpaceA GetDiskFreeSpaceExA GetDriveTypeA GetEnvironmentVariableA
    GetFileAttributesA GetFileAttributesExA GetFinalPathNameByHandleA GetFullPathNameA
    GetLongPathNameA GetMessageA GetModuleFileNameA GetModuleFileNameExA GetModuleHandleA
    GetModuleHandleExA GetPrivateProfileStringA GetShortPathNameA GetSystemDirectoryA
    GetTempFileNameA GetTempPathA GetUserNameA GetVolumeInformationA GetWindowsDirectoryA
    GetWindowTextA LoadCursorA LoadIconA LoadImageA LoadLibraryA LoadLibraryExA LoadStringA
    MessageBoxA MessageBoxExA MoveFileA MoveFileExA MoveFileWithProgressA OpenEventA
    OpenFileMappingA OpenMutexA OpenSemaphoreA OutputDebugStringA PathFileExistsA PeekMessageA
    PostMessageA RegCreateKeyA RegCreateKeyExA RegDeleteKeyA RegDeleteKeyExA RegDeleteValueA
    RegEnumKeyExA RegEnumValueA RegGetValueA RegOpenKeyA RegOpenKeyExA RegQueryValueExA
    RegSetValueExA RegisterClassA RegisterClassExA RemoveDirectoryA ReplaceFileA SearchPathA
    SendMessageA SetCurrentDirectoryA SetEnvironmentVariableA SetFileAttributesA SetWindowTextA
    SHFileOperationA SHGetFolderPathA SHGetSpecialFolderPathA ShellExecuteA ShellExecuteExA
    WaitNamedPipeA WritePrivateProfileStringA
    """.split()
)

# Functions that look an executable up in the current directory or PATH, or hand a command
# line to a shell that does. FrostIDE resolves every tool to an absolute path and starts it
# through IProcessLauncher (CLAUDE.md, PLAN.md 3.5). SearchPathW is among them because it
# still searches the current directory in safe search mode (see ProcessHardening in
# src/app/include/frost/app/startup.hpp). Matched as calls (the name followed by "("),
# unqualified or qualified with std:: or ::, never as members or in comments and strings.
EXE_SEARCH_APIS = frozenset(
    """
    SearchPathW WinExec system _wsystem popen _popen _wpopen
    _spawnlp _spawnlpe _spawnvp _spawnvpe _wspawnlp _wspawnlpe _wspawnvp _wspawnvpe
    _execlp _execlpe _execvp _execvpe _wexeclp _wexeclpe _wexecvp _wexecvpe
    execlp execvp execvpe posix_spawnp
    """.split()
)

# Files exempt from the exe-search rule only (every other rule still applies):
#   tests/app/startup_test.cpp  shows that SearchPathW still finds a tool planted in the
#                               current directory after frost's hardening, the reason
#                               SearchPathW is in EXE_SEARCH_APIS.
EXE_SEARCH_EXEMPT_GLOBS = ("tests/app/startup_test.cpp",)

# The only directories that may include Qt Svg headers (the build-time icon baker).
QT_SVG_ALLOWED_GLOBS = ("**/icon_baker/**",)

# Developer machine paths (FND-10). A product never guesses where a developer keeps the SDK, the
# build outputs or the user's live FrostEngine tree; tests take the SDK from FROST_SDK and corpora
# from FROST_CORPUS_ROOT. Checked in whole source files (code, strings and comments) under
# DEV_PATH_GLOBS; fixtures and goldens (DATA_GLOBS) are data and exempt. The development machine's
# roots (E:\sdk, E:\FrostBuilds; the live tree on any drive) with any run of separators, also in
# the WSL /mnt/<drive>/ spelling. Made-up roots in unit tests (C:\sdk on a MemFs) are not matched.
DEV_PATH_GLOBS = ("src/**", "tests/**")
DEV_PATH_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("the SDK root", re.compile(r"(?i)(?:\bE:|/mnt/e)[\\/]+sdk(?![\w.-])")),
    ("the build-output root", re.compile(r"(?i)(?:\bE:|/mnt/e)[\\/]+FrostBuilds(?![\w.-])")),
    ("the user's live FrostEngine tree",
     re.compile(r"(?i)(?:\b[a-z]:|/mnt/[a-z])[\\/]+dev[\\/]+FrostEngine-AI-Claude(?![\w.-])")),
)

# ---------------------------------------------------------------------------------------
# Truth rule (ADR 0078 decision 1, FND-5): result types, tool-output strings, parity claims
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ResultType:
    """
    A type whose values the IDE shows as a tool's result. Values may be constructed only in
    `parsers`: files (REUSE globs) that build them from a process stream started through
    IProcessLauncher, from an LSP or DAP message, or from another result type that one of
    those produced. For an enum, `outcomes` are the enumerators that state a result (Passed,
    Failed); producing one of them counts as constructing the result.
    """

    name: str
    category: str
    parsers: tuple[str, ...] = ()
    outcomes: tuple[str, ...] = ()


# The result types of the truth rule. A type listed with no parser has no real source yet:
# every construction is a violation until its owning epic adds a parser and lists it here, and
# that change goes through the independent review (ADR 0078 decision 4). Types are matched by
# name in C, C++ and C# code outside tests/** (test doubles are welcome there, decision 2).
# Names declared in several modules (Diagnostic) list every module's own parser.
RESULT_TYPES: tuple[ResultType, ...] = (
    # Test outcomes and test discovery. RUN-7 (gtest, CTest), RUN-8 (doctest), RUN-10 (the Unit
    # Tests window) and LTR-8 (dotnet test) add the parsers of ctest -N, --gtest_list_tests and
    # the test processes' own output.
    ResultType("UnitTestRunState", "test outcome", (), ("Passed", "Failed", "Skipped")),
    ResultType("DiscoveredTestEntry", "test discovery"),
    # Reserved for RUN-7's result model, so it starts behind the rule (R0 exit criterion).
    ResultType("TestResult", "test outcome"),
    ResultType("TestOutcome", "test outcome", (), ("Passed", "Failed", "Skipped")),
    # Packages: PackageReference items read from the user's own .csproj are file contents, not
    # invented; a package catalogue needs a real feed (LTR-7).
    ResultType("NuGetPackageEntry", "package", ("src/app/src/project_properties_xml.cpp",)),
    ResultType("CatalogItem", "package"),
    # Usage counts and usages: LSP textDocument/references and Code Vision lenses.
    ResultType("Usage", "usage", ("src/lang/src/navigation.cpp",)),
    ResultType("UsageGroup", "usage", ("src/lang/src/navigation.cpp",)),
    ResultType("EditorCodeVisionLens", "usage count"),
    # IL and disassembly: the DAP disassemble response.
    ResultType("DisassembledInstruction", "disassembly", ("src/debug/src/dap_types.cpp",)),
    # Diagnostics: LSP publishDiagnostics, the build output parsers and their editor adapter.
    # settings::Diagnostic, theme_compiler::Diagnostic and io::IgnoreDiagnostic are FrostIDE's
    # own validators reporting on its own inputs, never a tool's result.
    ResultType("LiveDiagnostic", "diagnostic", ("src/lang/src/diagnostics.cpp",)),
    ResultType("BuildDiagnostic", "diagnostic", ("src/build/src/**",)),
    ResultType("Diagnostic", "diagnostic",
               ("src/build/src/**", "src/settings/src/**", "src/ui/theme_compiler/**")),
    ResultType("EditorDiagnostic", "diagnostic", ("src/ui/lang/src/editor_insight_binding.cpp",)),
)

# Declared types whose names end like a result type's (RESULT_NAME_SUFFIX) but are not results:
# counters, folders, highlights and settings types. Anything else declared under that suffix in
# src/** must join RESULT_TYPES (rule result-type-registry), so a new result type cannot skip
# the allowlist by being new.
RESULT_NAME_SUFFIX = re.compile(r"(?:TestEntry|TestResult|TestOutcome|TestRunState|PackageEntry|Instruction|"
                                r"Diagnostic|Usage|UsageGroup|CodeVisionLens)$")
NOT_RESULT_TYPES = frozenset({
    "IgnoreDiagnostic",  # io: a malformed line in a .gitignore the IDE itself parsed
    "ActiveDiagnostic",  # ui/editor: which EditorDiagnostic is under the caret (no new data)
})


@dataclass(frozen=True)
class SiteAllowance:
    """
    One construction site allowed by exact text, for files that mix a real result with a
    harm that another slice removes. `line` is the reported source line, stripped and with
    whitespace runs collapsed; `context`, when given, must appear (collapsed the same way) in
    that line and the SITE_CONTEXT_LINES after it, which tells apart sites whose first lines
    are equal. A moved site still matches, an edited one does not. `count` is the number of
    findings the entry covers: a second identical site beyond it is reported (so one reviewed
    entry cannot silently cover a new copy), and an entry that covers fewer than `count`
    findings, none above all, is itself a violation, so a removed harm cannot leave its entry
    behind.
    """

    path: str
    symbol: str  # the result type or tool-output marker name
    line: str
    reason: str
    context: str = ""
    count: int = 1


SITE_CONTEXT_LINES = 8


# Every entry is a reviewed site in a file that the fabricated-result or tool-output rule scans.
# The temporary FND-3 entries were deleted at the R0 integration together with their harms.
SITE_ALLOWANCES: tuple[SiteAllowance, ...] = (
    # Reviewed, labelled heuristic: the Unit Tests window lists tests found by regular expressions
    # over the source text and labels every one "Discovered, not run"; no outcome is shown. RUN-7
    # replaces it with ctest -N / --gtest_list_tests. Five sites, one per recogniser: C# test
    # attributes and Main, C++ gtest, Catch2 and main.
    SiteAllowance("src/app/src/project_unit_tests.cpp", "DiscoveredTestEntry", "DiscoveredTestEntry entry;",
                  "source-text discovery shown as 'Discovered, not run' until RUN-7", count=5),
    # Reviewed real site: the Problems window's live rows adapt LSP LiveDiagnostics (clangd, LuaLS,
    # the managed server) into BuildDiagnostic rows field by field.
    SiteAllowance("src/app/src/workbench_clangd.cpp", "BuildDiagnostic", "build::BuildDiagnostic row;",
                  "adapter of lang::LiveDiagnostic from publishDiagnostics", context="row.diagnostic.message = d.message;"),
    # Reviewed real site: the author lens shows git blame's author (VcsService through authorForLine).
    SiteAllowance("src/ui/lang/src/editor_insight_binding.cpp", "EditorCodeVisionLens",
                  "out.push_back(EditorCodeVisionLens{", "git blame author from VcsService (authorForLine)",
                  context=".label = author,"),
)

# Synthesized tool output: literals that imitate a tool's own output lines. Outside the files
# that parse that output (`parsers`, REUSE globs) and outside tests/**, a string literal that
# contains one is invented output (ADR 0078 decision 1). Each entry: name, pattern, parsers.
TOOL_OUTPUT_MARKERS: tuple[tuple[str, re.Pattern[str], tuple[str, ...]], ...] = (
    ("gtest", re.compile(r"\[ RUN      \]|\[       OK \]|\[  FAILED  \]|\[  PASSED  \]|\[  SKIPPED \]|"
                         r"\[==========\]|\[----------\]"), ()),
    ("xunit", re.compile(r"\[xUnit\.net"), ()),
    ("doctest", re.compile(r"\[doctest\]"), ()),
    ("ctest", re.compile(r"tests failed out of"), ()),
    ("dotnet-test", re.compile(r"(?:Passed|Failed)!\s+-\s+Failed:"), ()),
    ("il", re.compile(r"\.assembly extern\b|\bIL_[0-9A-Fa-f]{4}:|\.maxstack\b"), ()),
)

# Parity claims (FND-4). The checked documents and the UI string literals may describe goals
# and the evidence, never claim parity or a complete replacement: PLAN.md section 7 "Roadmap
# v2" and tests/features/matrix.json hold the evidence. PLAN.md, the ADRs and docs/progress.md
# are history and quote the old claims, so they are not checked.
PARITY_CLAIM_DOC_GLOBS = ("README.md", "RIDER_PARITY_TODO.md", "CONTRIBUTING.md", "SECURITY.md",
                          "integrations/**/*.md", "integrations/**/package.json")
PARITY_CLAIM_UI_GLOBS = ("src/ui/**", "src/app/**")
PARITY_CLAIMS: tuple[re.Pattern[str], ...] = tuple(re.compile(p, re.I) for p in (
    r"\bcomplete replacement\b",
    r"\bdrop-in replacement\b",
    r"\b(?:full|complete|total)\s+(?:JetBrains\s+)?(?:Rider|CLion|ReSharper|Visual Studio)\s+replacement\b",
    r"\b1\s*:\s*1\b[^.\n]{0,40}\b(?:parity|Rider|CLion|JetBrains|ReSharper|Visual Studio)\b",
    r"\b(?:parity|Rider|CLion)\b[^.\n]{0,20}\b1\s*:\s*1\b",
    r"\b(?:full|complete|total|100\s*%)\s+(?:[\w-]+\s+){0,3}parity\b",
    r"\bfeature[- ]complete\b",
))

# ---------------------------------------------------------------------------------------
# Comment syntax table
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Syntax:
    """Comment syntax of one source language; `lexer` selects the scanner below."""

    name: str
    lexer: str  # "c", "csharp", "hash", "xml" or "bat"
    # The "hash" lexer's line-comment character; tree-sitter queries use ';' (and '#' starts
    # their predicates).
    comment_char: str = "#"
    # Options for the "hash" lexer.
    quotes: tuple[str, ...] = ()  # single-line string delimiters
    triple_quotes: bool = False  # Python/TOML """...""" and '''...''' strings
    hash_word_start: bool = False  # '#' starts a comment only at the start of a word
    powershell: bool = False  # <# #> block comments, @'...'@ here-strings, ` escapes
    cmake: bool = False  # [[...]] bracket arguments and #[[...]] bracket comments
    ini: bool = False  # '#' or ';' comments at the start of a line only
    raw_single_quotes: bool = False  # '...' has no escapes (shell, PowerShell, YAML, TOML literals)
    c_family: bool = False  # C/C++ code: the narrow-win32 and cpp26 rules apply


CPP = Syntax("C/C++", "c", c_family=True)
CSHARP = Syntax("C#", "csharp")
RC = Syntax("Windows resource script", "c")
CMAKE = Syntax("CMake", "hash", quotes=('"',), cmake=True)
POWERSHELL = Syntax("PowerShell", "hash", quotes=("'", '"'), powershell=True, raw_single_quotes=True)
SHELL = Syntax("shell", "hash", quotes=("'", '"'), hash_word_start=True, raw_single_quotes=True)
PYTHON = Syntax("Python", "hash", quotes=("'", '"'), triple_quotes=True)
YAML = Syntax("YAML", "hash", quotes=("'", '"'), hash_word_start=True, raw_single_quotes=True)
TOML = Syntax("TOML", "hash", quotes=("'", '"'), triple_quotes=True, raw_single_quotes=True)
INI = Syntax("INI", "hash", ini=True)
# .gitignore and .gitattributes: '#' starts a comment only at the start of a line. The
# INI scanner also reads a leading ';' as one; harmless, since only comments are checked.
GIT_PATTERNS = Syntax("git pattern file", "hash", ini=True)
XML = Syntax("XML", "xml")
# tree-sitter queries (src/syntax/queries/**): ';' line comments, '"' strings with \ escapes.
TREE_SITTER_QUERY = Syntax("tree-sitter query", "hash", quotes=('"',), comment_char=";")
BATCH = Syntax("batch", "bat")

# Source files: extension -> syntax. Every file listed here (outside DATA_GLOBS) needs an
# SPDX header; every file not listed needs REUSE coverage. A `.in` suffix (CMake
# configure_file templates such as build_info.hpp.in or windows-frost.manifest.in) is
# stripped before the lookup.
SOURCE_SYNTAX_BY_SUFFIX: dict[str, Syntax] = {
    # C and C++, including C++20 module interface units so the modules ban can see them.
    ".h": CPP, ".hh": CPP, ".hpp": CPP, ".hxx": CPP, ".inl": CPP, ".ipp": CPP,
    ".c": CPP, ".cc": CPP, ".cpp": CPP, ".cxx": CPP, ".ixx": CPP, ".cppm": CPP,
    ".cs": CSHARP,
    ".rc": RC,
    ".cmake": CMAKE,
    ".ps1": POWERSHELL, ".psm1": POWERSHELL, ".psd1": POWERSHELL,
    ".sh": SHELL, ".bash": SHELL,
    ".py": PYTHON,
    ".yml": YAML, ".yaml": YAML,
    ".toml": TOML,
    # XML files we author: manifests, natvis, MSBuild props/targets/projects, WiX sources.
    ".xml": XML, ".manifest": XML, ".natvis": XML, ".props": XML, ".targets": XML,
    ".csproj": XML, ".wxs": XML, ".xsd": XML,
    ".bat": BATCH, ".cmd": BATCH,
    ".scm": TREE_SITTER_QUERY,
}
# Source files recognised by their full name.
SOURCE_SYNTAX_BY_NAME: dict[str, Syntax] = {
    "CMakeLists.txt": CMAKE,
    ".clang-format": YAML,
    ".clang-tidy": YAML,
    ".clangd": YAML,
    ".editorconfig": INI,
    ".gitignore": GIT_PATTERNS,
    ".gitattributes": GIT_PATTERNS,
    # git hooks (tools/hooks) are POSIX shell scripts without an extension.
    "commit-msg": SHELL,
}


def source_syntax(rel_path: str) -> Syntax | None:
    """Returns the comment syntax of a source file, or None for files without one."""
    name = rel_path.rsplit("/", 1)[-1]
    if name.endswith(".in") and len(name) > 3:
        name = name[:-3]
    if name in SOURCE_SYNTAX_BY_NAME:
        return SOURCE_SYNTAX_BY_NAME[name]
    dot = name.rfind(".")
    if dot <= 0:
        return None
    return SOURCE_SYNTAX_BY_SUFFIX.get(name[dot:].lower())


# ---------------------------------------------------------------------------------------
# Glob matching (REUSE.toml paths and .gitignore patterns)
# ---------------------------------------------------------------------------------------


def glob_regex(pattern: str, *, gitignore: bool = False) -> str:
    """
    Translates a path glob into a regular expression body (no anchors).

    REUSE 3.x semantics (default): `*` matches within one directory, `**` matches across
    directories, `**/` matches zero or more leading directories, and `\\*` is a literal
    asterisk; every other character is literal. With gitignore=True, `?` and `[...]` are
    wildcards too and a `**` that is not a whole path component behaves like `*`
    (gitignore(5)). Written from the two specifications; no code was borrowed.
    """
    out: list[str] = []
    i, n = 0, len(pattern)
    while i < n:
        c = pattern[i]
        if c == "\\" and i + 1 < n:
            out.append(re.escape(pattern[i + 1]))
            i += 2
            continue
        if c == "*":
            if pattern.startswith("**", i):
                j = i + 2
                whole_component = i == 0 or pattern[i - 1] == "/"
                if whole_component and j < n and pattern[j] == "/":
                    out.append("(?:.*/)?")
                    i = j + 1
                    continue
                if whole_component and j == n:
                    out.append(".*")
                    i = j
                    continue
                out.append("[^/]*" if gitignore else ".*")
                i = j
                continue
            out.append("[^/]*")
            i += 1
            continue
        if gitignore and c == "?":
            out.append("[^/]")
            i += 1
            continue
        if gitignore and c == "[":
            j = pattern.find("]", i + 2)
            if j > 0:
                body = pattern[i + 1 : j]
                if body.startswith("!"):
                    body = "^" + body[1:]
                out.append("[" + body.replace("\\", "\\\\") + "]")
                i = j + 1
                continue
        out.append(re.escape(c))
        i += 1
    return "".join(out)


def compile_globs(patterns) -> re.Pattern[str]:
    """Compiles REUSE-style globs into one anchored regular expression."""
    return re.compile("^(?:" + "|".join(glob_regex(p) for p in patterns) + ")$")


_DATA_RE = compile_globs(DATA_GLOBS)
_VENDORED_RE = compile_globs(VENDORED_GLOBS)
_CPP26_EXEMPT_RE = compile_globs(CPP26_EXEMPT_GLOBS)
_EXE_SEARCH_EXEMPT_RE = compile_globs(EXE_SEARCH_EXEMPT_GLOBS)
_DEV_PATH_RE = compile_globs(DEV_PATH_GLOBS)
_QT_SVG_ALLOWED_RE = compile_globs(QT_SVG_ALLOWED_GLOBS)


def is_data_path(rel_path: str) -> bool:
    """True for byte-exact data files (fixtures, goldens, fuzz corpora)."""
    return _DATA_RE.match(rel_path) is not None


def is_vendored_path(rel_path: str) -> bool:
    """True for vendored third-party sources (VENDORED_GLOBS)."""
    return _VENDORED_RE.match(rel_path) is not None


@dataclass
class _IgnoreRule:
    base: str  # directory of the .gitignore, "" for the root, else "dir/sub/"
    regex: re.Pattern[str]
    negate: bool
    dir_only: bool


def parse_gitignore(base: str, text: str) -> list[_IgnoreRule]:
    """Parses one .gitignore file located in directory `base` ("" or "a/b/")."""
    rules = []
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        negate = line.startswith("!")
        if negate:
            line = line[1:]
        elif line.startswith("\\"):
            line = line[1:]
        dir_only = line.endswith("/")
        line = line.rstrip("/")
        if not line:
            continue
        # A slash anywhere but the end anchors the pattern to the .gitignore's directory.
        anchored = "/" in line
        body = glob_regex(line.lstrip("/"), gitignore=True)
        if not anchored:
            body = "(?:.*/)?" + body
        rules.append(_IgnoreRule(base, re.compile("^" + body + "$"), negate, dir_only))
    return rules


def is_ignored(rel_path: str, is_dir: bool, rules: list[_IgnoreRule]) -> bool:
    """Applies gitignore rules in order; the last matching rule decides."""
    ignored = False
    for rule in rules:
        if rule.dir_only and not is_dir:
            continue
        if rule.base and not rel_path.startswith(rule.base):
            continue
        if rule.regex.match(rel_path[len(rule.base) :]):
            ignored = not rule.negate
    return ignored


def _walk_repo(repo: Path) -> list[str]:
    """Lists files like git would without a .git directory, honouring .gitignore files."""
    files: list[str] = []
    rules: list[_IgnoreRule] = []

    def visit(directory: Path, rel: str, inherited: list[_IgnoreRule]) -> None:
        local = list(inherited)
        gitignore = directory / ".gitignore"
        if gitignore.is_file():
            local += parse_gitignore(rel, gitignore.read_text(encoding="utf-8", errors="replace"))
        try:
            entries = sorted(os.scandir(directory), key=lambda e: e.name)
        except OSError:
            return
        for entry in entries:
            if entry.name == ".git":
                continue
            child = rel + entry.name
            if entry.is_dir(follow_symlinks=False):
                if not is_ignored(child, True, local):
                    visit(Path(entry.path), child + "/", local)
            elif entry.is_file(follow_symlinks=False) and not is_ignored(child, False, local):
                files.append(child)

    visit(repo, "", rules)
    return files


def _git_toplevel_is(repo: Path) -> str | None:
    """Returns the git executable if `repo` is the top level of a git work tree."""
    git = shutil.which("git")
    if not git:
        return None
    result = subprocess.run(
        [git, "-C", str(repo), "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        return None
    try:
        return git if os.path.samefile(result.stdout.strip(), repo) else None
    except OSError:
        return None


def list_repo_files(repo: Path) -> list[str]:
    """
    Lists the repository's files as POSIX paths relative to `repo`: tracked plus untracked
    files that are not ignored (deleted tracked files and non-regular entries such as
    submodules are dropped). Uses git when `repo` is a git work tree, else a walk.
    """
    git = _git_toplevel_is(repo)
    if git is None:
        return _walk_repo(repo)
    result = subprocess.run(
        [git, "-C", str(repo), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        capture_output=True, check=True,
    )
    names = sorted({p for p in result.stdout.decode("utf-8").split("\0") if p})
    return [p for p in names if (repo / p).is_file()]


# ---------------------------------------------------------------------------------------
# Lexers: split a file into code (comments and string contents blanked) and comments
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Comment:
    line: int  # 1-based line number
    text: str  # the comment text on that line, without the comment delimiters


@dataclass
class Scan:
    code: str  # the file text with comments and string contents replaced by spaces
    comments: list[Comment] = field(default_factory=list)
    # (start, end) offsets of string and character literal contents (C, C++ and C# only), so
    # rules can read what a literal says; `code` has them blanked.
    strings: list[tuple[int, int]] = field(default_factory=list)


def _blank(buf: list[str], start: int, end: int) -> None:
    """Replaces buf[start:end] with spaces, keeping newlines so offsets and lines hold."""
    for k in range(start, min(end, len(buf))):
        if buf[k] != "\n":
            buf[k] = " "


def _add_block(comments: list[Comment], line: int, body: str, strip_star: bool) -> None:
    for k, part in enumerate(body.split("\n")):
        text = part.strip()
        if strip_star:
            text = text.lstrip("*").strip()
        comments.append(Comment(line + k, text))


_IDENT_CHARS = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
_RAW_PREFIXES = {"R", "u8R", "uR", "UR", "LR"}
_RAW_OPEN = re.compile(r'([^()\\\s"]{0,16})\(')


def _token_before(text: str, i: int) -> str:
    """The identifier-like token that ends right before position i."""
    k = i
    while k > 0 and text[k - 1] in _IDENT_CHARS:
        k -= 1
    return text[k:i]


def _scan_c(text: str, csharp: bool) -> Scan:
    """Lexes C, C++, C# and resource scripts: // and /* */ comments, string and char literals."""
    buf = list(text)
    comments: list[Comment] = []
    strings: list[tuple[int, int]] = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            comments.append(Comment(line, text[i + 2 : j].lstrip("/!").strip()))
            _blank(buf, i, j)
            i = j
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            body = text[i + 2 : (n if j < 0 else j)]
            _add_block(comments, line, body.lstrip("*!"), strip_star=True)
            line += text.count("\n", i, end)
            _blank(buf, i, end)
            i = end
        elif c == '"' or c == "'":
            prefix = _token_before(text, i)
            if c == '"' and not csharp and prefix in _RAW_PREFIXES:
                m = _RAW_OPEN.match(text, i + 1)
                if m:
                    close = ")" + m.group(1) + '"'
                    j = text.find(close, m.end())
                    end = n if j < 0 else j + len(close)
                    line += text.count("\n", i, end)
                    strings.append((m.end(), n if j < 0 else j))
                    _blank(buf, i + 1, end - 1)
                    i = end
                    continue
            if c == '"' and csharp and i > 0 and text[i - 1] == "@":
                j = i + 1
                while j < n:
                    if text[j] == '"':
                        if j + 1 < n and text[j + 1] == '"':
                            j += 2
                            continue
                        break
                    j += 1
                end = min(j + 1, n)
                line += text.count("\n", i, end)
                strings.append((i + 1, min(j, n)))
                _blank(buf, i + 1, end - 1)
                i = end
                continue
            if c == "'" and not csharp and prefix and prefix[0].isdigit():
                i += 1  # C++14 digit separator, e.g. 1'000'000
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n)
            terminated = j < n and text[j] == c
            end = j + 1 if terminated else j
            line += text.count("\n", i, end)
            strings.append((i + 1, j))
            _blank(buf, i + 1, end - 1 if terminated else end)
            i = end
        else:
            i += 1
    return Scan("".join(buf), comments, strings)


_CMAKE_BRACKET = re.compile(r"\[(=*)\[")


def _scan_hash(text: str, syntax: Syntax) -> Scan:
    """Lexes '#'-comment languages: CMake, PowerShell, shell, Python, YAML, TOML, INI."""
    buf = list(text)
    comments: list[Comment] = []
    i, n, line = 0, len(text), 1
    at_line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r":
            i += 1
            continue
        line_start, at_line_start = at_line_start, False
        if syntax.ini:
            if line_start and c in "#;":
                j = text.find("\n", i)
                j = n if j < 0 else j
                comments.append(Comment(line, text[i + 1 : j].strip()))
                _blank(buf, i, j)
                i = j
            else:
                j = text.find("\n", i)
                i = n if j < 0 else j
            continue
        if syntax.powershell and text.startswith("<#", i):
            j = text.find("#>", i + 2)
            end = n if j < 0 else j + 2
            _add_block(comments, line, text[i + 2 : (n if j < 0 else j)], strip_star=False)
            line += text.count("\n", i, end)
            _blank(buf, i, end)
            i = end
            continue
        if syntax.powershell and c == "@" and text[i + 1 : i + 2] in ("'", '"'):
            # Here-string: @' or @" at the end of a line, closed by '@ or "@ at a line start.
            quote = text[i + 1]
            m = re.compile(r"\n" + re.escape(quote) + "@").search(text, i + 2)
            end = n if m is None else m.end()
            line += text.count("\n", i, end)
            _blank(buf, i + 2, end - 2)
            i = end
            continue
        if syntax.cmake and c == "#":
            m = _CMAKE_BRACKET.match(text, i + 1)
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, m.end())
                end = n if j < 0 else j + len(close)
                _add_block(comments, line, text[m.end() : (n if j < 0 else j)], strip_star=False)
                line += text.count("\n", i, end)
                _blank(buf, i, end)
                i = end
                continue
        if syntax.cmake and c == "[":
            m = _CMAKE_BRACKET.match(text, i)
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, m.end())
                end = n if j < 0 else j + len(close)
                line += text.count("\n", i, end)
                _blank(buf, m.end(), end - len(close))
                i = end
                continue
        if c == syntax.comment_char:
            prev = text[i - 1] if i > 0 else "\n"
            if not syntax.hash_word_start or prev in " \t\r\n;&|()":
                j = text.find("\n", i)
                j = n if j < 0 else j
                comments.append(Comment(line, text[i + 1 : j].strip()))
                _blank(buf, i, j)
                i = j
                continue
            i += 1
            continue
        if syntax.triple_quotes and text.startswith(('"""', "'''"), i):
            close = text[i : i + 3]
            j = text.find(close, i + 3)
            end = n if j < 0 else j + 3
            line += text.count("\n", i, end)
            _blank(buf, i + 3, end - 3)
            i = end
            continue
        if c in syntax.quotes:
            # Strings end at the closing quote or, if unterminated, at the end of the line.
            # Single quotes are raw where the language says so; elsewhere \ (or `) escapes.
            escape = "`" if syntax.powershell else "\\"
            raw = c == "'" and syntax.raw_single_quotes
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if (not raw and text[j] == escape) else 1
            j = min(j, n)
            terminated = j < n and text[j] == c
            end = j + 1 if terminated else j
            line += text.count("\n", i, end)
            _blank(buf, i + 1, end - 1 if terminated else end)
            i = end
            continue
        if c == "\\" and syntax.hash_word_start:
            i += 2  # shell escape such as \# or \"
            continue
        i += 1
    return Scan("".join(buf), comments)


def _scan_xml(text: str) -> Scan:
    """Lexes XML: only <!-- --> comments matter."""
    buf = list(text)
    comments: list[Comment] = []
    pos = 0
    while True:
        i = text.find("<!--", pos)
        if i < 0:
            break
        j = text.find("-->", i + 4)
        end = len(text) if j < 0 else j + 3
        line = text.count("\n", 0, i) + 1
        _add_block(comments, line, text[i + 4 : (len(text) if j < 0 else j)], strip_star=False)
        _blank(buf, i, end)
        pos = end
    return Scan("".join(buf), comments)


_BAT_COMMENT = re.compile(r"^[ \t]*(?:@?rem(?=[ \t]|$)|::)(.*)$", re.IGNORECASE | re.MULTILINE)


def _scan_bat(text: str) -> Scan:
    """Lexes batch files: REM and :: comment lines."""
    buf = list(text)
    comments = []
    for m in _BAT_COMMENT.finditer(text):
        comments.append(Comment(text.count("\n", 0, m.start()) + 1, m.group(1).strip()))
        _blank(buf, m.start(), m.end())
    return Scan("".join(buf), comments)


def scan(text: str, syntax: Syntax) -> Scan:
    """Splits `text` into blanked code and comment lines according to `syntax`."""
    if syntax.lexer in ("c", "csharp"):
        return _scan_c(text, syntax.lexer == "csharp")
    if syntax.lexer == "xml":
        return _scan_xml(text)
    if syntax.lexer == "bat":
        return _scan_bat(text)
    return _scan_hash(text, syntax)


# ---------------------------------------------------------------------------------------
# Violations and rules
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    rule: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.message}"


class _Lines:
    """Maps character offsets in a text to 1-based line numbers."""

    def __init__(self, text: str) -> None:
        self._starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def line(self, offset: int) -> int:
        return bisect.bisect_right(self._starts, offset)


_SPDX_TAG = re.compile(r"SPDX-License-Identifier:\s*(\S.*?)\s*(?:-->|\*/|#>)?\s*$")
_WORD = re.compile(r"[A-Za-z]{2,}")


def _first_code_line(scan_result: Scan, syntax: Syntax) -> int:
    """1-based number of the first line holding code (preambles excluded); a large value if none."""
    for number, line in enumerate(scan_result.code.split("\n"), start=1):
        stripped = line.strip()
        if not stripped:
            continue
        if number == 1 and syntax.lexer == "xml" and stripped.startswith("<?xml"):
            continue
        if number == 1 and syntax.lexer == "bat" and stripped.lower() == "@echo off":
            continue
        return number
    return 1 << 30


def check_header(rel: str, text: str, syntax: Syntax, scanned: Scan) -> list[Violation]:
    """spdx-header: an SPDX line near the top and a descriptive header comment."""
    first_code = _first_code_line(scanned, syntax)
    header = [c for c in scanned.comments if c.line < first_code]
    out = []
    spdx = [c for c in header if _SPDX_TAG.search(c.text)]
    if not spdx or spdx[0].line > SPDX_MAX_LINE:
        anywhere = [c for c in scanned.comments if "SPDX-License-Identifier:" in c.text]
        where = anywhere[0].line if anywhere else 1
        if anywhere:
            message = (f"the SPDX-License-Identifier line must be in the header comment within the "
                       f"first {SPDX_MAX_LINE} lines")
        else:
            message = f"missing 'SPDX-License-Identifier:' header line ({syntax.name} comment)"
        out.append(Violation(rel, where, "spdx-header", message))
    described = any(
        len(_WORD.findall(c.text)) >= 3
        for c in header
        if "SPDX-" not in c.text and not (c.line == 1 and c.text.startswith("!"))
    )
    if not described:
        out.append(Violation(rel, 1, "spdx-header",
                             "the header comment must say what the file is for (purpose, module)"))
    return out


# A to-do marker: the word at the start of a comment line, or followed by ':' or '('.
_TODO_WORD = "TO" "DO"  # split so this file's own code is not read as a marker
_TODO_MARKER = re.compile(r"\b" + _TODO_WORD + r"\b")
_TODO_VALID = re.compile(_TODO_WORD + r"\(M(\d{1,2})\)")


def check_todos(rel: str, comments: list[Comment]) -> list[Violation]:
    """todo: markers must read TODO(Mn) with n in 0..17 and name an open milestone."""
    out = []
    for comment in comments:
        for m in _TODO_MARKER.finditer(comment.text):
            following = comment.text[m.end() : m.end() + 1]
            at_start = comment.text[: m.start()].strip(" \t-*/#;!") == ""
            if not at_start and following not in (":", "("):
                continue  # prose mentioning the word, e.g. "the TO DO window"
            valid = _TODO_VALID.match(comment.text, m.start())
            if not valid:
                out.append(Violation(rel, comment.line, "todo",
                                     f"to-do markers must read {_TODO_WORD}(Mn) with a milestone "
                                     f"M{FIRST_MILESTONE}..M{LAST_MILESTONE}"))
                continue
            number = int(valid.group(1))
            if not FIRST_MILESTONE <= number <= LAST_MILESTONE:
                out.append(Violation(rel, comment.line, "todo",
                                     f"M{number} is not a milestone (M{FIRST_MILESTONE}..M{LAST_MILESTONE})"))
            elif number in CLOSED_MILESTONES:
                out.append(Violation(rel, comment.line, "todo",
                                     f"M{number} is closed; finish the work or move the marker"))
    return out


_BUDGET_REF = re.compile(r"\bBudget:\s*(.*)")
_BUDGET_ID = re.compile(r"\bB\d+\b")


def check_budgets(rel: str, comments: list[Comment], budget_ids: set[str] | None) -> list[Violation]:
    """budget: every budget reference names IDs that exist in budgets.json."""
    out = []
    for comment in comments:
        for m in _BUDGET_REF.finditer(comment.text):
            ids = _BUDGET_ID.findall(m.group(1))
            if not ids:
                out.append(Violation(rel, comment.line, "budget",
                                     "budget reference names no budget ID (expected e.g. 'Budget: B5.')"))
            for budget in ids:
                if budget_ids is None:
                    out.append(Violation(rel, comment.line, "budget",
                                         f"{budget}: {BUDGETS_FILE} is missing or unreadable"))
                elif budget not in budget_ids:
                    out.append(Violation(rel, comment.line, "budget",
                                         f"{budget} is not defined in {BUDGETS_FILE}"))
    return out


_IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")


def check_narrow_win32(rel: str, code: str, lines: _Lines) -> list[Violation]:
    """narrow-win32: no narrow (...A) Win32 APIs in C/C++ code."""
    out = []
    for m in _IDENTIFIER.finditer(code):
        if m.group(0) in NARROW_WIN32_APIS:
            wide = m.group(0)[:-1] + "W"
            out.append(Violation(rel, lines.line(m.start()), "narrow-win32",
                                 f"narrow Win32 API {m.group(0)} is banned; use {wide} (PLAN.md 2.1)"))
    return out


# A call of one of EXE_SEARCH_APIS: not preceded by a word character, '.', '>' (member
# access) or ':' (another class's or namespace's function), except through `std::` or `::`.
_EXE_SEARCH_CALL = re.compile(r"(?<![\w.>:])(?:std\s*::\s*|::\s*)?\b(" +
                              "|".join(sorted(EXE_SEARCH_APIS)) + r")\s*\(")


def check_dev_paths(rel: str, text: str) -> list[Violation]:
    """dev-path: no developer machine path (DEV_PATH_PATTERNS) in a source file under DEV_PATH_GLOBS."""
    if not _DEV_PATH_RE.match(rel):
        return []
    lines = _Lines(text)
    return [Violation(rel, lines.line(m.start()), "dev-path",
                      f"{m.group(0)!r} is a developer machine path ({what}); search beside the executable, then "
                      f"FROST_SDK, then PATH, and let tests read FROST_SDK or FROST_CORPUS_ROOT (FND-10)")
            for what, pattern in DEV_PATH_PATTERNS for m in pattern.finditer(text)]


def check_exe_search(rel: str, code: str, lines: _Lines) -> list[Violation]:
    """exe-search: no calls that look an executable up in the current directory or PATH."""
    return [Violation(rel, lines.line(m.start(1)), "exe-search",
                      f"{m.group(1)} looks the executable up in the current directory or PATH; resolve the "
                      f"tool to an absolute path and start it through IProcessLauncher (PLAN.md 3.5)")
            for m in _EXE_SEARCH_CALL.finditer(code)]


_CPP26_HEADERS = ("inplace_vector", "hazard_pointer", "rcu", "linalg", "debugging",
                  "text_encoding", "contracts", "meta", "simd", "hive")
_CPP26_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^[ \t]*#[ \t]*embed\b", re.M), "#embed is banned"),
    (re.compile(r"\bcontract_assert\b"), "contracts syntax (contract_assert) is banned; use FROST_ASSERT"),
    (re.compile(r"\^\^"), "reflection (the ^^ operator) is banned"),
    (re.compile(r"\[:(?!:)[^\n]*?:\]"), "reflection splices ([: ... :]) are banned"),
    (re.compile(r"\btemplate\s+for\b"), "expansion statements (template for) are banned"),
    (re.compile(r"^[ \t]*export[ \t]+module\b", re.M), "named modules (export module) are banned"),
    (re.compile(r"^[ \t]*module[ \t]*;", re.M), "named modules (the global module fragment 'module;') are banned"),
    (re.compile(r"^[ \t]*module[ \t]+[A-Za-z_][\w.]*(?:[ \t]*:[ \t]*[A-Za-z_][\w.]*)?[ \t]*;", re.M),
     "named modules (module declarations) are banned"),
    (re.compile(r'^[ \t]*(?:export[ \t]+)?import[ \t]+(?::?[ \t]*[A-Za-z_][\w.:]*|<[^>\n]*>|"[^"\n]*")[ \t]*;', re.M),
     "named modules (import declarations, including import std) are banned"),
    (re.compile(r"^[ \t]*#[ \t]*include[ \t]*<execution>", re.M), "std::execution (<execution>) is banned"),
    (re.compile(r"^[ \t]*#[ \t]*include[ \t]*<(" + "|".join(_CPP26_HEADERS) + r")>", re.M),
     "direct C++26 library headers are banned; go through frost/compat"),
]

# Contract specifiers: `pre(...)`/`post(...)` after a function declarator. The scan is
# deliberately conservative: it only fires when the specifier follows the closing
# parenthesis of a parameter list (optionally through cv/ref/noexcept qualifiers) whose
# opening parenthesis follows a function name, a lambda introducer `]` or template
# arguments `>`, or when it follows a trailing return type `) -> T`. Calls such as
# `if (ready) post(task);` are not matched.
_PRE_POST = re.compile(r"\b(pre|post)\s*\(")
_QUALIFIERS = {"const", "volatile", "noexcept", "override", "final", "mutable"}
_NOT_DECLARATORS = {
    "if", "while", "for", "switch", "catch", "return", "sizeof", "alignof", "alignas", "decltype",
    "typeid", "requires", "defined", "static_assert", "case", "co_await", "co_yield", "co_return",
    "throw", "new", "delete", "and", "or", "not", "__attribute__", "__declspec", "assert",
}
_TRAILING_RETURN = re.compile(r"\)\s*->\s*[\w:<>,\s*&]*$")


# An #include directive in lexed code; group 1 is the opening delimiter. Quoted names are
# blanked in the lexed code, so the name is read from the original text at the same offset.
_INCLUDE_OPEN = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])', re.M)
# A Qt Svg or Qt Svg Widgets header: a path component starting QSvg, QtSvg or QGraphicsSvg in
# any case (<QSvgRenderer>, <qsvgrenderer.h>, <QtSvgWidgets/...>, <qgraphicssvgitem.h>).
_QT_SVG_HEADER = re.compile(r"(?:^|/)(?:qt?svg|qgraphicssvg)", re.I)


def check_qt_svg(rel: str, text: str, code: str, lines: _Lines) -> list[Violation]:
    """qt-svg: Qt Svg headers (class, lower-case and module spellings) only in the icon baker."""
    out = []
    for m in _INCLUDE_OPEN.finditer(code):
        close = ">" if m.group(1) == "<" else '"'
        end = text.find(close, m.end())
        newline = text.find("\n", m.end())
        if end < 0 or 0 <= newline < end:
            continue
        name = text[m.end() : end].strip()
        if _QT_SVG_HEADER.search(name):
            out.append(Violation(rel, lines.line(m.start()), "qt-svg",
                                 f"#include of {name}: Qt Svg is linked only by tools/icon_baker; the runtime "
                                 f"draws baked icon atlases (ADR 0015)"))
    return out


def _skip_space_back(code: str, k: int) -> int:
    while k >= 0 and code[k] in " \t\r\n":
        k -= 1
    return k


def _matching_open(code: str, close: int) -> int:
    depth = 0
    for k in range(close, -1, -1):
        if code[k] == ")":
            depth += 1
        elif code[k] == "(":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _is_contract_specifier(code: str, start: int) -> bool:
    line_start = code.rfind("\n", 0, start) + 1
    if code[line_start:start].lstrip().startswith("#"):
        return False  # preprocessor line, e.g. a macro definition
    k = _skip_space_back(code, start - 1)
    for _ in range(16):  # bounded walk over qualifiers
        if k < 0:
            return False
        if code[k] == ")":
            open_paren = _matching_open(code, k)
            if open_paren < 0:
                return False
            before = _skip_space_back(code, open_paren - 1)
            if before < 0:
                return False
            if code[before] in "]>":
                return True
            token = _token_before(code, before + 1)
            if token == "noexcept":
                k = _skip_space_back(code, before - len(token))
                continue
            return bool(token) and not token[0].isdigit() and token not in _NOT_DECLARATORS
        if code[k] == "&":
            k = _skip_space_back(code, k - 1)
            continue
        token = _token_before(code, k + 1)
        if token in _QUALIFIERS:
            k = _skip_space_back(code, k - len(token))
            continue
        if token in ("pre", "post"):
            return False  # handled when the earlier specifier itself is checked
        return bool(_TRAILING_RETURN.search(code, max(0, k - 200), k + 1))
    return False


def check_cpp26(rel: str, code: str, lines: _Lines) -> list[Violation]:
    """cpp26: the C++26 constructs PLAN.md 2.1 bans in v1."""
    out = []
    for pattern, message in _CPP26_PATTERNS:
        for m in pattern.finditer(code):
            out.append(Violation(rel, lines.line(m.start()), "cpp26", message + " (PLAN.md 2.1)"))
    for m in _PRE_POST.finditer(code):
        if _is_contract_specifier(code, m.start()):
            out.append(Violation(rel, lines.line(m.start()), "cpp26",
                                 f"contracts syntax ({m.group(1)}(...) specifier) is banned; "
                                 f"use FROST_PRE/FROST_POST (PLAN.md 2.1)"))
    return out


# ---------------------------------------------------------------------------------------
# Truth-rule checks: fabricated-result, result-type-registry, synthesized-output, parity-claim
# ---------------------------------------------------------------------------------------

# tests/** may build results and imitate tool output: doubles feed the parsers (ADR 0078 decision 2).
_TRUTH_EXEMPT_RE = compile_globs(("tests/**",))
_HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
_PARITY_DOC_RE = compile_globs(PARITY_CLAIM_DOC_GLOBS)
_PARITY_UI_RE = compile_globs(PARITY_CLAIM_UI_GLOBS)
_RESULT_BY_NAME = {t.name: t for t in RESULT_TYPES}


@dataclass(frozen=True)
class _Hit:
    """A truth-rule finding before the site allowances are applied."""

    line: int
    rule: str
    symbol: str
    message: str


def _qualified(name: str) -> str:
    """A regex for `name` as a whole identifier, optionally namespace-qualified, not a member."""
    return r"(?<![\w.>])(?:\w+\s*::\s*)*\b" + re.escape(name) + r"\b"


def _before(code: str, start: int, width: int = 120) -> str:
    return code[max(0, start - width) : start]


# A type definition or base-class list right before the name: `struct T {`, `class X : public T {`.
_DEFINITION_TAIL = re.compile(r"\b(?:struct|class|union|enum|public|private|protected|typename|friend)\s+"
                              r"(?:\[\[[^\]]*\]\]\s*)?(?:\w+\s*::\s*)*$")
# A trailing return type right before the name: `auto f() -> T {` opens a function body.
_TRAILING_ARROW = re.compile(r"->\s*(?:[\w:]+\s*<?\s*)*$")
_COMPARISON_TAIL = re.compile(r"(?:==|!=|\bcase)\s*$")


def _construction_patterns(result: ResultType, header: bool) -> list[tuple[re.Pattern[str], str]]:
    """The construction forms of `result` (see check_fabricated_results), each with a label."""
    name = _qualified(result.name)
    # In a header, `T member;` and `T member{};` declare a defaulted member, not a result.
    brace = r"\{(?!\s*\})" if header else r"\{"
    forms = [
        # T{...}: a braced temporary or aggregate, designated initializers included.
        (re.compile(name + r"\s*\{"), "braced"),
        # T x{...}; T x = {...}; T x = T{...}; and, outside headers, T x; and T x{};
        (re.compile(name + r"\s+[A-Za-z_]\w*\s*(?:" + brace + r"|=\s*" + brace + r"|=\s*(?:\w+\s*::\s*)*" +
                    re.escape(result.name) + r"\s*" + brace + ("" if header else r"|;") + ")"), "declaration"),
        # A container literal: std::array<T, 8> k{{...}}, std::vector<T>{...}.
        (re.compile(r"<\s*(?:const\s+)?" + name + r"\s*(?:,[^<>;{}]*)?>\s*(?:[A-Za-z_]\w*\s*)?(?:=\s*)?\{(?!\s*\})"),
         "container literal"),
        # make_shared<T>, make_unique<T>, make_optional<T>.
        (re.compile(r"\bmake_(?:shared|unique|optional)\s*<\s*" + name + r"\s*>"), "factory"),
        # T x[N] = {...}; T x[]{...}; and, outside headers, T x[N]; (a C array of results).
        (re.compile(name + r"\s+[A-Za-z_]\w*\s*\[[^\]\n]*\]\s*(?:=\s*)?" +
                    (r"\{(?!\s*\})" if header else r"(?:\{|;)")), "C array"),
        # static_cast<T>(...), bit_cast<T>(...), reinterpret_cast<T>(...): a value converted into a result
        # (for an enum, an outcome produced from a number). Casts to T& or T* do not construct.
        (re.compile(r"\b(?:static_cast|reinterpret_cast|bit_cast)\s*<\s*(?:const\s+)?" + name + r"\s*>"), "cast"),
        # T x(...); and T(...): checked further by _paren_construction (function declarations and types
        # also have this shape).
        (re.compile(name + r"\s+[A-Za-z_]\w*\s*\("), "parenthesized declaration"),
        (re.compile(r"(?<!~)" + name + r"\s*\("), "temporary"),
    ]
    if result.outcomes:
        forms.append((re.compile(name + r"\s*::\s*(?:" + "|".join(result.outcomes) + r")\b"), "outcome"))
    return forms


# The words that can start or make up a parameter's type, so `(const T& x)` reads as a parameter
# list and not as constructor arguments.
_TYPE_WORDS = frozenset("""
    auto bool char char8_t char16_t char32_t class const double enum float int long short signed
    std struct typename unsigned void volatile wchar_t size_t ptrdiff_t
""".split())
# One parameter declaration: a type (qualified, templated, with & * or const) and an optional name
# and default; or a bare type spelled like one (a builtin, qualified, a pointer or reference, or a
# capitalised name: FrostIDE spells types in UpperCamelCase and variables in lowerCamelCase).
_PARAMETER = re.compile(r"^(?:(?:const|volatile|struct|class|enum|typename|unsigned|signed|long|short)\s+)*"
                        r"[A-Za-z_][\w:]*(?:\s*<[^()]*>)?(?:\s*::\s*\w+)*(?:\s*(?:const|volatile))?"
                        r"(?:[\s&*]+(?:const\s+)?[A-Za-z_]\w*(?:\s*\[[^\]]*\])?(?:\s*=.*)?|\s*[&*]+)$", re.S)
_BARE_TYPE = re.compile(r"^(?:const\s+)?(?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*(?:\s*<[^()]*>)?\s*[&*]*$")


def _closing_paren(code: str, open_index: int) -> int:
    """The index of the ')' that closes the '(' at `open_index`, or -1."""
    depth = 0
    for i in range(open_index, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i if c == ")" else -1
    return -1


def _split_arguments(text: str) -> list[str]:
    """`text` split at the commas outside brackets, each part stripped; [] for an empty list."""
    parts, depth, current = [], 0, []
    for c in text:
        if c in "([{<":
            depth += 1
        elif c in ")]}>" and depth > 0:
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
        else:
            current.append(c)
    last = "".join(current).strip()
    return parts + [last] if (parts or last) else []


def _is_parameter(argument: str) -> bool:
    """Whether `argument` reads as a parameter declaration rather than a value."""
    if argument in ("...", "void") or _PARAMETER.match(argument):
        return True
    if _BARE_TYPE.match(argument):
        word = argument.replace("const ", "").strip()
        return "::" in word or word[-1:] in "&*" or word in _TYPE_WORDS or word[:1].isupper()
    return False


def _paren_construction(code: str, m: re.Match[str], form: str, type_name: str) -> bool:
    """
    Whether the `T x(...)` or `T(...)` at `m` constructs a value. A parameter list (every argument
    reads as a parameter) is a function declaration or a function type; `T x(...)` followed by a body,
    qualifiers or `= default` is a function; `T x();` is the vexing-parse declaration; `T::T(` is a
    constructor definition; `T()` in a declaration context (a class body: after ; { } : or a
    specifier) followed by ; = { : or a qualifier declares a constructor; `T()` between < or , and
    > or , is a function type in a template argument (std::function<T()>).
    """
    open_index = m.end() - 1
    close = _closing_paren(code, open_index)
    if close < 0:
        return False
    arguments = _split_arguments(code[open_index + 1 : close])
    after = code[close + 1 : close + 40].lstrip()
    if arguments and all(_is_parameter(a) for a in arguments):
        return False
    if arguments and arguments[0][:1] in ("*", "&"):
        return False  # A function pointer or reference type: T(*)(...).
    if form == "parenthesized declaration":
        if not after[:1] in (";", ","):
            return False  # A function definition, a qualified declaration, or not a declaration at all.
        return bool(arguments)  # `T x();` declares a function.
    # A temporary T(...).
    if re.search(r"\b" + re.escape(type_name) + r"\s*::\s*" + re.escape(type_name) + r"\s*\($", m.group(0)):
        return False  # T::T( defines a constructor.
    before = _before(code, m.start(), 60).rstrip()
    if not arguments:
        if re.search(r"(?:^|[;{}]|(?<!:):|\b(?:explicit|inline|constexpr|consteval|virtual|friend))$", before) and \
                re.match(r"(?:;|=|\{|:(?!:)|noexcept\b|const\b|override\b|->)", after):
            return False  # A constructor declaration in a class body.
        if before.endswith(("<", ",")) and after[:1] in (">", ","):
            return False  # std::function<T()>, a function type.
    return True


# The element containers whose implicit braced or emplaced elements construct a result.
_CONTAINER_TEMPLATE = r"(?:std\s*::\s*)?(?:vector|deque|list|forward_list|optional|QList|QVector|span|set|multiset)"
_ELEMENT_INSERTION = r"(?:push_back|push_front|emplace_back|emplace_front|emplace|insert)"


def _element_constructions(result: ResultType, code: str) -> list[tuple[int, str]]:
    """
    Results built as elements of a container declared in this file with the result as its element
    type (`std::vector<T> rows;`, a parameter `std::vector<T>& out`, a member `QList<T> m_rows;`):
    `rows.push_back({...})` (implicit braced), `rows.emplace_back(a, b)` and `rows.emplace_back()`
    (emplaced; a single argument may be a copy or move of an existing result, so it is not counted),
    and `rows = {{...}}` / `rows.insert(rows.end(), {...})`. Containers declared in another file
    (a member declared in a header, a type alias) are not resolved: the lexer reads one file at a time.
    """
    name = _qualified(result.name)
    declared = re.compile(r"\b" + _CONTAINER_TEMPLATE + r"\s*<\s*(?:const\s+)?" + name +
                          r"\s*(?:,[^<>;{}()]*)?>\s*(?:const\s*)?[&*]*\s*([A-Za-z_]\w*)\b")
    variables = {m.group(1) for m in declared.finditer(code)}
    out = []
    for variable in sorted(variables):
        # Not a member of something else (`.all = {` in a designated initializer is not `all`).
        member = r"(?<![\w.>:])" + re.escape(variable) + r"\s*(?:\.|->)\s*"
        for m in re.finditer(member + _ELEMENT_INSERTION + r"\s*\(", code):
            close = _closing_paren(code, m.end() - 1)
            if close < 0:
                continue
            arguments = _split_arguments(code[m.end() : close])
            method = re.search(_ELEMENT_INSERTION, m.group(0)).group(0)  # type: ignore[union-attr]
            if any(a.startswith("{") for a in arguments):
                out.append((m.start(), "implicit braced element"))
            elif method.startswith("emplace") and (len(arguments) == 0 or
                                                   len(arguments) >= (3 if method == "emplace" else 2)):
                out.append((m.start(), "emplaced element"))
        for m in re.finditer(r"(?<![\w.>:])" + re.escape(variable) + r"\s*=\s*\{(?!\s*\})", code):
            out.append((m.start(), "container literal"))
    return out


def check_fabricated_results(rel: str, code: str, lines: _Lines) -> list[_Hit]:
    """
    fabricated-result: a RESULT_TYPES value constructed outside the type's allowlisted parsers.
    Constructions are recognised structurally in lexed code (comments and strings blanked):
    braced temporaries and aggregates, declarations with an initializer (braced, `= ...` or
    parenthesized, or default-constructed outside headers), temporaries T(...), C arrays,
    container literals, implicit braced or emplaced elements of a container of results declared
    in the same file, make_shared/make_unique/make_optional, casts (static_cast<T>), and, for
    enums, producing an outcome enumerator (anything but comparing with == or != or a case
    label). Not recognised, so the independent review still reads them (ADR 0078 decision 4):
    `return {...};` from a function returning a result (it needs the enclosing function's
    declared type), elements of a container declared in another file or behind a type alias,
    and conversions hidden behind a function. One finding per line and type.
    """
    if _TRUTH_EXEMPT_RE.match(rel):
        return []
    header = rel.endswith(_HEADER_SUFFIXES)
    out = []
    for result in RESULT_TYPES:
        if result.name not in code:
            continue
        if result.parsers and compile_globs(result.parsers).match(rel):
            continue
        found: list[tuple[int, str]] = []
        for pattern, form in _construction_patterns(result, header):
            for m in pattern.finditer(code):
                before = _before(code, m.start())
                if form in ("braced", "declaration", "C array", "parenthesized declaration", "temporary") and \
                        _DEFINITION_TAIL.search(before):
                    continue
                if form in ("braced", "container literal", "temporary") and _TRAILING_ARROW.search(before):
                    continue
                if form in ("parenthesized declaration", "temporary") and \
                        not _paren_construction(code, m, form, result.name):
                    continue
                if form == "outcome" and (_COMPARISON_TAIL.search(before) or
                                          re.match(r"\s*(?:==|!=)", code[m.end() : m.end() + 4])):
                    continue
                found.append((m.start(), form))
        found += _element_constructions(result, code)
        where = ("; allowed only in " + ", ".join(result.parsers)) if result.parsers else \
                "; no parser feeds it yet, so its owning epic must add one and list it in RESULT_TYPES"
        reported: set[int] = set()
        for offset, form in sorted(found):
            line = lines.line(offset)
            if line in reported:
                continue  # One finding per line and type, whichever forms matched there.
            reported.add(line)
            out.append(_Hit(line, "fabricated-result", result.name,
                            f"{result.name} ({result.category}, {form}) is built outside a parser of a real "
                            f"tool's output{where} (ADR 0078 decision 1)"))
    return out


_TYPE_DECLARATION = re.compile(r"\b(?:struct|class|enum\s+class|enum\s+struct|enum)\s+(?:\[\[[^\]]*\]\]\s*)?"
                               r"([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{()]*)?\{")


def check_result_registry(rel: str, code: str, lines: _Lines) -> list[_Hit]:
    """result-type-registry: types under src/** named like a result type are in RESULT_TYPES."""
    if not rel.startswith("src/"):
        return []
    out = []
    for m in _TYPE_DECLARATION.finditer(code):
        name = m.group(1)
        if RESULT_NAME_SUFFIX.search(name) and name not in _RESULT_BY_NAME and name not in NOT_RESULT_TYPES:
            out.append(_Hit(lines.line(m.start(1)), "result-type-registry", name,
                            f"{name} is named like a tool result; list it in RESULT_TYPES with the parsers that "
                            f"may build it (or in NOT_RESULT_TYPES with a reason) in tools/frost_lint.py"))
    return out


_LITERAL_GAP = re.compile(r"\s*(?:u8|u|U|L)?")


def _literal_groups(text: str, strings: list[tuple[int, int]]) -> list[list[tuple[int, int]]]:
    """The string literals' content ranges, adjacent literals ("a" "b", only whitespace and the next
    literal's encoding prefix L, u, U or u8 between the closing and the next opening quote) grouped
    together, as the compiler concatenates them."""
    groups: list[list[tuple[int, int]]] = []
    previous_end = -2
    for start, end in strings:
        gap = text[previous_end + 1 : start - 1] if previous_end >= 0 else None
        if groups and gap is not None and _LITERAL_GAP.fullmatch(gap) and start - 1 > previous_end:
            groups[-1].append((start, end))
        else:
            groups.append([(start, end)])
        previous_end = end
    return groups


def _joined_literals(text: str, strings: list[tuple[int, int]]) -> list[tuple[int, str]]:
    """String literal contents, with adjacent literals ("a" "b") joined; each with its start offset."""
    return [(group[0][0], "".join(text[s:e] for s, e in group)) for group in _literal_groups(text, strings)]


def check_tool_output_strings(rel: str, text: str, scanned: Scan, lines: _Lines) -> list[_Hit]:
    """
    synthesized-output: string literals that imitate a tool's output lines (TOOL_OUTPUT_MARKERS).
    The markers are matched in the joined text of adjacent literals, so splitting a marker across
    literals ("[ RUN " "     ]") does not hide it; each match is reported at the source line where
    it starts, once per line and marker.
    """
    if _TRUTH_EXEMPT_RE.match(rel):
        return []
    markers = [(name, pattern) for name, pattern, parsers in TOOL_OUTPUT_MARKERS
               if not (parsers and compile_globs(parsers).match(rel))]
    out = []
    seen: set[tuple[int, str]] = set()
    for group in _literal_groups(text, scanned.strings):
        joined = "".join(text[s:e] for s, e in group)
        # offsets[i]: where segment i starts in `joined`, to map a match back to the source.
        offsets, total = [], 0
        for s, e in group:
            offsets.append(total)
            total += e - s
        for name, pattern in markers:
            for m in pattern.finditer(joined):
                segment = bisect.bisect_right(offsets, m.start()) - 1
                line = lines.line(group[segment][0] + m.start() - offsets[segment])
                if (line, name) in seen:
                    continue
                seen.add((line, name))
                split = " (split across adjacent literals)" if \
                    bisect.bisect_right(offsets, m.end() - 1) - 1 != segment else ""
                out.append(_Hit(line, "synthesized-output", name,
                                f"string literal imitates {name} output ({m.group(0)!r}){split}; show only what "
                                f"the tool printed, parsed from its process stream (ADR 0078 decision 1)"))
    return out


def check_parity_claims_in_document(rel: str, text: str) -> list[Violation]:
    """parity-claim (documents): no parity or complete-replacement claims in PARITY_CLAIM_DOC_GLOBS."""
    out = []
    for number, line in enumerate(text.split("\n"), start=1):
        for pattern in PARITY_CLAIMS:
            m = pattern.search(line)
            if m:
                out.append(Violation(rel, number, "parity-claim",
                                     f"claims {m.group(0)!r}; state goals and the evidence instead (PLAN.md section 7 "
                                     f"'Roadmap v2', tests/features/matrix.json; FND-4)"))
    return out


def check_parity_claims_in_strings(rel: str, text: str, scanned: Scan, lines: _Lines) -> list[Violation]:
    """parity-claim (UI strings): no parity claims in string literals under PARITY_CLAIM_UI_GLOBS."""
    if not _PARITY_UI_RE.match(rel):
        return []
    out = []
    for start, literal in _joined_literals(text, scanned.strings):
        for pattern in PARITY_CLAIMS:
            m = pattern.search(literal)
            if m:
                out.append(Violation(rel, lines.line(start), "parity-claim",
                                     f"UI string claims {m.group(0)!r}; describe goals and evidence instead (FND-4)"))
    return out


def _collapse(text: str) -> str:
    return " ".join(text.split())


def apply_site_allowances(rel: str, text: str, hits: list[_Hit], used: dict[int, int]) -> list[Violation]:
    """
    Drops hits that a SITE_ALLOWANCES entry covers, at most entry.count per entry, counting them in
    `used` (entry index -> findings covered). A matching hit beyond an entry's count is reported
    with a note that the entry is exhausted: a new copy of a reviewed site needs its own review.
    """
    source_lines = text.split("\n")
    out = []
    for hit in hits:
        reported = _collapse(source_lines[hit.line - 1]) if 0 < hit.line <= len(source_lines) else ""
        window = _collapse(" ".join(source_lines[hit.line - 1 : hit.line - 1 + SITE_CONTEXT_LINES + 1]))
        allowed = False
        exhausted = None
        for index, entry in enumerate(SITE_ALLOWANCES):
            if entry.path == rel and entry.symbol == hit.symbol and _collapse(entry.line) == reported and \
                    (not entry.context or _collapse(entry.context) in window):
                if used.get(index, 0) >= entry.count:
                    exhausted = entry
                    continue
                used[index] = used.get(index, 0) + 1
                allowed = True
                break
        if not allowed:
            note = (f" (the SITE_ALLOWANCES entry for this line covers {exhausted.count} finding(s) and they are "
                    f"all taken: a new identical site needs its own review and count)") if exhausted else ""
            out.append(Violation(rel, hit.line, hit.rule, hit.message + note))
    return out


def stale_site_allowances(linted: set[str], used: dict[int, int]) -> list[Violation]:
    """Every SITE_ALLOWANCES entry for a linted file that covered fewer findings than its count."""
    out = []
    for index, entry in enumerate(SITE_ALLOWANCES):
        if entry.path not in linted or used.get(index, 0) >= entry.count:
            continue
        covered = used.get(index, 0)
        what = "no site matches" if covered == 0 else f"only {covered} of its {entry.count} findings match"
        out.append(Violation(entry.path, 1, "fabricated-result",
                             f"stale site allowance for {entry.symbol} ({entry.reason}): {what} "
                             f"{entry.line!r}{' with ' + repr(entry.context) if entry.context else ''}; delete the "
                             f"entry or correct its count in SITE_ALLOWANCES in tools/frost_lint.py"))
    return out


# ---------------------------------------------------------------------------------------
# REUSE.toml coverage
# ---------------------------------------------------------------------------------------


@dataclass
class _ReuseFile:
    base: str  # directory holding the REUSE.toml, "" or "dir/"
    patterns: list[re.Pattern[str]]


def load_reuse(repo: Path, files: list[str]) -> tuple[list[_ReuseFile], list[Violation]]:
    """Loads every REUSE.toml outside the data directories."""
    loaded, problems = [], []
    for rel in files:
        if rel.rsplit("/", 1)[-1] != "REUSE.toml" or is_data_path(rel):
            continue
        base = rel[: -len("REUSE.toml")]
        try:
            data = tomllib.loads((repo / rel).read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
            problems.append(Violation(rel, 1, "reuse", f"cannot parse REUSE.toml: {error}"))
            continue
        if data.get("version") != 1:
            problems.append(Violation(rel, 1, "reuse", "REUSE.toml must declare version = 1"))
        patterns = []
        for index, annotation in enumerate(data.get("annotations", []), start=1):
            if "SPDX-License-Identifier" not in annotation or "SPDX-FileCopyrightText" not in annotation:
                problems.append(Violation(rel, 1, "reuse",
                                          f"annotation {index} lacks SPDX-License-Identifier or "
                                          f"SPDX-FileCopyrightText"))
                continue
            paths = annotation.get("path", [])
            if isinstance(paths, str):
                paths = [paths]
            patterns.append(compile_globs(paths))
        loaded.append(_ReuseFile(base, patterns))
    return loaded, problems


def reuse_covered(rel: str, reuse: list[_ReuseFile], file_set: set[str]) -> bool:
    """True when a REUSE.toml annotation or a `<file>.license` sidecar covers `rel`."""
    if rel + ".license" in file_set:
        return True
    for entry in reuse:
        if entry.base and not rel.startswith(entry.base):
            continue
        local = rel[len(entry.base) :]
        if any(p.match(local) for p in entry.patterns):
            return True
    return False


# ---------------------------------------------------------------------------------------
# Source-to-test line ratchet (ADR 0078 decision 6, FND-5)
# ---------------------------------------------------------------------------------------

# A slice is the difference between the merge base of HEAD and the verify base (main) and the
# working tree, untracked files included. Source lines grow by the added minus the deleted lines
# of code files under RATCHET_SOURCE_GLOBS, so moving or reformatting code costs nothing; test
# lines are the added lines of code files under tests/** outside the data globs (fixtures,
# goldens and corpora do not count). When the source grows by RATCHET_MIN_SOURCE_LINES or more,
# the slice needs at least RATCHET_MIN_TEST_RATIO test lines per source line. Calibrated on the
# history: the reviewed M0-M5 slices ran 0.19 to 2.2 test lines per source line, the
# feature-first commits that ADR 0077 audited 0.00 to 0.16 (1c29790: 9,372 against 196). Only
# files with comment syntax (SOURCE_SYNTAX_BY_SUFFIX) count; Markdown and JSON do not.
RATCHET_SOURCE_GLOBS = ("src/**", "integrations/**")
RATCHET_TEST_GLOBS = ("tests/**",)
RATCHET_MIN_SOURCE_LINES = 200
RATCHET_MIN_TEST_PERCENT = 20

_RATCHET_SOURCE_RE = compile_globs(RATCHET_SOURCE_GLOBS)
_RATCHET_TEST_RE = compile_globs(RATCHET_TEST_GLOBS)


class RatchetError(Exception):
    """The slice could not be measured (git missing, the base does not resolve)."""


@dataclass
class RatchetCounts:
    """Lines a slice adds and deletes (see RATCHET_MIN_SOURCE_LINES)."""

    source_added: int = 0
    source_deleted: int = 0
    test_added: int = 0

    @property
    def source_net(self) -> int:
        return self.source_added - self.source_deleted


def ratchet_kind(rel: str) -> str | None:
    """'source', 'test' or None (not counted) for a repository-relative path."""
    if source_syntax(rel) is None or is_vendored_path(rel) or is_data_path(rel):
        return None
    if _RATCHET_SOURCE_RE.match(rel):
        return "source"
    if _RATCHET_TEST_RE.match(rel):
        return "test"
    return None


def ratchet_verdict(counts: RatchetCounts) -> tuple[bool, str]:
    """(passed, summary item) for a measured slice; the item reads 'ratchet ok (...)' or 'ratchet FAILED (...)'."""
    net, tests = counts.source_net, counts.test_added
    measured = f"src {net:+d} net, tests +{tests}"
    if net < RATCHET_MIN_SOURCE_LINES:
        return True, f"ratchet ok ({measured})"
    needed = -(-net * RATCHET_MIN_TEST_PERCENT // 100)  # the ceiling of net * percent / 100
    if tests >= needed:
        return True, f"ratchet ok ({measured})"
    return False, (f"ratchet FAILED ({measured}; {net} source lines need {needed} test lines, "
                   f"{RATCHET_MIN_TEST_PERCENT} per 100)")


def _git_run(repo: Path, git_dir: str | None, work_tree: str | None, *args: str) -> bytes:
    """Runs git (absolute path, no shell) in `repo`; raises RatchetError on failure."""
    exe = shutil.which("git")
    if exe is None:
        raise RatchetError("git is not on PATH")
    cmd = [os.path.abspath(exe), "-C", str(repo)]
    if git_dir:
        cmd.append(f"--git-dir={git_dir}")
    if work_tree:
        cmd.append(f"--work-tree={work_tree}")
    result = subprocess.run([*cmd, *args], capture_output=True, stdin=subprocess.DEVNULL, check=False)
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip().splitlines()
        raise RatchetError(f"git {' '.join(args)} failed: {message[-1] if message else result.returncode}")
    return result.stdout


def _count_lines(data: bytes) -> int:
    """Lines in a text file's bytes; 0 for binary data."""
    if not data or b"\0" in data[:8192]:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def measure_slice(repo: Path, base: str = "main", git_dir: str | None = None,
                  work_tree: str | None = None) -> RatchetCounts:
    """Counts the slice's source and test lines against merge-base(HEAD, base); raises RatchetError."""
    commit = None
    for candidate in (base, f"origin/{base}"):
        try:
            commit = _git_run(repo, git_dir, work_tree, "rev-parse", "--verify", "--quiet",
                              f"{candidate}^{{commit}}").decode("utf-8").strip()
            break
        except RatchetError:
            continue
    if not commit:
        raise RatchetError(f"the base {base!r} does not resolve (tried {base} and origin/{base})")
    merge_base = _git_run(repo, git_dir, work_tree, "merge-base", "HEAD", commit).decode("utf-8").strip()
    counts = RatchetCounts()
    numstat = _git_run(repo, git_dir, work_tree, "diff", "--numstat", "-z", "--no-renames", merge_base, "--")
    for record in numstat.decode("utf-8", "replace").split("\0"):
        parts = record.split("\t", 2)
        if len(parts) != 3 or not parts[0].isdigit() or not parts[1].isdigit():
            continue  # a binary file ("-") or the empty tail
        kind = ratchet_kind(parts[2])
        if kind == "source":
            counts.source_added += int(parts[0])
            counts.source_deleted += int(parts[1])
        elif kind == "test":
            counts.test_added += int(parts[0])
    untracked = _git_run(repo, git_dir, work_tree, "ls-files", "--others", "--exclude-standard", "-z")
    for rel in untracked.decode("utf-8", "replace").split("\0"):
        kind = ratchet_kind(rel) if rel else None
        path = Path(work_tree or repo) / rel
        if kind is None or not path.is_file():
            continue
        lines = _count_lines(path.read_bytes())
        if kind == "source":
            counts.source_added += lines
        else:
            counts.test_added += lines
    return counts


def ratchet_main(repo: Path, base: str, git_dir: str | None, work_tree: str | None) -> int:
    """Prints the ratchet's summary item as the last stdout line; returns 0 (ok, not-run) or 1 (failed)."""
    try:
        counts = measure_slice(repo, base, git_dir, work_tree)
    except (RatchetError, OSError) as error:
        print(f"ratchet not-run ({error})")
        return 0
    passed, item = ratchet_verdict(counts)
    print(f"frost-lint ratchet: against the merge base with {base}: source +{counts.source_added} "
          f"-{counts.source_deleted}, tests +{counts.test_added}", file=sys.stderr)
    print(item)
    return 0 if passed else 1


# ---------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------


def load_budget_ids(repo: Path) -> set[str] | None:
    """Budget IDs from tests/perf/budgets.json, or None if it is missing or malformed."""
    try:
        data = json.loads((repo / BUDGETS_FILE).read_text(encoding="utf-8"))
        return {str(row["id"]) for row in data["budgets"]}
    except (OSError, ValueError, KeyError, TypeError):
        return None


def _read_text(path: Path) -> str | None:
    """The file as text (UTF-8, optional BOM), or None for binary or undecodable files."""
    data = path.read_bytes()
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None


def lint_file(repo: Path, rel: str, budget_ids: set[str] | None, used: dict[int, int] | None = None) -> list[Violation]:
    """Runs the content rules on one source file; `used` counts the findings each SITE_ALLOWANCES entry covered."""
    syntax = source_syntax(rel)
    assert syntax is not None
    text = _read_text(repo / rel)
    if text is None:
        return [Violation(rel, 1, "spdx-header", "source file is not UTF-8 text")]
    text = text.replace("\r\n", "\n")
    scanned = scan(text, syntax)
    out = check_header(rel, text, syntax, scanned)
    out += check_todos(rel, scanned.comments)
    out += check_budgets(rel, scanned.comments, budget_ids)
    out += check_dev_paths(rel, text)
    if syntax.c_family:
        lines = _Lines(scanned.code)
        out += check_narrow_win32(rel, scanned.code, lines)
        if not _EXE_SEARCH_EXEMPT_RE.match(rel):
            out += check_exe_search(rel, scanned.code, lines)
        if not _CPP26_EXEMPT_RE.match(rel):
            out += check_cpp26(rel, scanned.code, lines)
        if not _QT_SVG_ALLOWED_RE.match(rel):
            out += check_qt_svg(rel, text, scanned.code, lines)
    if syntax.lexer in ("c", "csharp"):
        lines = _Lines(scanned.code)
        hits = check_fabricated_results(rel, scanned.code, lines)
        hits += check_result_registry(rel, scanned.code, lines)
        hits += check_tool_output_strings(rel, text, scanned, lines)
        out += apply_site_allowances(rel, text, hits, {} if used is None else used)
        out += check_parity_claims_in_strings(rel, text, scanned, lines)
    return out


def lint(repo: Path, files: list[str] | None = None) -> list[Violation]:
    """Lints the repository at `repo` (or only `files`, POSIX paths relative to it)."""
    if files is None:
        files = list_repo_files(repo)
    file_set = set(files)
    reuse, violations = load_reuse(repo, files)
    budget_ids = load_budget_ids(repo)
    if (repo / ".github" / "workflows").is_dir() or any(f.startswith(".github/workflows/") for f in files):
        violations.append(Violation(".github/workflows", 1, "no-ci",
                                    "CI workflows are banned; verification is local only (D24)"))
    used: dict[int, int] = {}
    for rel in files:
        if _PARITY_DOC_RE.match(rel):
            violations.extend(check_parity_claims_in_document(rel, (_read_text(repo / rel) or "").replace("\r\n", "\n")))
        if rel.endswith(".license"):
            text = _read_text(repo / rel) or ""
            if "SPDX-License-Identifier:" not in text:
                violations.append(Violation(rel, 1, "reuse", "sidecar lacks an SPDX-License-Identifier line"))
            continue
        if is_data_path(rel) or is_vendored_path(rel) or source_syntax(rel) is None:
            if not reuse_covered(rel, reuse, file_set):
                violations.append(Violation(
                    rel, 1, "reuse",
                    "not covered by REUSE.toml or a .license sidecar (if this file type has comment "
                    "syntax, add its extension to SOURCE_SYNTAX_BY_SUFFIX (or its name to "
                    "SOURCE_SYNTAX_BY_NAME) in tools/frost_lint.py and give it a header)"))
            continue
        violations.extend(lint_file(repo, rel, budget_ids, used))
    violations.extend(stale_site_allowances(file_set, used))
    return sorted(set(violations))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="frost-lint v0: FrostIDE repository conventions")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("command", nargs="?", choices=("lint", "ratchet"), default="lint",
                        help="lint (default), or ratchet: the source-to-test line ratchet of this slice")
    parser.add_argument("--base", default="main", help="ratchet: the base whose merge base with HEAD starts the slice")
    parser.add_argument("--git-dir", default=None, help="ratchet: passed to git as --git-dir (verify.sh)")
    parser.add_argument("--work-tree", default=None, help="ratchet: passed to git as --work-tree (verify.sh)")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    if not repo.is_dir():
        print(f"frost-lint: {repo} is not a directory", file=sys.stderr)
        return 2
    if args.command == "ratchet":
        return ratchet_main(repo, args.base, args.git_dir, args.work_tree)
    # A failing git or an unreadable tree is an environment error, not a violation.
    try:
        files = list_repo_files(repo)
        violations = lint(repo, files)
    except subprocess.CalledProcessError as error:
        stderr = error.stderr.decode("utf-8", "replace") if isinstance(error.stderr, bytes) else error.stderr
        print(f"frost-lint: {(stderr or '').strip() or error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"frost-lint: {error}", file=sys.stderr)
        return 2
    for violation in violations:
        print(violation)
    print(f"frost-lint: {len(files)} files, {len(violations)} violation(s)", file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
