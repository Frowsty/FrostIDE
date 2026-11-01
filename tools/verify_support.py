#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# verify_support.py: the logic of tools/verify.ps1 and tools/verify.sh that deserves its own tests
# (PLAN.md section 6.4; ADR 0027). One file with no imports from tools/, because both scripts run it
# with `python -I`, which puts neither the script's directory nor the current one on sys.path.
#
#   python -I tools/verify_support.py changed --repo DIR [--git-dir DIR --work-tree DIR] [--base main]
#                                            [--out FILE]
#   python -I tools/verify_support.py tsan (--changed FILE | --all)
#   python -I tools/verify_support.py strip-path [--keep DIR]... VALUE
#   python -I tools/verify_support.py tidy --repo DIR --build-dir DIR [--config Debug]
#       (--changed FILE | --all [--reason TEXT] [--extra-build-dir DIR --extra-config CFG])
#       --clang-tidy ABS --ninja ABS [--jobs N] [--timeout 600]
#
# changed     The files that differ between merge-base(HEAD, base) and the working tree, plus the
#             untracked files git does not ignore: repository-relative POSIX paths, sorted and unique,
#             deleted files left out. git diff refreshes the stat data of the index it reads, so the
#             diff runs on a copy of the index in a temporary directory and the repository's own index
#             is never written. The base resolves as <base>, then origin/<base>. --out writes the list
#             atomically as UTF-8 with LF endings (no BOM), creating missing parent directories;
#             without it the list goes to stdout.
# tsan        Line 1: the modules whose TSan tests the WSL quick leg runs (space-separated), line 2:
#             the ctest label regex for them (^mod\.(io|text)$). Nothing when no module is affected;
#             --all names all four (rpc, io, sched, text).
# strip-path  VALUE (a PATH) without its drvfs entries (/mnt/<drive>, which make every CMake search
#             also probe the Windows drives over 9P), except those under a --keep directory. Order and
#             empty entries are kept. "dropped N" goes to stderr.
# tidy        Plans the clang-tidy run (the changed TUs, one includer per changed header, or every TU)
#             over the Debug entries of <build-dir>/compile_commands.json, writes that filtered,
#             deduplicated database to <build-dir>/frost-tidy/compile_commands.json, runs clang-tidy
#             once per TU with --jobs workers and prints each finding as "path:line:col: check:
#             message", each skipped file as "tidy: skipped <path>: <reason>", the raw output of every
#             TU that failed, and last a summary ("tidy ok (3 TUs)", "tidy FAILED (2 findings in 1
#             files)"). The configuration is the checked-in .clang-tidy nearest each TU.
#
# Exit status: 0 success (tidy: no finding); 1 tidy found findings or a TU failed; 2 usage or
# environment error (git failure, unresolvable base, no merge base, an invalid changed-file entry,
# a missing compile database, .clang-tidy, clang-tidy or ninja). The last stderr line says why.
# Python 3.11+, standard library only.

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any

EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_USAGE = 2


class VerifyError(Exception):
    """A usage or environment error; the CLI prints the message and exits 2."""


