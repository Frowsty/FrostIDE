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
#   spdx-header   source files (SOURCE_SYNTAX below) start with a header comment that
#                 holds an SPDX-License-Identifier line and says what the file is for;
#   reuse         every other file is covered by a REUSE.toml annotation or a
#                 `<file>.license` sidecar (a `reuse lint` equivalent);
#   todo          to-do markers in comments name an open milestone M0..M14;
#   budget        every budget reference ("Budget: B5.") exists in tests/perf/budgets.json;
#   narrow-win32  narrow (...A) Win32 APIs are banned in C/C++ (PLAN.md 2.1);
#   cpp26         banned C++26 constructs in C/C++ outside the exempt directories;
#   no-ci         there is no .github/workflows directory (D24).
# Byte-exact data (fixtures, goldens, fuzz corpora; DATA_GLOBS) only needs REUSE coverage.
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
    sys.exit("frost_lint.py needs Python 3.11 or newer (tomllib)")

import tomllib  # noqa: E402  (after the version check on purpose)

# ---------------------------------------------------------------------------------------
# Policy constants
# ---------------------------------------------------------------------------------------

# Milestones are M0..M14 (PLAN.md section 7).
FIRST_MILESTONE = 0
LAST_MILESTONE = 14
# Milestones whose exit has been signed off. A to-do marker naming one of them is a
# violation, because the work it describes should have landed (PLAN.md 6.6). None are
# closed yet; add the number here at each milestone sign-off.
CLOSED_MILESTONES: frozenset[int] = frozenset()

# Paths (REUSE glob syntax, relative to the repository root) that hold byte-exact data:
# fixtures, goldens and fuzz corpora (PLAN.md 6.6 "Every other file"). They must stay
# byte-identical, so only the REUSE coverage rule applies to them. Mirrors the `-text`
# entries in .gitattributes.
DATA_GLOBS = ("tests/**/fixtures/**", "tests/**/goldens/**", "tests/fuzz/corpus/**")

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

# ---------------------------------------------------------------------------------------
# Comment syntax table
# ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Syntax:
    """Comment syntax of one source language; `lexer` selects the scanner below."""

    name: str
    lexer: str  # "c", "csharp", "hash", "xml" or "bat"
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
XML = Syntax("XML", "xml")
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
}
# Source files recognised by their full name.
SOURCE_SYNTAX_BY_NAME: dict[str, Syntax] = {
    "CMakeLists.txt": CMAKE,
    ".clang-format": YAML,
    ".clang-tidy": YAML,
    ".clangd": YAML,
    ".editorconfig": INI,
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
_CPP26_EXEMPT_RE = compile_globs(CPP26_EXEMPT_GLOBS)


def is_data_path(rel_path: str) -> bool:
    """True for byte-exact data files (fixtures, goldens, fuzz corpora)."""
    return _DATA_RE.match(rel_path) is not None


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
            _blank(buf, i + 1, end - 1 if terminated else end)
            i = end
        else:
            i += 1
    return Scan("".join(buf), comments)


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
        if c == "#":
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
    """todo: markers must read TODO(Mn) with n in 0..14 and name an open milestone."""
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


def lint_file(repo: Path, rel: str, budget_ids: set[str] | None) -> list[Violation]:
    """Runs the content rules on one source file."""
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
    if syntax.c_family:
        lines = _Lines(scanned.code)
        out += check_narrow_win32(rel, scanned.code, lines)
        if not _CPP26_EXEMPT_RE.match(rel):
            out += check_cpp26(rel, scanned.code, lines)
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
    for rel in files:
        if rel.endswith(".license"):
            text = _read_text(repo / rel) or ""
            if "SPDX-License-Identifier:" not in text:
                violations.append(Violation(rel, 1, "reuse", "sidecar lacks an SPDX-License-Identifier line"))
            continue
        if is_data_path(rel) or source_syntax(rel) is None:
            if not reuse_covered(rel, reuse, file_set):
                violations.append(Violation(
                    rel, 1, "reuse",
                    "not covered by REUSE.toml or a .license sidecar (if this file type has comment "
                    "syntax, add it to SOURCE_SYNTAX in tools/frost_lint.py and give it a header)"))
            continue
        violations.extend(lint_file(repo, rel, budget_ids))
    return sorted(set(violations))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="frost-lint v0: FrostIDE repository conventions")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    if not repo.is_dir():
        print(f"frost-lint: {repo} is not a directory", file=sys.stderr)
        return 2
    files = list_repo_files(repo)
    violations = lint(repo, files)
    for violation in violations:
        print(violation)
    print(f"frost-lint: {len(files)} files, {len(violations)} violation(s)", file=sys.stderr)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