def _write_atomic(path: Path, text: str) -> None:
    """Writes `text` as UTF-8 (no BOM, LF kept) to `path` via a temporary file in its directory."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def _norm(path: str | os.PathLike[str]) -> str:
    """The comparison form of a path: absolute, normalised, and case-folded on Windows."""
    return os.path.normcase(os.path.abspath(os.fspath(path)))


# ---------------------------------------------------------------------------------------------
# Changed files
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class GitContext:
    """
    Where git runs: `repo` is the working tree's root. git_dir and work_tree, when given, are passed
    as --git-dir/--work-tree (verify.sh needs them for a worktree whose .git file names a Windows path).
    """

    repo: Path
    git_dir: Path | None = None
    work_tree: Path | None = None

    def command(self, *args: str) -> list[str]:
        """The git command line for `args` (absolute git; never a shell)."""
        exe = shutil.which("git")
        if exe is None:
            raise VerifyError("git is not on PATH")
        cmd = [os.path.abspath(exe), "-C", str(self.repo)]
        if self.git_dir:
            cmd.append(f"--git-dir={self.git_dir}")
        if self.work_tree:
            cmd.append(f"--work-tree={self.work_tree}")
        return [*cmd, *args]

    def run(self, *args: str, env: dict[str, str] | None = None, check: bool = True) -> subprocess.CompletedProcess:
        """Runs git; raises VerifyError on a non-zero exit when `check`. Output is bytes."""
        result = subprocess.run(self.command(*args), capture_output=True, env=env, stdin=subprocess.DEVNULL,
                                check=False)
        if check and result.returncode != 0:
            message = result.stderr.decode("utf-8", "replace").strip().splitlines()
            raise VerifyError(f"git {' '.join(args)} failed: {message[-1] if message else result.returncode}")
        return result


def _resolve_commit(ctx: GitContext, base: str) -> str:
    for candidate in (base, f"origin/{base}"):
        result = ctx.run("rev-parse", "--verify", "--quiet", f"{candidate}^{{commit}}", check=False)
        if result.returncode == 0:
            return result.stdout.decode("utf-8").strip()
    raise VerifyError(f"the base {base!r} does not resolve (tried {base} and origin/{base})")


def changed_files(ctx: GitContext, base: str = "main") -> list[str]:
    """
    The changed-file list (see the header): differences between merge-base(HEAD, base) and the working
    tree plus untracked, non-ignored files; repository-relative POSIX paths, sorted, unique, without
    deleted files. Four git calls (rev-parse --git-path, rev-parse of the base, merge-base, diff) plus
    ls-files, and one more rev-parse when the base falls back to origin/<base>. Raises VerifyError.
    """
    index = ctx.run("rev-parse", "--git-path", "index").stdout.decode("utf-8").strip()
    index_path = Path(index) if os.path.isabs(index) else Path(ctx.repo) / index
    base_commit = _resolve_commit(ctx, base)
    merge_base = ctx.run("merge-base", "HEAD", base_commit, check=False)
    if merge_base.returncode != 0:
        raise VerifyError(f"HEAD and {base} have no merge base")
    mb = merge_base.stdout.decode("utf-8").strip()
    with tempfile.TemporaryDirectory(prefix="frost-verify-index-") as tmp:
        copy = Path(tmp) / "index"
        if index_path.is_file():
            shutil.copyfile(index_path, copy)
        env = dict(os.environ, GIT_INDEX_FILE=str(copy), GIT_OPTIONAL_LOCKS="0")
        diff = ctx.run("diff", "--name-only", "-z", "--no-renames", "--diff-filter=d", "--no-ext-diff", mb, "--",
                       env=env).stdout
        others = ctx.run("ls-files", "-z", "--others", "--exclude-standard", env=env).stdout
    names: set[str] = set()
    for blob in (diff, others):
        for raw in blob.split(b"\0"):
            if not raw:
                continue
            name = raw.decode("utf-8", "surrogateescape")
            if "\n" in name or "\r" in name:
                raise VerifyError(f"a changed path contains a line break: {name!r}")
            names.add(name)
    return sorted(names)


def validate_changed(paths: Iterable[str]) -> list[str]:
    """
    Checks changed-list entries: repository-relative POSIX paths only. Raises VerifyError for an
    absolute path, a drive letter, a backslash or a `..` component; drops empty lines.
    """
    out = []
    for raw in paths:
        path = raw.rstrip("\r")
        if not path:
            continue
        if "\\" in path:
            raise VerifyError(f"changed-list entry has a backslash: {path!r}")
        if path.startswith("/") or re.match(r"^[A-Za-z]:", path):
            raise VerifyError(f"changed-list entry is absolute: {path!r}")
        if ".." in PurePosixPath(path).parts:
            raise VerifyError(f"changed-list entry contains '..': {path!r}")
        out.append(path)
    return out


def read_changed(path: Path) -> list[str]:
    """Reads and validates a changed-file list written by `changed --out`; raises VerifyError."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise VerifyError(f"cannot read the changed-file list {path}: {error}") from error
    return validate_changed(text.split("\n"))


# ---------------------------------------------------------------------------------------------
# TSan selection
# ---------------------------------------------------------------------------------------------

# The modules whose tests run under TSan in the WSL quick leg (PLAN.md 6.4 names sched, io and rpc;
# ADR 0027 adds text, whose readers run concurrently).
TSAN_MODULES = ("io", "rpc", "sched", "text")
# Budget: B28. Each module and the TSan modules that depend on it (frost:: DEPS of src/*/CMakeLists.txt
# and tests/<m>/CMakeLists.txt): src/text links frost::io, text_perf_tests links frost::sched, and src/io
# links frost::sched (its reactor and watchers, ADR 0020).
_TSAN_CLOSURE = {"io": ("io", "text"), "rpc": ("rpc",), "sched": ("io", "sched", "text"), "text": ("text",)}
# Every TSan module depends on these, so a change in them affects all three that exist.
_FOUNDATION_MODULES = {"core": ("io", "sched", "text"), "compat": ("io", "sched", "text")}
# Build inputs and shared test code: a change here can alter every module's tests.
_ALL_MODULE_FILES = frozenset({"CMakeLists.txt", "src/CMakeLists.txt", "tests/CMakeLists.txt", "CMakePresets.json",
                               "vcpkg.json", "sdk/sdk.lock.json"})
_ALL_MODULE_DIRS = (("cmake",), ("triplets",), ("tests", "support"))


def tsan_modules(paths: list[str] | None) -> list[str]:
    """
    The TSan modules a changed-file list affects, sorted (None, meaning no list: all four). Paths are
    normalised to '/' and matched on whole components, so src/textual and src/io2 count for nothing.
    Budget: B28 (the quick leg runs only when needed).
    """
    if paths is None:
        return list(TSAN_MODULES)
    out: set[str] = set()
    for raw in paths:
        path = raw.replace("\\", "/")
        while path.startswith("./"):
            path = path[2:]
        parts = tuple(p for p in path.split("/") if p)
        if not parts:
            continue
        if "/".join(parts) in _ALL_MODULE_FILES or any(
                len(parts) > len(d) and parts[: len(d)] == d for d in _ALL_MODULE_DIRS):
            out.update(TSAN_MODULES)
            continue
        if len(parts) >= 3 and parts[0] in ("src", "tests"):
            module = parts[1]
            out.update(_TSAN_CLOSURE.get(module, ()))
            out.update(_FOUNDATION_MODULES.get(module, ()))
    return sorted(out)


def tsan_label_regex(modules: Sequence[str]) -> str:
    """The ctest -L regex selecting the module labels of `modules` ("" for none)."""
    if not modules:
        return ""
    return r"^mod\.(" + "|".join(sorted(modules)) + r")$"


# ---------------------------------------------------------------------------------------------
# PATH filter
# ---------------------------------------------------------------------------------------------

_DRVFS = re.compile(r"^/mnt/[a-z](/|$)")


def strip_drvfs(value: str, keep: Sequence[str] = ()) -> tuple[str, int]:
    """
    Removes the drvfs entries (/mnt/<drive>[/...]) from a PATH value unless they lie under a `keep`
    directory; returns (new value, number dropped). /mnt/wslg and other non-drive paths stay, as do
    the order and empty entries.
    """
    prefixes = [k.rstrip("/") for k in keep if k and k.rstrip("/")]
    kept, dropped = [], 0
    for entry in value.split(":"):
        if _DRVFS.match(entry) and not any(entry == p or entry.startswith(p + "/") for p in prefixes):
            dropped += 1
            continue
        kept.append(entry)
    return ":".join(kept), dropped


# ---------------------------------------------------------------------------------------------
# clang-tidy planning
# ---------------------------------------------------------------------------------------------

SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".cxx")
HEADER_EXTENSIONS = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
# Never analysed: compile-fail probes (they do not compile by design), byte-exact fixtures, build trees.
_EXCLUDED = (re.compile(r"^tests/toolchain_probe/banned/"), re.compile(r"^tests/[^/]+/fixtures/"),
             re.compile(r"^build/"))
# A change to any of these changes how every TU is analysed, so tidy runs on all of them.
_ALL_TU_FILES = frozenset({"sdk/sdk.lock.json", "cmake/FrostCompilerOptions.cmake", "CMakePresets.json"})


@dataclass(frozen=True)
class CompileEntry:
    """One compile-database entry: `file` verbatim, its directory and output, and the raw JSON object."""

    file: str
    directory: str
    output: str | None
    path: str
    raw: dict[str, Any] = field(compare=False, hash=False, repr=False)


def _entry_config(entry: dict[str, Any]) -> str | None:
    output = entry.get("output")
    if output:
        match = re.search(r"[/\\](Debug|Release|RelWithDebInfo|MinSizeRel)[/\\]", output)
        if match:
            return match.group(1)
    command = entry.get("command") or " ".join(entry.get("arguments") or [])
    match = re.search(r'CMAKE_INTDIR=\\*"(\w+)\\*"', command)
    return match.group(1) if match else None


def load_compile_db(path: Path, config: str) -> dict[str, CompileEntry]:
    """
    Reads compile_commands.json and keeps the C/C++ sources of configuration `config` (from the
    "/<config>/" directory in "output", falling back to CMAKE_INTDIR="<config>" in the command),
    deduplicated: Ninja Multi-Config lists every TU once per configuration, and .rc entries drop out.
    Keyed by the normalised absolute source path. Raises VerifyError when the file cannot be read.
    """
    try:
        entries = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise VerifyError(f"cannot read the compile database {path}: {error}") from error
    db: dict[str, CompileEntry] = {}
    for entry in entries:
        file = str(entry.get("file", ""))
        if os.path.splitext(file)[1].lower() not in SOURCE_EXTENSIONS:
            continue
        if _entry_config(entry) != config:
            continue
        directory = str(entry.get("directory", ""))
        absolute = file if os.path.isabs(file) else os.path.join(directory, file)
        key = _norm(absolute)
        if key not in db:
            db[key] = CompileEntry(file, directory, entry.get("output"), key, entry)
    return db


_DEPS_HEADER = re.compile(r"^(.+?): #deps \d+, deps mtime \d+ \((?:VALID|STALE)\)\s*$")


def parse_ninja_deps(text: str, build_dir: str | os.PathLike[str]) -> dict[str, set[str]]:
    """
    Parses `ninja -t deps` output (the same for msvc and gcc deps) into object -> dependencies, both
    normalised absolute paths; relative paths are relative to `build_dir`. VALID and STALE entries count.
    """
    deps: dict[str, set[str]] = {}
    current: set[str] | None = None
    base = os.fspath(build_dir)
    for line in text.splitlines():
        header = _DEPS_HEADER.match(line)
        if header:
            current = deps.setdefault(_norm(os.path.join(base, header.group(1))), set())
            continue
        if current is not None and line[:1].isspace() and line.strip():
            current.add(_norm(os.path.join(base, line.strip())))
        elif not line.strip():
            current = None
    return deps


@dataclass(frozen=True)
class TidyUnit:
    """One clang-tidy invocation: the database's `file` string, the -p directory and display path."""

    file: str
    db_dir: str
    rel: str


@dataclass
class TidyPlan:
    """What tidy will analyse: units in order, header -> covering TU, skipped (path, reason), and why all."""

    units: list[TidyUnit]
    covered: dict[str, str]
    skipped: list[tuple[str, str]]
    all_reason: str | None = None
    all_files: bool = False


def _rel(repo_norm: str, path: str, original: str | None = None) -> str | None:
    """Repository-relative POSIX form of normalised `path`, or None outside the repository."""
    if path == repo_norm or not path.startswith(repo_norm.rstrip(os.sep) + os.sep):
        return None
    text = os.path.abspath(original) if original else path
    return text[len(repo_norm.rstrip(os.sep)) + 1:].replace("\\", "/")


def _excluded(rel: str) -> bool:
    return any(p.match(rel) for p in _EXCLUDED)


def _all_trigger(changed: Sequence[str]) -> str | None:
    for path in changed:
        name = path.rsplit("/", 1)[-1]
        if name == ".clang-tidy" or path in _ALL_TU_FILES or path.startswith("cmake/toolchains/"):
            return f"{path} changed"
    return None


def plan_tidy(repo: str | os.PathLike[str], db: dict[str, CompileEntry], deps: dict[str, set[str]] | None,
              changed: list[str] | None, extra_db: dict[str, CompileEntry] | None = None, *, db_dir: str = "",
              extra_db_dir: str = "", config: str = "Debug", os_name: str | None = None) -> TidyPlan:
    """
    Plans a tidy run (ADR 0027 decision 2). changed=None means every TU of `db` under the repository
    root (minus the exclusions) plus the TUs only `extra_db` has. Otherwise each changed C/C++ file
    outside the exclusions: a source in the database is a unit, one that is not is skipped; a header
    is covered by one includer from `deps` (a same-stem source, else one under the same src/<m> or
    tests/<m>, else the first by path), or skipped when nothing includes it. A changed .clang-tidy,
    sdk/sdk.lock.json, cmake/FrostCompilerOptions.cmake, cmake/toolchains/** or CMakePresets.json
    switches to every TU. O(changed + dependency edges).
    """
    repo_norm = _norm(repo)
    os_name = os_name or ("windows" if os.name == "nt" else "linux")
    extra_db = extra_db or {}
    units: dict[str, TidyUnit] = {}

    def unit_for(key: str) -> TidyUnit | None:
        if key in db:
            entry, directory = db[key], db_dir
        elif key in extra_db:
            entry, directory = extra_db[key], extra_db_dir
        else:
            return None
        # CMake writes absolute "file" values, passed on verbatim so clang-tidy finds its own entry; a
        # relative one is relative to the entry's directory, not to clang-tidy's working directory.
        file = entry.file if os.path.isabs(entry.file) else os.path.normpath(os.path.join(entry.directory, entry.file))
        rel = _rel(repo_norm, key, file) or file.replace("\\", "/")
        return TidyUnit(file, directory, rel)

    reason = _all_trigger(changed) if changed is not None else None
    if changed is None or reason:
        for key in sorted(set(db) | set(extra_db)):
            rel = _rel(repo_norm, key)
            if rel is None or _excluded(rel):
                continue
            unit = unit_for(key)
            if unit:
                units[key] = unit
        ordered = sorted(units.values(), key=lambda u: u.rel)
        return TidyPlan(ordered, {}, [], reason, True)

    covered: dict[str, str] = {}
    skipped: list[tuple[str, str]] = []
    includers: dict[str, list[str]] | None = None
    for rel in changed:
        ext = os.path.splitext(rel)[1].lower()
        if ext not in SOURCE_EXTENSIONS + HEADER_EXTENSIONS or _excluded(rel):
            continue
        key = _norm(os.path.join(os.fspath(repo), rel))
        if ext in SOURCE_EXTENSIONS:
            unit = unit_for(key)
            if unit:
                units[key] = unit
            else:
                skipped.append((rel, f"not in the {config} compile database on {os_name}"))
            continue
        if includers is None:
            includers = _includers(repo_norm, db, deps or {})
        candidates = includers.get(key, [])
        if not candidates:
            skipped.append((rel, "no TU includes it"))
            continue
        chosen = _choose_includer(rel, candidates, repo_norm)
        units[chosen] = unit_for(chosen) or TidyUnit(db[chosen].file, db_dir, chosen)
        covered[rel] = units[chosen].rel
    ordered = sorted(units.values(), key=lambda u: u.rel)
    return TidyPlan(ordered, covered, skipped)


def _includers(repo_norm: str, db: dict[str, CompileEntry], deps: dict[str, set[str]]) -> dict[str, list[str]]:
    """Header -> the database sources (under the repository, not excluded) whose objects list it."""
    by_object = {}
    for key, entry in db.items():
        if entry.output:
            by_object[_norm(os.path.join(entry.directory, entry.output))] = key
    out: dict[str, list[str]] = {}
    for obj, headers in deps.items():
        source = by_object.get(obj)
        if source is None:
            continue
        rel = _rel(repo_norm, source)
        if rel is None or _excluded(rel):
            continue
        for header in headers:
            out.setdefault(header, []).append(source)
    return out


def _choose_includer(header_rel: str, candidates: list[str], repo_norm: str) -> str:
    rels = {c: (_rel(repo_norm, c) or c) for c in set(candidates)}
    ordered = sorted(rels, key=lambda c: rels[c])
    stem = PurePosixPath(header_rel).stem
    for c in ordered:
        if PurePosixPath(rels[c]).stem == stem:
            return c
    area = header_rel.split("/")[:2]
    for c in ordered:
        if rels[c].split("/")[:2] == area:
            return c
    return ordered[0]


# ---------------------------------------------------------------------------------------------
# clang-tidy running
# ---------------------------------------------------------------------------------------------

_ERE_SPECIAL = set(".[]()*+?{}|^$\\")


def _ere_escape(text: str) -> str:
    return "".join("\\" + c if c in _ERE_SPECIAL else c for c in text)


def header_filter_regex(repo: str | os.PathLike[str], windows: bool | None = None) -> str:
    """
    clang-tidy's --header-filter (llvm::Regex, POSIX ERE) for headers under <repo>/src and
    <repo>/tests: anchored at the repository root with ERE metacharacters escaped, either separator
    accepted, and on Windows the drive letter in either case. Anchoring keeps out the generated
    headers under build/<preset>/src.
    """
    windows = (os.name == "nt") if windows is None else windows
    root = os.fspath(repo).rstrip("/\\")
    parts = re.split(r"[/\\]+", root)
    pieces = []
    for index, part in enumerate(parts):
        if index == 0 and windows and re.fullmatch(r"[A-Za-z]:", part):
            pieces.append(f"[{part[0].upper()}{part[0].lower()}]:")
        else:
            pieces.append(_ere_escape(part))
    return "^" + r"[/\\]".join(pieces) + r"[/\\](src|tests)[/\\]"


@dataclass(frozen=True)
class Finding:
    """One clang-tidy diagnostic of one check: display path (repository-relative when inside it)."""

    path: str
    line: int
    col: int
    severity: str
    check: str
    message: str


_FINDING = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*) \[([^\]]+)\]$")


def parse_findings(text: str, repo: str | os.PathLike[str]) -> list[Finding]:
    """
    The warning and error lines of clang-tidy output, one Finding per check named in the tag (the
    ",-warnings-as-errors" marker removed), deduplicated on (path, line, column, check); notes ignored.
    """
    repo_norm = _norm(repo)
    seen: set[tuple[str, int, int, str]] = set()
    out: list[Finding] = []
    for line in text.splitlines():
        match = _FINDING.match(line.rstrip("\r"))
        if not match:
            continue
        raw_path, line_no, col, severity, message, tag = match.groups()
        key_path = _norm(raw_path)
        shown = _rel(repo_norm, key_path, raw_path) or raw_path.replace("\\", "/")
        for check in tag.replace(",-warnings-as-errors", "").split(","):
            check = check.strip()
            if not check:
                continue
            key = (key_path, int(line_no), int(col), check)
            if key in seen:
                continue
            seen.add(key)
            out.append(Finding(shown, int(line_no), int(col), severity, check, message))
    return out


@dataclass
class UnitResult:
    """One TU's run: exit status (None on timeout), output, parsed findings and wall seconds."""

    unit: TidyUnit
    exit: int | None
    output: str
    findings: list[Finding]
    seconds: float

    @property
    def timed_out(self) -> bool:
        return self.exit is None

    @property
    def failed(self) -> bool:
        """A tool error: a timeout, or a non-zero exit without a parsed finding (a compile error or crash)."""
        return self.timed_out or (self.exit != 0 and not self.findings)


@dataclass
class TidyResult:
    """A whole run: unit results in plan order, deduplicated sorted findings, skipped files, wall time."""

    units: list[UnitResult]
    findings: list[Finding]
    skipped: list[tuple[str, str]]
    seconds: float
    all_files: bool = False

    @property
    def failed(self) -> list[UnitResult]:
        return [u for u in self.units if u.failed]


def run_tidy(plan: TidyPlan, command: Sequence[str], header_filter: str, jobs: int, timeout: float,
             repo: str | os.PathLike[str] = ".") -> TidyResult:
    """
    Runs `command` (clang-tidy, or a test fake) once per unit with -p, --quiet, --use-color=false and
    --header-filter, `jobs` at a time; a unit is killed after `timeout` seconds. Each worker returns
    its own output and the results are combined after all have finished, so no state is shared.
    """
    started = time.perf_counter()

    def one(unit: TidyUnit) -> UnitResult:
        cmd = [*command, "-p", unit.db_dir, "--quiet", "--use-color=false", f"--header-filter={header_filter}",
               unit.file]
        t0 = time.perf_counter()
        try:
            proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=timeout, check=False)
            code: int | None = proc.returncode
            output = proc.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as expired:
            code = None
            output = (expired.stdout or b"").decode("utf-8", "replace")
        return UnitResult(unit, code, output, parse_findings(output, repo), time.perf_counter() - t0)

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        results = list(pool.map(one, plan.units))
    merged: dict[tuple[str, int, int, str], Finding] = {}
    for result in results:
        for finding in result.findings:
            merged.setdefault((os.path.normcase(finding.path), finding.line, finding.col, finding.check), finding)
    findings = sorted(merged.values(), key=lambda f: (f.path, f.line, f.col, f.check))
    return TidyResult(results, findings, list(plan.skipped), time.perf_counter() - started, plan.all_files)


def _summary_safe(text: str) -> str:
    """`text` made safe inside a summary item's parentheses: no ", " and no parentheses of its own."""
    return " ".join(text.split()).replace(", ", "; ").replace("(", "[").replace(")", "]")


def summary_line(result: TidyResult, reason: str | None = None) -> str:
    """
    The run's summary item for the verification footer. It never contains ", " and its parentheses
    balance, because verify.ps1 splits verify.sh's summary at ", " outside parentheses (a reason is
    passed through _summary_safe).
    """
    problems = []
    if result.findings:
        files = len({f.path for f in result.findings})
        problems.append(f"{len(result.findings)} findings in {files} files")
    timeouts = sum(1 for u in result.units if u.timed_out)
    errors = sum(1 for u in result.units if u.failed and not u.timed_out)
    if timeouts:
        problems.append(f"timeout in {timeouts} TUs")
    if errors:
        problems.append(f"tool error in {errors} TUs")
    if problems:
        return f"tidy FAILED ({'; '.join(problems)})"
    n = len(result.units)
    if reason:
        return f"tidy ok ({n} TUs; all files: {_summary_safe(reason)})"
    if n == 0 and not result.skipped and not result.all_files:
        return "tidy ok (0 TUs: no C/C++ changes)"
    if result.skipped:
        return f"tidy ok ({n} TUs; {len(result.skipped)} skipped)"
    return f"tidy ok ({n} TUs)"


def _write_filtered_db(db: dict[str, CompileEntry], build_dir: Path) -> str:
    """Writes the filtered database to <build_dir>/frost-tidy/compile_commands.json; returns that directory."""
    directory = Path(build_dir) / "frost-tidy"
    _write_atomic(directory / "compile_commands.json",
                  json.dumps([db[k].raw for k in sorted(db)], indent=1) + "\n")
    return str(directory)


def _ninja_deps(ninja: str, build_dir: Path, config: str) -> dict[str, set[str]]:
    cmd = [ninja, "-C", str(build_dir)]
    if (Path(build_dir) / f"build-{config}.ninja").is_file():
        cmd += ["-f", f"build-{config}.ninja"]
    result = subprocess.run([*cmd, "-t", "deps"], capture_output=True, stdin=subprocess.DEVNULL, check=False)
    if result.returncode != 0:
        raise VerifyError(f"ninja -t deps failed in {build_dir}: {result.stderr.decode('utf-8', 'replace').strip()}")
    return parse_ninja_deps(result.stdout.decode("utf-8", "replace"), build_dir)


def tidy_main(args: argparse.Namespace) -> int:
    """The `tidy` command (see the header); returns its exit status."""
    repo = Path(os.path.abspath(args.repo))
    build_dir = Path(os.path.abspath(args.build_dir))
    if not (repo / ".clang-tidy").is_file():
        raise VerifyError(f"no .clang-tidy at the repository root {repo}")
    for name, tool in (("clang-tidy", args.clang_tidy), ("ninja", args.ninja)):
        if not tool or not os.path.isabs(tool) or not os.path.isfile(tool):
            raise VerifyError(f"{name} not found (need an absolute path): {tool!r}")
    db_file = build_dir / "compile_commands.json"
    if not db_file.is_file():
        raise VerifyError(f"no compile database {db_file} (configure with CMAKE_EXPORT_COMPILE_COMMANDS)")
    db = load_compile_db(db_file, args.config)
    db_dir = _write_filtered_db(db, build_dir)
    changed = None if args.all else read_changed(Path(args.changed))
    extra_db: dict[str, CompileEntry] = {}
    extra_dir = ""
    if args.extra_build_dir:
        extra_build = Path(os.path.abspath(args.extra_build_dir))
        extra_file = extra_build / "compile_commands.json"
        if not extra_file.is_file():
            raise VerifyError(f"no compile database {extra_file}")
        extra_all = load_compile_db(extra_file, args.extra_config)
        extra_db = {k: v for k, v in extra_all.items() if k not in db}
        extra_dir = _write_filtered_db(extra_all, extra_build)
    needs_deps = changed is not None and any(os.path.splitext(p)[1].lower() in HEADER_EXTENSIONS for p in changed)
    deps = _ninja_deps(args.ninja, build_dir, args.config) if needs_deps else {}
    plan = plan_tidy(repo, db, deps, changed, extra_db, db_dir=db_dir, extra_db_dir=extra_dir, config=args.config)
    reason = args.reason if args.all else plan.all_reason
    print(f"tidy: {len(plan.units)} TUs with -j {args.jobs}" + (f" (all files: {reason})" if reason else ""),
          flush=True)
    for header, unit in sorted(plan.covered.items()):
        print(f"tidy: {header} through {unit}")
    result = run_tidy(plan, [args.clang_tidy], header_filter_regex(repo), args.jobs, args.timeout, repo)
    for path, why in result.skipped:
        print(f"tidy: skipped {path}: {why}")
    for f in result.findings:
        print(f"{f.path}:{f.line}:{f.col}: {f.check}: {f.message}")
    for unit in result.failed:
        why = "timed out" if unit.timed_out else f"exit {unit.exit}"
        print(f"tidy: {unit.unit.rel} failed ({why}); its output:")
        print(unit.output.rstrip())
    print(f"tidy: {len(result.units)} TUs in {result.seconds:.1f} s")
    print(summary_line(result, reason), flush=True)
    return EXIT_FINDINGS if (result.findings or result.failed) else EXIT_OK


# ---------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="verify_support.py", description="verify.ps1/verify.sh helpers (ADR 0027)")
    sub = parser.add_subparsers(dest="command", required=True)
    changed = sub.add_parser("changed", help="list the files changed against the merge base with --base")
    changed.add_argument("--repo", required=True)
    changed.add_argument("--git-dir")
    changed.add_argument("--work-tree")
    changed.add_argument("--base", default="main")
    changed.add_argument("--out")
    tsan = sub.add_parser("tsan", help="the TSan quick leg's modules and label regex")
    group = tsan.add_mutually_exclusive_group(required=True)
    group.add_argument("--changed")
    group.add_argument("--all", action="store_true")
    strip = sub.add_parser("strip-path", help="drop drvfs entries from a PATH value")
    strip.add_argument("--keep", action="append", default=[])
    strip.add_argument("value")
    tidy = sub.add_parser("tidy", help="run clang-tidy on the changed TUs or on all of them")
    tidy.add_argument("--repo", required=True)
    tidy.add_argument("--build-dir", required=True)
    tidy.add_argument("--config", default="Debug")
    which = tidy.add_mutually_exclusive_group(required=True)
    which.add_argument("--changed")
    which.add_argument("--all", action="store_true")
    tidy.add_argument("--reason")
    tidy.add_argument("--extra-build-dir")
    tidy.add_argument("--extra-config", default="RelWithDebInfo")
    tidy.add_argument("--clang-tidy", required=True)
    tidy.add_argument("--ninja", required=True)
    tidy.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    tidy.add_argument("--timeout", type=float, default=600.0)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    """Runs one command (see the header) and returns its exit status."""
    args = _parser().parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "changed":
            ctx = GitContext(Path(args.repo), Path(args.git_dir) if args.git_dir else None,
                             Path(args.work_tree) if args.work_tree else None)
            text = "".join(f"{name}\n" for name in changed_files(ctx, args.base))
            if args.out:
                _write_atomic(Path(args.out), text)
            else:
                sys.stdout.buffer.write(text.encode("utf-8"))
            return EXIT_OK
        if args.command == "tsan":
            modules = tsan_modules(None if args.all else read_changed(Path(args.changed)))
            if modules:
                print(" ".join(modules))
                print(tsan_label_regex(modules))
            return EXIT_OK
        if args.command == "strip-path":
            value, dropped = strip_drvfs(args.value, args.keep)
            print(value)
            print(f"dropped {dropped}", file=sys.stderr)
            return EXIT_OK
        if args.extra_build_dir and not args.all:
            raise VerifyError("--extra-build-dir needs --all")
        if args.reason and not args.all:
            raise VerifyError("--reason needs --all")
        return tidy_main(args)
    except VerifyError as error:
        print(f"verify_support: {error}", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
