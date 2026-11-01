#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# frost_perf.py: FrostIDE's performance-budget runner (PLAN.md section 4), the start of the M1
# B-suite harness. tools/perf.ps1 is its entry point on Windows: it dot-sources env.ps1 and
# forwards its arguments here unchanged, so this file's argparse is the only CLI definition.
#
#   powershell -ExecutionPolicy Bypass -File tools\perf.ps1 [-List] [-Budget B28]
#       [-Variant rebuild,noop,verify,wsl-leg] [-Runs n] [-Warmup n] [-Files core,main,sched]
#       [-Label text] [-PerfRoot E:\FrostBuilds\perf] [-Milestone m1] [-BuildDir build\win-clangcl]
#       [-LockRecord <file>] [-Restore <run dir>]
#
# Options take the PowerShell spelling (-Budget), the GNU spelling (--budget) or any case; comma
# lists ("rebuild,noop", which `-File` passes as one string) are split here. Each harness reads
# its targets and method from tests/perf/budgets.json; only B28 has a harness so far (ADR 0010,
# ADR 0027). -List prints every budget with its gate and whether it has a harness.
#
# Results go to <PerfRoot>\<milestone>\<budget id, lower case>\<YYYYMMDDTHHMMSS>-<sha7>[-<label>]\
# (write-once): conditions.json (machine, tools, git state), runs.jsonl (one record per timed run),
# summary.json (per variant: percentiles and the verdict), logs\<tag>.log (each run's raw output),
# originals\<key>.orig and probes.json (the probe journal). PerfRoot defaults to
# $FROST_PERF_ROOT when it is non-empty, else E:\FrostBuilds\perf (PLAN.md section 4); the
# milestone defaults to the branch prefix (m1/... gives m1), else "adhoc".
#
# Side effects: a harness that edits sources (B28's probes) saves the originals and writes the
# journal before its first edit, and a pointer file <git dir>\frost-perf-active naming the run
# directory. It restores the original bytes in a finally block (also on Ctrl+C) and then removes
# the pointer. A file whose bytes match neither the original nor the harness's last write is never
# overwritten. A run killed outright (for example by a timeout wrapper) skips the finally block;
# the next run then refuses to start until `perf.ps1 -Restore <run dir>` has restored the files.
#
# Exit status: 0 every gated variant within budget (or only informational variants ran); 1 a gated
# variant over budget; 2 usage or environment error (unknown budget or variant, no harness, a probe
# file dirty or already probed, an unfinished probe journal, a dirty worktree for the verify
# variant, an existing output directory, a missing tool); 3 the run is invalid (a timed command
# failed or timed out, the run was interrupted, restoring failed). As PLAN.md section 4 says, an
# invalid run is neither green nor red.
#
# Python 3.11+, standard library only, and standalone: `python -I` puts neither this directory nor
# the current one on sys.path, so nothing here imports tools/verify_support.py.
#
# TODO(M1): m1/automation-bench adds the B1, B5, B6, B15, B19, B20 and B21 harnesses, the
# session-state precheck (PLAN 6.3) and the perf-history SQLite file (PLAN 4).

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
BUDGETS_FILE = Path("tests") / "perf" / "budgets.json"
# PLAN.md section 4, Enforcement: results live outside the repository on E:.
DEFAULT_PERF_ROOT = r"E:\FrostBuilds\perf"
SCHEMA_VERSION = 1
POINTER_NAME = "frost-perf-active"
WSL_DISTRO = "Ubuntu-24.04"

EXIT_OK = 0
EXIT_OVER = 1
EXIT_USAGE = 2
EXIT_INVALID = 3

# Seconds before a timed child is killed (the run is then invalid). Generous: a verify -Quick
# that triggers a first TSan build in a fresh WSL mirror takes several minutes.
BUILD_TIMEOUT_S = 3600
VERIFY_TIMEOUT_S = 3600
WSL_LEG_TIMEOUT_S = 3600
# ADR 0010: the distro stops by itself about 16 s after its last process exits; wait at most this
# long (seconds) for that before a wsl-leg run, polling every WSL_POLL_S seconds.
WSL_STOP_WAIT_S = 180.0
WSL_POLL_S = 2.0


class PerfError(Exception):
    """A usage or environment error: main() prints it and exits 2."""


class RunInvalid(Exception):
    """The run cannot be counted (a timed command failed, an external edit): main() exits 3."""


# ---------------------------------------------------------------------------------------------
# Budgets (tests/perf/budgets.json)
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Target:
    """
    One budgets.json target: `percentile` of `variant` must be at most `max` `unit`. variant is None
    for a single-variant budget, percentile None for a plain bound (a count, a size).
    """

    variant: str | None
    percentile: str | None
    max: float
    unit: str


@dataclass(frozen=True)
class Budget:
    """The fields of one budgets.json entry that a harness needs."""

    id: str
    metric: str
    targets: tuple[Target, ...]
    method: str
    gate: tuple[str, ...]


def load_budgets(budgets_json: Path) -> list[dict[str, Any]]:
    """Reads every entry of budgets.json; raises PerfError when the file is missing or malformed."""
    try:
        document = json.loads(Path(budgets_json).read_text(encoding="utf-8"))
        return list(document["budgets"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise PerfError(f"cannot read {budgets_json}: {error}") from error


def load_budget(budgets_json: Path, budget_id: str) -> Budget:
    """Returns budget `budget_id` (case-insensitive) from budgets.json; raises PerfError if unknown."""
    for entry in load_budgets(budgets_json):
        if str(entry.get("id", "")).lower() == budget_id.lower():
            targets = tuple(
                Target(t.get("variant"), t.get("percentile"), float(t["max"]), str(t["unit"]))
                for t in entry.get("targets", [])
                if t.get("max") is not None
            )
            gate = tuple(str(g.get("milestone")) for g in entry.get("gate", []))
            return Budget(str(entry["id"]), str(entry.get("metric", "")), targets, str(entry.get("method", "")), gate)
    raise PerfError(f"unknown budget {budget_id!r} (not in {budgets_json})")


_UNIT_SECONDS = {"ms": Fraction(1, 1000), "s": Fraction(1), "min": Fraction(60)}


def to_seconds(value: float, unit: str) -> float:
    """Converts a time `value` in budgets.json `unit` (ms, s or min) to seconds; ValueError otherwise."""
    if unit not in _UNIT_SECONDS:
        raise ValueError(f"not a time unit: {unit!r}")
    return float(Fraction(str(value)) * _UNIT_SECONDS[unit])


# ---------------------------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------------------------


def median(samples: Sequence[float]) -> float:
    """p50 as ADR 0010 defines it: statistics.median, so 10 runs give the mean of the 5th and 6th."""
    if not samples:
        raise ValueError("median of no samples")
    return float(statistics.median(samples))


def percentile(samples: Sequence[float], q: float) -> float:
    """
    The nearest-rank percentile: the sample of rank ceil(q/100 * n), clamped to [1, n], in sorted
    order. Defined for 0 < q <= 100; ValueError for any other q or for no samples. The rank is
    computed exactly (Fraction), so q = 70 over 10 samples is rank 7, not 8 from 7.000000000000001.
    """
    if not samples:
        raise ValueError("percentile of no samples")
    if not 0 < q <= 100:
        raise ValueError(f"percentile q must be in (0, 100]: {q}")
    n = len(samples)
    rank = math.ceil(Fraction(str(q)) * n / 100)
    rank = min(max(rank, 1), n)
    return float(sorted(samples)[rank - 1])


def percentile_value(samples: Sequence[float], name: str) -> float:
    """The value of percentile `name` ("p50", "p95", "max", ...): p50 is the median, others nearest rank."""
    if name == "p50":
        return median(samples)
    if name == "max":
        return float(max(samples))
    match = re.fullmatch(r"p(\d+(?:\.\d+)?)", name)
    if not match:
        raise ValueError(f"unknown percentile {name!r}")
    return percentile(samples, float(match.group(1)))


# ---------------------------------------------------------------------------------------------
# Probe edits (ported from E:\FrostBuilds\perf\m0\b28\scripts\b28_bench.py, the project's own
# Apache-2.0 harness of ADR 0010)
# ---------------------------------------------------------------------------------------------

PROBE_RE = re.compile(rb"^\[\[maybe_unused\]\] constexpr int kB28Probe = -?\d+;(\r?\n)\r?\n", re.M)
_NAMESPACE_RE = re.compile(rb"^namespace\b[^\n]*?(\r?\n|$)", re.M)


def has_probe(data: bytes) -> bool:
    """True if `data` already carries a B28 probe line."""
    return PROBE_RE.search(data) is not None


def insert_probe(data: bytes, n: int) -> bytes:
    """
    Returns `data` with `[[maybe_unused]] constexpr int kB28Probe = N;` and an empty line before
    its first line that starts with `namespace`, or with an existing probe's value replaced. The
    probe uses the line ending of that namespace line (CRLF stays CRLF); every other byte is
    unchanged. Raises ValueError when the file has no such line.
    """
    existing = PROBE_RE.search(data)
    if existing:
        eol = existing.group(1)
        line = b"[[maybe_unused]] constexpr int kB28Probe = %d;" % n + eol + eol
        return data[: existing.start()] + line + data[existing.end():]
    match = _NAMESPACE_RE.search(data)
    if not match:
        raise ValueError("no line starts with 'namespace'")
    eol = match.group(1) or b"\n"
    line = b"[[maybe_unused]] constexpr int kB28Probe = %d;" % n + eol + eol
    return data[: match.start()] + line + data[match.start():]


def remove_probe(data: bytes) -> bytes:
    """Returns `data` without its probe line and the empty line after it (the inverse of insert_probe)."""
    return PROBE_RE.sub(b"", data, count=1)


class ProbeValues:
    """Probe values N that never repeat: max(previous + 1, milliseconds since the epoch mod 2e9)."""

    def __init__(self, wall: Callable[[], float] = time.time) -> None:
        self._wall = wall
        self._last = 0

    def next(self) -> int:
        """The next value; strictly greater than every earlier one (fits in an int literal)."""
        n = int(self._wall() * 1000) % 2_000_000_000
        self._last = max(self._last + 1, n)
        return self._last


def sha256(data: bytes) -> str:
    """Hex SHA-256 of `data`."""
    return hashlib.sha256(data).hexdigest()


def write_atomic(path: Path, data: bytes) -> None:
    """Writes `data` to `path` through a temporary file in the same directory and os.replace."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
        if path.exists():
            shutil.copymode(path, tmp)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def write_json_atomic(path: Path, document: Any) -> None:
    """Writes `document` as indented UTF-8 JSON with LF endings, atomically."""
    write_atomic(path, (json.dumps(document, indent=1, ensure_ascii=False) + "\n").encode("utf-8"))


class ProbeJournal:
    """
    The record that makes probe edits recoverable: <run dir>\\probes.json lists each probed file
    with the SHA-256 of its original bytes, of the harness's last write and of the write before it,
    and originals\\<key>.orig holds the original bytes. The journal is rewritten atomically before
    every probe write, so after a kill the file holds the original, the last recorded write, or (a
    kill between the journal and the file write) the write before it. Not thread-safe; one harness
    thread writes probes.
    """

    def __init__(self, run_dir: Path, repo: Path, git_dir: Path, files: dict[str, Path]) -> None:
        self.run_dir = Path(run_dir)
        self.path = self.run_dir / "probes.json"
        self.repo = Path(repo)
        self.git_dir = Path(git_dir)
        self.files = dict(files)
        self.entries: dict[str, dict[str, Any]] = {}

    def begin(self) -> None:
        """Saves the originals, writes the journal and then the pointer file in the git directory."""
        originals = self.run_dir / "originals"
        for key, path in self.files.items():
            data = path.read_bytes()
            write_atomic(originals / f"{key}.orig", data)
            digest = sha256(data)
            self.entries[key] = {"key": key, "path": str(path), "original": str(originals / f"{key}.orig"),
                                 "original_sha256": digest, "last_written_sha256": digest,
                                 "previous_written_sha256": digest}
        self._save()
        write_atomic(self.git_dir / POINTER_NAME, (str(self.run_dir) + "\n").encode("utf-8"))

    def _save(self) -> None:
        write_json_atomic(self.path, {"schemaVersion": SCHEMA_VERSION, "repo": str(self.repo),
                                      "git_dir": str(self.git_dir), "files": list(self.entries.values())})

    def write_probe(self, key: str, n: int) -> None:
        """
        Inserts probe `n` into file `key`. Raises RunInvalid, without writing, when the file's
        bytes match neither its original nor the harness's last write (an external edit).
        """
        entry = self.entries[key]
        path = Path(entry["path"])
        current = path.read_bytes()
        if sha256(current) not in (entry["original_sha256"], entry["last_written_sha256"]):
            raise RunInvalid(f"{path} changed outside the harness; not overwritten (original: {entry['original']})")
        data = insert_probe(current, n)
        entry["previous_written_sha256"] = sha256(current)
        entry["last_written_sha256"] = sha256(data)
        self._save()
        write_atomic(path, data)

    def restore_original(self, key: str) -> bool:
        """
        Puts file `key` back to its original bytes mid-run, through the journal like a probe write;
        returns whether it wrote (False when the file is already original). Raises RunInvalid,
        without writing, for an external edit (bytes matching no journal state).
        """
        entry = self.entries[key]
        path = Path(entry["path"])
        digest = sha256(path.read_bytes())
        if digest == entry["original_sha256"]:
            return False
        if digest not in (entry["last_written_sha256"], entry["previous_written_sha256"]):
            raise RunInvalid(f"{path} changed outside the harness; not overwritten (original: {entry['original']})")
        original = Path(entry["original"]).read_bytes()
        if sha256(original) != entry["original_sha256"]:
            raise RunInvalid(f"{entry['original']} does not match the journal; {path} not restored")
        entry["previous_written_sha256"] = digest
        entry["last_written_sha256"] = entry["original_sha256"]
        self._save()
        write_atomic(path, original)
        return True


def restore_journal(journal_path: Path) -> list[str]:
    """
    Restores every file of a probe journal to its original bytes and returns the problems (empty
    when all are restored). A file already at its original bytes is left alone; one at the last
    recorded write or the write before it is rewritten from originals\\<key>.orig; any other file is
    not touched and is reported with the path of its original. Removes the pointer file when nothing
    is left over.
    """
    try:
        journal = json.loads(Path(journal_path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PerfError(f"cannot read the probe journal {journal_path}: {error}") from error
    problems: list[str] = []
    for entry in journal.get("files", []):
        path = Path(entry["path"])
        try:
            current = path.read_bytes()
        except OSError as error:
            problems.append(f"{path}: cannot read ({error}); its original is {entry['original']}")
            continue
        digest = sha256(current)
        if digest == entry["original_sha256"]:
            continue
        # Journals written before previous_written_sha256 existed name only the last write.
        if digest not in (entry["last_written_sha256"], entry.get("previous_written_sha256")):
            problems.append(f"{path} changed outside the harness; not overwritten. Its original is {entry['original']}")
            continue
        original = Path(entry["original"]).read_bytes()
        if sha256(original) != entry["original_sha256"]:
            problems.append(f"{entry['original']} does not match the journal; {path} not restored")
            continue
        write_atomic(path, original)
    if not problems:
        pointer = Path(journal.get("git_dir", "")) / POINTER_NAME
        if pointer.is_file():
            named = pointer.read_text(encoding="utf-8").strip()
            if os.path.normcase(os.path.abspath(named)) == os.path.normcase(os.path.abspath(Path(journal_path).parent)):
                pointer.unlink()
    return problems


# ---------------------------------------------------------------------------------------------
# Timing helpers
# ---------------------------------------------------------------------------------------------

_NINJA_HEADER = re.compile(r"^# ninja log v(\d+)\s*$")
_EDGE_LINE = re.compile(r"^\[(\d+)/(\d+)\] ")


def parse_ninja_log(path: Path, offset: int = 0) -> tuple[list[dict[str, Any]], int]:
    """
    Reads the .ninja_log lines after byte `offset` and returns (edges, end offset). An edge is
    {"start_ms", "end_ms", "output"}; formats v5, v6 and v7 share these columns (start, end, mtime,
    output, hash). Malformed lines are skipped. A file shorter than `offset` was recompacted by
    ninja and is read from its start. ValueError for a header naming another version.
    """
    path = Path(path)
    try:
        with path.open("rb") as handle:
            size = handle.seek(0, os.SEEK_END)
            start = offset if 0 <= offset <= size else 0
            handle.seek(start)
            text = handle.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return [], 0
    edges = []
    for line in text.splitlines():
        header = _NINJA_HEADER.match(line)
        if header:
            if int(header.group(1)) not in (5, 6, 7):
                raise ValueError(f"{path}: unsupported ninja log version {header.group(1)}")
            continue
        parts = line.split("\t")
        if len(parts) < 4 or not parts[0].isdigit() or not parts[1].isdigit() or not parts[3]:
            continue
        edges.append({"start_ms": int(parts[0]), "end_ms": int(parts[1]), "output": parts[3]})
    return edges, start + len(text.encode("utf-8", "replace"))


def ninja_log_size(path: Path) -> int:
    """The current size of a .ninja_log in bytes (0 when it does not exist)."""
    try:
        return Path(path).stat().st_size
    except FileNotFoundError:
        return 0


def phase_split(markers: Sequence[tuple[float, str]], wall: float) -> dict[str, float]:
    """
    Turns timestamped markers [(seconds since start, name), ...] into phase durations: "startup"
    up to the first marker, then each marker's name up to the next marker (the last up to `wall`).
    A repeated name gets " #2", " #3". The durations add up to `wall`.
    """
    phases: dict[str, float] = {}
    points = sorted(markers, key=lambda m: m[0])
    if not points:
        return {"startup": round(wall, 3)}
    phases["startup"] = round(points[0][0], 3)
    for index, (start, name) in enumerate(points):
        end = points[index + 1][0] if index + 1 < len(points) else wall
        key, k = name, 2
        while key in phases:
            key, k = f"{name} #{k}", k + 1
        phases[key] = round(end - start, 3)
    return phases


def to_wsl_path(path: str) -> str:
    """
    The WSL (drvfs) form of a Windows drive path: "E:\\dev\\x y" becomes "/mnt/e/dev/x y". A copy of
    verify.ps1's ConvertTo-WslPath, tested against the same table (tests/tools/fixtures/verify).
    """
    if not re.match(r"^[A-Za-z]:[\\/]", path):
        raise ValueError(f"not a drive path: {path!r}")
    return "/mnt/" + path[0].lower() + path[2:].replace("\\", "/")


def _wsl_form(path: Path | str) -> str:
    """to_wsl_path for a Windows drive path; any other path (the tests' Linux runs) unchanged."""
    text = str(path)
    return to_wsl_path(text) if re.match(r"^[A-Za-z]:[\\/]", text) else text


@dataclass
class Streamed:
    """The result of a streamed child: exit status (None when killed on timeout), lines, wall."""

    exit: int | None
    lines: list[tuple[float, str]]
    wall: float
    timed_out: bool = False


def run_streamed(cmd: list[str], *, log: Path, clock: Callable[[], float], timeout: float,
                 cwd: Path | None = None, env: dict[str, str] | None = None, header: str = "") -> Streamed:
    """
    Runs `cmd` (absolute executable, no shell) with stdout and stderr merged, timestamping each
    line as it arrives with `clock`, and writes "<seconds> <line>" to `log`. Kills the child after
    `timeout` seconds (its own children may outlive it). Lines are decoded as UTF-8 with errors
    replaced and a leading BOM dropped.
    """
    log.parent.mkdir(parents=True, exist_ok=True)
    lines: list[tuple[float, str]] = []
    with log.open("w", encoding="utf-8", newline="\n") as out:
        out.write(f"# {_iso_now()} {subprocess.list2cmdline(cmd)}\n")
        if header:
            out.write(f"# {header}\n")
        t0 = clock()
        proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        killed = threading.Event()

        def kill() -> None:
            killed.set()
            proc.kill()

        timer = threading.Timer(timeout, kill)
        timer.daemon = True
        timer.start()
        try:
            assert proc.stdout is not None
            for raw in proc.stdout:
                stamp = clock() - t0
                text = raw.decode("utf-8", "replace").rstrip("\r\n").lstrip("\ufeff")
                lines.append((stamp, text))
                out.write(f"{stamp:9.3f} {text}\n")
            code = proc.wait()
        finally:
            timer.cancel()
        wall = clock() - t0
        out.write(f"# exit {code}, wall {wall:.3f} s{' (killed on timeout)' if killed.is_set() else ''}\n")
    return Streamed(None if killed.is_set() else code, lines, wall, killed.is_set())


def _iso_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S")


# ---------------------------------------------------------------------------------------------
# Tools, hooks and machine conditions
# ---------------------------------------------------------------------------------------------


@dataclass
class Tools:
    """
    Command prefixes (absolute executable first) of the tools a harness runs; None when absent.
    resolve_tools() finds the real ones; tests inject Python fakes such as [sys.executable, fake.py].
    verify_base: whether verify.ps1 declares -Base (None: read its param block with PowerShell).
    """

    cmake: list[str] | None
    ninja: list[str] | None
    sccache: list[str] | None
    powershell: list[str] | None
    wsl: list[str] | None
    verify_command: list[str] | None
    clang_cl: list[str] | None = None
    verify_base: bool | None = None


def run_build_command(cmd: list[str], timeout: float) -> tuple[int | None, bytes]:
    """
    Runs one build command (absolute executable, no shell) with stdout and stderr merged and stdin
    closed; returns (exit status, output), or (None, partial output) after killing it on `timeout`
    seconds. Hooks.run_build's default.
    """
    try:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, check=False)
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as expired:
        return None, (expired.stdout or b"") + b"\n(killed on timeout)"


@dataclass
class Hooks:
    """
    Test seams. clock: the timer around timed commands (called once before and once after each,
    and per streamed line). timestamp: the run directory's YYYYMMDDTHHMMSS. after_probe_write:
    called with (key, path, n) after each probe write. cpu_sample_s: GetSystemTimes interval.
    distro_wait_s and sleep: the wsl-leg stop wait. run_build: runs the settle and timed
    `cmake --build` commands (run_build_command); the self-tests' long gated series replace it with
    an in-process fake, because hundreds of fake-tool processes made that suite the longest
    Windows test.
    """

    clock: Callable[[], float] = time.perf_counter
    timestamp: Callable[[], str] = field(default=lambda: time.strftime("%Y%m%dT%H%M%S"))
    after_probe_write: Callable[[str, Path, int], None] | None = None
    cpu_sample_s: float = 3.0
    distro_wait_s: float = WSL_STOP_WAIT_S
    sleep: Callable[[float], None] = time.sleep
    run_build: Callable[[list[str], float], tuple[int | None, bytes]] = run_build_command


def _system32(*parts: str) -> str | None:
    root = os.environ.get("SystemRoot", r"C:\Windows")
    path = os.path.join(root, "System32", *parts)
    return path if os.path.isfile(path) else None


def _which(name: str) -> list[str] | None:
    found = shutil.which(name)
    return [os.path.abspath(found)] if found else None


def resolve_tools(repo: Path) -> Tools:
    """Finds the real tools: cmake, ninja, sccache, clang-cl on PATH (env.ps1's SDK), the rest in System32."""
    powershell = _system32("WindowsPowerShell", "v1.0", "powershell.exe")
    wsl = _system32("wsl.exe")
    verify = None
    if powershell:
        verify = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                  str(Path(repo) / "tools" / "verify.ps1")]
    return Tools(cmake=_which("cmake"), ninja=_which("ninja"), sccache=_which("sccache"),
                 powershell=[powershell] if powershell else None, wsl=[wsl] if wsl else None,
                 verify_command=verify, clang_cl=_which("clang-cl"))


def verify_declares_base(powershell: list[str], script: Path) -> bool:
    """True when verify.ps1's param block declares -Base (read with the PowerShell parser, not run)."""
    command = (
        "$t = $null; $e = $null; "
        f"$a = [System.Management.Automation.Language.Parser]::ParseFile('{str(script).replace(chr(39), chr(39) * 2)}', "
        "[ref]$t, [ref]$e); if ($a.ParamBlock) { $a.ParamBlock.Parameters | "
        "ForEach-Object { $_.Name.VariablePath.UserPath } }")
    result = subprocess.run([*powershell, "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
                            check=False)
    if result.returncode != 0:
        raise PerfError(f"cannot read verify.ps1's param block: {result.stderr.strip()}")
    return any(name.strip().lower() == "base" for name in result.stdout.splitlines())


def _first_line(cmd: list[str] | None, *args: str) -> str | None:
    if not cmd:
        return None
    try:
        out = subprocess.run([*cmd, *args], capture_output=True, text=True, encoding="utf-8", errors="replace",
                             timeout=60, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    return lines[0] if lines else None


def _memory() -> dict[str, float] | None:
    if sys.platform != "win32":
        return None

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

    status = MEMORYSTATUSEX()
    status.dwLength = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    gb = 1024 ** 3
    return {"total_phys_gb": round(status.ullTotalPhys / gb, 1), "free_phys_gb": round(status.ullAvailPhys / gb, 1),
            "commit_limit_gb": round(status.ullTotalPageFile / gb, 1),
            "free_commit_gb": round(status.ullAvailPageFile / gb, 1)}


def _system_cpu_percent(seconds: float, sleep: Callable[[float], None]) -> float | None:
    """System-wide CPU use over `seconds` from GetSystemTimes (kernel time includes idle time)."""
    if sys.platform != "win32":
        return None

    class FILETIME(ctypes.Structure):
        _fields_ = [("low", ctypes.c_ulong), ("high", ctypes.c_ulong)]

    def sample() -> tuple[int, int, int] | None:
        idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
        if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
            return None
        value = lambda f: (f.high << 32) | f.low  # noqa: E731
        return value(idle), value(kernel), value(user)

    first = sample()
    sleep(seconds)
    second = sample()
    if not first or not second:
        return None
    total = (second[1] - first[1]) + (second[2] - first[2])
    return round(100.0 * (total - (second[0] - first[0])) / total, 1) if total else 0.0


def _cpu_name() -> str | None:
    if sys.platform == "win32":
        try:
            import winreg  # Windows only; the module calls the registry's W functions.

            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
                return str(winreg.QueryValueEx(key, "ProcessorNameString")[0]).strip()
        except OSError:
            return None
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def _power_scheme() -> str | None:
    powercfg = _system32("powercfg.exe")
    if not powercfg:
        return None
    try:
        out = subprocess.run([powercfg, "/getactivescheme"], capture_output=True, timeout=30, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.decode("mbcs" if sys.platform == "win32" else "utf-8", "replace").strip() or None


def wsl_running(wsl: list[str] | None) -> bool | None:
    """Whether the WSL distro runs (`wsl --list --running --quiet`, which never boots it); None if unknown."""
    if not wsl:
        return None
    env = dict(os.environ, WSL_UTF8="1")
    try:
        result = subprocess.run([*wsl, "--list", "--running", "--quiet"], capture_output=True, timeout=30, env=env,
                                check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = result.stdout.decode("utf-8", "replace").replace("\x00", "")
    return any(line.strip().split(" ")[0] == WSL_DISTRO for line in text.splitlines())


def _lock_record(path: str | None) -> Any:
    if not path:
        return None
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    for line in reversed(text.splitlines()):
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            return value
    return {"raw": text}


# ---------------------------------------------------------------------------------------------
# Harness protocol
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Variant:
    """
    One series of a harness. target_variant is the exact budgets.json targets[].variant string, or
    None for an informational series. A gated verdict needs at least min_counted counted runs and
    the default probe files.
    """

    name: str
    target_variant: str | None
    warmup: int
    runs: int
    min_counted: int
    files: tuple[str, ...] = ()


@dataclass
class RunContext:
    """What a harness's run_variant gets: paths, tools, hooks, the probe journal and the options."""

    repo: Path
    build_dir: Path
    run_dir: Path
    tools: Tools
    hooks: Hooks
    journal: ProbeJournal | None
    files: list[str] | None
    runs: int | None
    warmup: int | None
    budget: Budget
    verify_base: bool = False
    probe_values: ProbeValues = field(default_factory=ProbeValues)

    def log(self, tag: str) -> Path:
        """The raw-output log for run `tag`."""
        return self.run_dir / "logs" / f"{tag}.log"

    def precheck(self) -> None:
        """
        PLAN.md section 6.3's session-state precheck for on-screen harnesses; a no-op until
        m1/automation-bench supplies it. Raises RunInvalid when the session cannot give valid samples.
        """
        return None

    def write_probe(self, key: str) -> int:
        """Writes a fresh probe into probe file `key` through the journal; returns its value."""
        if self.journal is None:
            raise RunInvalid("no probe journal")
        n = self.probe_values.next()
        self.journal.write_probe(key, n)
        if self.hooks.after_probe_write:
            self.hooks.after_probe_write(key, self.journal.files[key], n)
        return n

    def restore_other_probes(self, key: str) -> list[str]:
        """
        Puts every probed file except `key` back to its original bytes (ProbeJournal.restore_original),
        so the working tree differs from HEAD in at most `key`'s file; returns the keys it rewrote.
        """
        if self.journal is None:
            raise RunInvalid("no probe journal")
        return [other for other in sorted(self.journal.files) if other != key and self.journal.restore_original(other)]


def record(ctx: RunContext, variant: Variant, **fields: Any) -> dict[str, Any]:
    """A runs.jsonl record: the generic core fields, then the harness's own (for B28 a "b28" object)."""
    base: dict[str, Any] = {"schemaVersion": SCHEMA_VERSION, "budget": ctx.budget.id, "variant": variant.name,
                            "target_variant": variant.target_variant, "file": None, "round": None, "run": None,
                            "counted": True, "start": _iso_now(), "value": None, "unit": "s", "wall_s": None,
                            "exit": None, "valid": True, "phases": None, "summary_line": None}
    base.update(fields)
    return base


# ---------------------------------------------------------------------------------------------
# B28: the dev loop (ADR 0010; budgets.json B28's method; ADR 0011 F3)
# ---------------------------------------------------------------------------------------------

# Budget: B28. The probe files budgets.json's method fixes (ADR 0010 decision 3): core is linked by
# frost and 14 other executables, main is frost's own TU, sched is linked only by the sched tests.
B28_PROBE_FILES = {"core": "src/core/src/error.cpp", "main": "src/app/src/main.cpp", "sched": "src/sched/src/lane.cpp"}
# Budget: B28. verify -Quick runs with -Jobs 4 (budgets.json); the timed rebuild with ninja's default
# parallelism, so it passes no -j (ADR 0010 decision 2, accepted in ADR 0011 F3).
B28_VERIFY_JOBS = 4
B28_DEFAULT_BUILD_DIR = Path("build") / "win-clangcl"
_REBUILD_ROUND = (("core", "frost"), ("core", "all"), ("main", "frost"), ("main", "all"), ("sched", "all"))


class B28Harness:
    """
    B28, "one-.cpp change to rebuilt and linked / verify -Quick". Variants:
      rebuild  1 warm-up round + 10 counted; a round is core/frost, core/all, main/frost, main/all and
               sched/all, each after an untimed full build (the settle). "all" (the full default build,
               cmake --build <tree> --config Debug) is gated: the worst probe's p50 at most 20 s;
               "frost" (--target frost) is reported.
      noop     5 runs of each command with no change (informational).
      verify   1 warm-up + 6 runs of tools\\verify.ps1 -Quick -Jobs 4 alternating core and main, each
               after restoring the other probe file, a settle and a probe; gated: p50 at most 5 min. With
               -Base HEAD when verify.ps1 declares it, so tidy and TSan see only the probe change, as on
               main (ADR 0027 decision 7).
      wsl-leg  3 runs of verify.sh --no-checks after the distro has stopped by itself (informational).
    Builds and tests only, nothing on screen, so no session precheck (ADR 0010, Conditions).
    """

    id = "B28"
    needs_session_precheck = False
    on_screen = False
    variants = (
        Variant("rebuild", "one-.cpp rebuild", warmup=1, runs=10, min_counted=10, files=("core", "main", "sched")),
        Variant("noop", None, warmup=0, runs=5, min_counted=0),
        Variant("verify", "verify -Quick", warmup=1, runs=6, min_counted=6, files=("core", "main")),
        Variant("wsl-leg", None, warmup=0, runs=3, min_counted=0, files=("core",)),
    )

    @staticmethod
    def probe_files() -> dict[str, str]:
        """Probe key -> repository-relative path."""
        return dict(B28_PROBE_FILES)

    @staticmethod
    def required_tools(variant: Variant) -> tuple[str, ...]:
        """The Tools fields `variant` cannot run without."""
        return {"rebuild": ("cmake",), "noop": ("cmake",), "verify": ("cmake", "verify_command"),
                "wsl-leg": ("wsl",)}[variant.name]

    @staticmethod
    def commands(ctx: RunContext) -> dict[str, list[str] | None]:
        """The exact command lines, for conditions.json."""
        build = [*ctx.tools.cmake, "--build", str(ctx.build_dir), "--config", "Debug"] if ctx.tools.cmake else None
        return {"settle": build, "rebuild_all": build, "rebuild_frost": build and [*build, "--target", "frost"],
                "verify": B28Harness._verify_command(ctx),
                "wsl_leg": B28Harness._wsl_command(ctx, "<changed-list>") if ctx.tools.wsl else None}

    @staticmethod
    def _verify_command(ctx: RunContext) -> list[str] | None:
        if not ctx.tools.verify_command:
            return None
        cmd = [*ctx.tools.verify_command, "-Quick", "-Jobs", str(B28_VERIFY_JOBS)]
        if ctx.build_dir != (ctx.repo / B28_DEFAULT_BUILD_DIR):
            cmd += ["-BuildDir", str(ctx.build_dir)]
        if ctx.verify_base:
            cmd += ["-Base", "HEAD"]
        return cmd

    @staticmethod
    def _wsl_command(ctx: RunContext, changed: str) -> list[str]:
        assert ctx.tools.wsl is not None
        script = _wsl_form(ctx.repo) + "/tools/verify.sh"
        return [*ctx.tools.wsl, "-d", WSL_DISTRO, "--exec", "env", f"FROST_JOBS={B28_VERIFY_JOBS}",
                "FROST_VERIFY_BASE=HEAD", f"FROST_CHANGED_FILES={changed}", "bash", script, "--no-checks"]

    # -- running --------------------------------------------------------------------------------

    def _build(self, ctx: RunContext, target: str | None, tag: str) -> dict[str, Any]:
        assert ctx.tools.cmake is not None
        cmd = [*ctx.tools.cmake, "--build", str(ctx.build_dir), "--config", "Debug"]
        if target:
            cmd += ["--target", target]
        ninja_log = ctx.build_dir / ".ninja_log"
        offset = ninja_log_size(ninja_log)
        log = ctx.log(tag)
        log.parent.mkdir(parents=True, exist_ok=True)
        t0 = ctx.hooks.clock()
        code, raw = ctx.hooks.run_build(cmd, BUILD_TIMEOUT_S)
        wall = ctx.hooks.clock() - t0
        out = raw.decode("utf-8", "replace")
        with log.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(f"# {_iso_now()} {subprocess.list2cmdline(cmd)}\n# exit {code}, wall {wall:.3f} s\n")
            handle.write(out.replace("\r\n", "\n"))
        edges = sum(1 for line in out.splitlines() if _EDGE_LINE.match(line))
        try:
            log_edges, _ = parse_ninja_log(ninja_log, offset)
        except ValueError:
            log_edges = []
        return {"wall_s": round(wall, 3), "exit": code, "edges": edges, "ninja_log_edges": log_edges,
                "no_work": "ninja: no work to do" in out}

    def _sccache(self, ctx: RunContext) -> dict[str, int] | None:
        if not ctx.tools.sccache:
            return None
        try:
            out = subprocess.run([*ctx.tools.sccache, "--show-stats", "--stats-format", "json"], capture_output=True,
                                 timeout=60, check=False).stdout
            stats = json.loads(out.decode("utf-8", "replace"))["stats"]
        except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired):
            return None

        def count(value: Any) -> int:
            return sum((value or {}).get("counts", {}).values()) if isinstance(value, dict) else 0

        return {"requests": stats.get("compile_requests", 0), "executed": stats.get("requests_executed", 0),
                "hits": count(stats.get("cache_hits")), "misses": count(stats.get("cache_misses")),
                "errors": count(stats.get("cache_errors")), "fails": stats.get("compile_fails", 0)}

    @staticmethod
    def _delta(before: dict[str, int] | None, after: dict[str, int] | None) -> dict[str, int] | None:
        if before is None or after is None:
            return None
        return {key: after[key] - before[key] for key in before}

    def _settle(self, ctx: RunContext, tag: str) -> dict[str, Any]:
        settle = self._build(ctx, None, f"{tag}-settle")
        if settle["exit"] != 0:
            raise RunInvalid(f"the settle build before {tag} failed (exit {settle['exit']})")
        return {"wall_s": settle["wall_s"], "exit": settle["exit"], "edges": settle["edges"]}

    def run_variant(self, ctx: RunContext, variant: Variant) -> Iterator[dict[str, Any]]:
        """Runs one variant; yields one runs.jsonl record per timed run; raises RunInvalid on a failure."""
        runner = {"rebuild": self._rebuild, "noop": self._noop, "verify": self._verify, "wsl-leg": self._wsl_leg}
        yield from runner[variant.name](ctx, variant)

    def _counts(self, ctx: RunContext, variant: Variant) -> tuple[int, int]:
        warmup = variant.warmup if ctx.warmup is None else ctx.warmup
        runs = variant.runs if ctx.runs is None else ctx.runs
        return warmup, runs

    def _files(self, ctx: RunContext, variant: Variant) -> list[str]:
        return list(ctx.files) if ctx.files else list(variant.files)

    def _rebuild(self, ctx: RunContext, variant: Variant) -> Iterator[dict[str, Any]]:
        warmup, runs = self._counts(ctx, variant)
        files = self._files(ctx, variant)
        plan = [(key, target) for key, target in _REBUILD_ROUND if key in files]
        for rnd in range(warmup + runs):
            for key, target in plan:
                tag = f"r{rnd:02d}-{key}-{target}"
                settle = self._settle(ctx, tag)
                before = self._sccache(ctx)
                n = ctx.write_probe(key)
                start = _iso_now()
                result = self._build(ctx, "frost" if target == "frost" else None, tag)
                after = self._sccache(ctx)
                ok = result["exit"] == 0
                yield record(ctx, variant, file=key, round=rnd, counted=rnd >= warmup, start=start,
                             value=result["wall_s"],
                             wall_s=result["wall_s"], exit=result["exit"], valid=ok,
                             b28={"probe": n, "target": target, "edges": result["edges"],
                                  "ninja_log_edges": result["ninja_log_edges"],
                                  "sccache": self._delta(before, after), "settle": settle})
                if not ok:
                    raise RunInvalid(f"the timed build {tag} failed (exit {result['exit']})")

    def _noop(self, ctx: RunContext, variant: Variant) -> Iterator[dict[str, Any]]:
        warmup, runs = self._counts(ctx, variant)
        settle = self._settle(ctx, "n")
        for run in range(warmup + runs):
            for target in ("frost", "all"):
                tag = f"n{run:02d}-{target}"
                start = _iso_now()
                result = self._build(ctx, "frost" if target == "frost" else None, tag)
                ok = result["exit"] == 0
                yield record(ctx, variant, run=run, counted=run >= warmup, start=start, value=result["wall_s"],
                             wall_s=result["wall_s"], exit=result["exit"], valid=ok,
                             b28={"probe": None, "target": target, "edges": result["edges"],
                                  "ninja_log_edges": result["ninja_log_edges"], "sccache": None, "settle": settle})
                if not ok:
                    raise RunInvalid(f"the no-op build {tag} failed (exit {result['exit']})")

    @staticmethod
    def _marker_name(text: str) -> str:
        name = text[4:].strip()
        return "wsl" if name.startswith("wsl ") else name

    def _verify(self, ctx: RunContext, variant: Variant) -> Iterator[dict[str, Any]]:
        warmup, runs = self._counts(ctx, variant)
        files = self._files(ctx, variant)
        cmd = self._verify_command(ctx)
        assert cmd is not None
        for run in range(warmup + runs):
            key = files[run % len(files)]
            tag = f"v{run:02d}-{key}"
            # The previous run's probe goes before the settle, so the timed run's changed files (and
            # with them verify's tidy and TSan work) are the one probe file, as for a one-file edit on
            # main (decision 7 of ADR 0027).
            restored = ctx.restore_other_probes(key)
            settle = self._settle(ctx, tag)
            before = self._sccache(ctx)
            n = ctx.write_probe(key)
            start = _iso_now()
            streamed = run_streamed(cmd, log=ctx.log(tag), clock=ctx.hooks.clock, timeout=VERIFY_TIMEOUT_S,
                                    cwd=ctx.repo, header=f"probe {key} = {n}; elapsed seconds prefix each line")
            after = self._sccache(ctx)
            markers = [(t, self._marker_name(text)) for t, text in streamed.lines if text.startswith("=== ")]
            verified = [text for _, text in streamed.lines if text.startswith(("Verified:", "FAILED: Verified:"))]
            ok = streamed.exit == 0
            yield record(ctx, variant, file=key, run=run, counted=run >= warmup, start=start,
                         value=round(streamed.wall, 3),
                         wall_s=round(streamed.wall, 3), exit=streamed.exit, valid=ok,
                         phases=phase_split(markers, streamed.wall), summary_line=verified[-1] if verified else None,
                         b28={"probe": n, "markers": [[round(t, 3), name] for t, name in markers],
                              "sccache": self._delta(before, after), "settle": settle, "restored": restored,
                              "command": cmd})
            if not ok:
                why = "timed out" if streamed.timed_out else f"exit {streamed.exit}"
                raise RunInvalid(f"verify run {tag} failed ({why})")

    def _wsl_leg(self, ctx: RunContext, variant: Variant) -> Iterator[dict[str, Any]]:
        warmup, runs = self._counts(ctx, variant)
        files = self._files(ctx, variant)
        assert ctx.tools.wsl is not None
        for run in range(warmup + runs):
            key = files[run % len(files)]
            tag = f"w{run:02d}-{key}"
            waited_from = ctx.hooks.clock()
            still_running = wsl_running(ctx.tools.wsl)
            while still_running and ctx.hooks.clock() - waited_from < ctx.hooks.distro_wait_s:
                ctx.hooks.sleep(WSL_POLL_S)
                still_running = wsl_running(ctx.tools.wsl)
            waited = round(ctx.hooks.clock() - waited_from, 1)
            n = ctx.write_probe(key)
            start = _iso_now()
            changed = ctx.run_dir / f"wsl-changed-{run:02d}.txt"
            write_atomic(changed, (B28_PROBE_FILES[key] + "\n").encode("utf-8"))
            distro_start: float | None = None
            note = None
            if still_running:
                note = f"the distro was still running after {ctx.hooks.distro_wait_s:.0f} s; no distro start timed"
            else:
                t0 = ctx.hooks.clock()
                subprocess.run([*ctx.tools.wsl, "-d", WSL_DISTRO, "--exec", "true"], stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=300, check=False)
                distro_start = round(ctx.hooks.clock() - t0, 3)
            cmd = self._wsl_command(ctx, _wsl_form(changed))
            streamed = run_streamed(cmd, log=ctx.log(tag), clock=ctx.hooks.clock, timeout=WSL_LEG_TIMEOUT_S,
                                    header=f"probe {key} = {n}; distro start {distro_start}; elapsed seconds since "
                                           "verify.sh's launch prefix each line")
            markers = []
            for t, text in streamed.lines:
                if text.startswith("verify.sh: source"):
                    markers.append((t, "verify.sh: source"))
                elif text.startswith("=== "):
                    markers.append((t, text[4:].split(" (", 1)[0].strip()))
                elif text.startswith("wsl: "):
                    markers.append((t, "wsl: summary"))
            phases = phase_split(markers, streamed.wall)
            summary = [text for _, text in streamed.lines if text.startswith("wsl: ")]
            ok = streamed.exit == 0
            yield record(ctx, variant, file=key, run=run, counted=run >= warmup, start=start,
                         value=round(streamed.wall, 3), wall_s=round(streamed.wall, 3), exit=streamed.exit,
                         valid=ok, phases=phases,
                         summary_line=summary[-1] if summary else None,
                         b28={"probe": n, "distro_start_s": distro_start, "waited_for_stop_s": waited,
                              "distro_note": note, "command": cmd})
            if not ok:
                why = "timed out" if streamed.timed_out else f"exit {streamed.exit}"
                raise RunInvalid(f"WSL leg {tag} failed ({why})")

    # -- summaries ------------------------------------------------------------------------------

    def summarize(self, variant: Variant, records: list[dict[str, Any]], budget: Budget, files: list[str],
                  valid: bool, invalid_reason: str | None) -> dict[str, Any]:
        """summary.json's entry for `variant` from its records (warm-up runs are not counted)."""
        counted = [r for r in records if r["counted"] and r["valid"]]
        target = next((t for t in budget.targets
                       if variant.target_variant is not None and t.variant == variant.target_variant), None)
        summary: dict[str, Any] = {"schemaVersion": SCHEMA_VERSION, "valid": valid, "invalid_reason": invalid_reason,
                                   "target_variant": variant.target_variant, "unit": "s", "n": 0,
                                   "counted_required": variant.min_counted if target else 0, "percentiles": {},
                                   "min": None, "max": None, "values": [],
                                   "target": ({"percentile": target.percentile, "max": target.max, "unit": target.unit}
                                              if target else None),
                                   "verdict": "info", "verdict_reason": None}
        groups: dict[str, list[float]] = {}
        for r in counted:
            if variant.name in ("rebuild", "noop"):
                key = f"{r['file']}/{r['b28']['target']}" if r["file"] else r["b28"]["target"]
            else:
                key = r["file"] or "all"
            groups.setdefault(key, []).append(r["value"])
        summary["per_probe"] = {k: {"n": len(v), "p50": round(median(v), 3), "min": min(v), "max": max(v),
                                    "values": v} for k, v in sorted(groups.items())}
        if variant.name == "rebuild":
            gated = {k: v for k, v in groups.items() if k.endswith("/all")}
            if gated:
                worst = max(gated, key=lambda k: median(gated[k]))
                values = gated[worst]
                summary["worst_probe"] = worst.split("/")[0]
                summary["n"] = min(len(v) for v in gated.values())
            else:
                values = []
        elif variant.name == "noop":
            values = groups.get("all", [])
            summary["n"] = len(values)
        else:
            values = [r["value"] for r in counted]
            summary["n"] = len(values)
            if variant.name == "verify":
                names = list(dict.fromkeys(name for r in counted for name in (r["phases"] or {})))
                summary["phases_p50"] = {name: round(median([(r["phases"] or {}).get(name, 0.0) for r in counted]), 3)
                                         for name in names} if counted else {}
        if values:
            summary["values"] = values
            summary["min"] = min(values)
            summary["max"] = max(values)
            summary["percentiles"] = {"p50": round(median(values), 3)}
        if not valid:
            summary["verdict"] = "info"
            summary["verdict_reason"] = "invalid run"
        elif target is None:
            summary["verdict_reason"] = "informational series"
        elif sorted(files) != sorted(variant.files):
            summary["verdict_reason"] = "probe files differ from the method"
        elif summary["n"] < variant.min_counted:
            summary["verdict_reason"] = f"n < {variant.min_counted}"
        else:
            limit = to_seconds(target.max, target.unit)
            name = target.percentile or "max"
            p = percentile_value(values, name)
            summary["percentiles"][name] = round(p, 3)
            summary["verdict"] = "ok" if p <= limit else "over"
        return summary

    def describe(self, variant: Variant, summary: dict[str, Any]) -> str:
        """One clause of the last console line for `variant`."""
        p50 = summary["percentiles"].get("p50")
        shown = f"p50 {p50:.2f} s" if isinstance(p50, float) and p50 < 10 else (f"p50 {p50:.1f} s" if p50 is not None
                                                                                 else "no samples")
        name = variant.target_variant or variant.name
        target = summary["target"]
        if target:
            limit = f"max {target['max']:g} {target['unit']}"
            extra = f"worst probe {summary['worst_probe']}; {limit}" if summary.get("worst_probe") else limit
            verdict = summary["verdict"] if summary["verdict"] != "info" else f"info ({summary['verdict_reason']})"
            return f"{name} {shown} ({extra}) {verdict}"
        return f"{name} {shown} info"


HARNESSES: dict[str, type] = {"B28": B28Harness}


# ---------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------

_OPTIONS = ("list", "budget", "variant", "runs", "warmup", "files", "label", "perf-root", "milestone",
            "build-dir", "restore", "lock-record", "repo")
_FLAGS = {"list"}


def _normalise(argv: Sequence[str]) -> list[str]:
    """Maps -Budget, --budget, -BUDGET and -PerfRoot/--perf-root to argparse's --budget/--perf-root."""
    names = {name.replace("-", ""): name for name in _OPTIONS}
    out = []
    for token in argv:
        if token.startswith("-") and not re.fullmatch(r"-\d+(\.\d+)?", token):
            key = token.lstrip("-").replace("-", "").lower()
            if key in names:
                out.append("--" + names[key])
                continue
        out.append(token)
    return out


def _split(values: list[str] | None) -> list[str] | None:
    if not values:
        return None
    return [part.strip() for value in values for part in value.split(",") if part.strip()]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """Parses the command line (PowerShell or GNU spellings); exits 2 on a usage error."""
    parser = argparse.ArgumentParser(prog="perf.ps1", allow_abbrev=False,
                                     description="FrostIDE performance budgets (PLAN.md section 4)")
    parser.add_argument("--list", action="store_true", help="list every budget, its gate and whether it has a harness")
    parser.add_argument("--budget", default="B28", help="the budget ID (default B28)")
    parser.add_argument("--variant", action="append", help="variants to run, comma-separated (default: all)")
    parser.add_argument("--runs", type=int, help="counted runs (rounds for rebuild) per variant")
    parser.add_argument("--warmup", type=int, help="warm-up runs per variant")
    parser.add_argument("--files", action="append", help="probe files, comma-separated (core, main, sched)")
    parser.add_argument("--label", help="suffix for the run directory ([A-Za-z0-9._-])")
    parser.add_argument("--perf-root", help="results root (default $FROST_PERF_ROOT, else E:\\FrostBuilds\\perf)")
    parser.add_argument("--milestone", help="results subdirectory (default: the branch prefix, else adhoc)")
    parser.add_argument("--build-dir", help="the Debug build tree (default build\\win-clangcl)")
    parser.add_argument("--restore", help="restore the probe journal of an interrupted run directory and exit")
    parser.add_argument("--lock-record", help="a file holding night.py measure-begin's JSON line, for conditions.json")
    parser.add_argument("--repo", help=argparse.SUPPRESS)
    return parser.parse_args(_normalise(argv))


def _git(repo: Path, *args: str) -> str:
    exe = shutil.which("git")
    if not exe:
        raise PerfError("git is not on PATH")
    result = subprocess.run([os.path.abspath(exe), "-C", str(repo), *args], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", check=False)
    if result.returncode != 0:
        raise PerfError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def _milestone(branch: str) -> str:
    match = re.match(r"^(m\d+)/", branch)
    return match.group(1) if match else "adhoc"


def cmd_list(budgets_json: Path) -> int:
    """Prints every budget: ID, metric, gate and whether a harness exists. Returns 0."""
    for entry in load_budgets(budgets_json):
        gate = ", ".join(str(g.get("milestone")) for g in entry.get("gate", []))
        harness = "harness" if entry.get("id") in HARNESSES else "no harness"
        print(f"{entry.get('id'):4} gate {gate:9} {harness:10}  {entry.get('metric')}")
    return EXIT_OK


def cmd_restore(run_dir: Path) -> int:
    """perf.ps1 -Restore: restores a run's probe journal; 0 restored, 2 no journal, 3 refused."""
    journal = Path(run_dir) / "probes.json"
    if not journal.is_file():
        print(f"perf: no probe journal in {run_dir}", file=sys.stderr)
        return EXIT_USAGE
    problems = restore_journal(journal)
    for problem in problems:
        print(f"perf: {problem}", file=sys.stderr)
    if problems:
        print("perf: restore refused; fix the files above by hand, then run -Restore again", file=sys.stderr)
        return EXIT_INVALID
    print(f"perf: restored the probe files of {run_dir}")
    return EXIT_OK


def main(argv: Sequence[str] | None = None, *, tools: Tools | None = None, hooks: Hooks | None = None) -> int:
    """Runs the command line `argv`; returns the exit status (see the header). Tests inject tools and hooks."""
    args = parse_args(sys.argv[1:] if argv is None else argv)
    hooks = hooks or Hooks()
    try:
        return _run(args, tools, hooks)
    except PerfError as error:
        print(f"perf: {error}", file=sys.stderr)
        return EXIT_USAGE


def _run(args: argparse.Namespace, injected: Tools | None, hooks: Hooks) -> int:
    repo = Path(args.repo).resolve() if args.repo else REPO
    budgets_json = repo / BUDGETS_FILE
    if args.list:
        return cmd_list(budgets_json)
    if args.restore:
        return cmd_restore(Path(args.restore))

    budget = load_budget(budgets_json, args.budget)
    harness_type = HARNESSES.get(budget.id)
    if harness_type is None:
        raise PerfError(f"{budget.id} has no harness yet; harnessed budgets: {', '.join(sorted(HARNESSES))}")
    harness = harness_type()
    by_name = {v.name: v for v in harness.variants}
    names = _split(args.variant) or list(by_name)
    unknown = [n for n in names if n not in by_name]
    if unknown:
        raise PerfError(f"unknown variant(s) {', '.join(unknown)} for {budget.id}; known: {', '.join(by_name)}")
    variants = [by_name[n] for n in names]
    files = _split(args.files)
    probe_paths = harness.probe_files()
    if files and any(f not in probe_paths for f in files):
        raise PerfError(f"unknown probe file key in {files}; known: {', '.join(probe_paths)}")
    for count, flag in ((args.runs, "-Runs"), (args.warmup, "-Warmup")):
        if count is not None and count < 0:
            raise PerfError(f"{flag} must not be negative")
    if args.label and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.label):
        raise PerfError(f"label {args.label!r} may hold only letters, digits, '.', '_' and '-'")

    tools = injected or resolve_tools(repo)
    for variant in variants:
        missing = [name for name in harness.required_tools(variant) if not getattr(tools, name)]
        if missing:
            raise PerfError(f"variant {variant.name} needs {', '.join(missing)}, which was not found "
                            "(dot-source tools\\env.ps1, or run tools\\perf.ps1)")

    git_dir = Path(_git(repo, "rev-parse", "--absolute-git-dir").strip())
    pointer = git_dir / POINTER_NAME
    if pointer.exists():
        named = pointer.read_text(encoding="utf-8", errors="replace").strip()
        raise PerfError(f"an earlier run did not restore its probe files; run: perf.ps1 -Restore {named}")
    sha = _git(repo, "rev-parse", "HEAD").strip()
    branch = _git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip()
    status = _git(repo, "status", "--porcelain")

    used_keys = sorted({key for v in variants for key in (files or v.files)})
    paths = {key: repo / probe_paths[key] for key in used_keys}
    for key, path in paths.items():
        if not path.is_file():
            raise PerfError(f"probe file {probe_paths[key]} does not exist")
        if _git(repo, "status", "--porcelain", "--", probe_paths[key]).strip():
            raise PerfError(f"probe file {probe_paths[key]} has uncommitted changes")
        data = path.read_bytes()
        if has_probe(data):
            raise PerfError(f"probe file {probe_paths[key]} already carries a kB28Probe line")
        try:
            insert_probe(data, 1)
        except ValueError as error:
            raise PerfError(f"probe file {probe_paths[key]}: {error}") from error
    if any(v.name == "verify" for v in variants) and status.strip():
        raise PerfError("the verify variant needs a clean worktree (git status --porcelain is not empty): other "
                        "changes would alter verify's tidy and TSan sets")

    root = args.perf_root or os.environ.get("FROST_PERF_ROOT") or ""
    if not root:
        if sys.platform != "win32":
            raise PerfError("no results root: pass -PerfRoot or set FROST_PERF_ROOT")
        root = DEFAULT_PERF_ROOT
    milestone = args.milestone or _milestone(branch)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", milestone):
        raise PerfError(f"milestone {milestone!r} may hold only letters, digits, '.', '_' and '-'")
    name = f"{hooks.timestamp()}-{sha[:7]}" + (f"-{args.label}" if args.label else "")
    run_dir = Path(root) / milestone / budget.id.lower() / name
    if run_dir.exists():
        raise PerfError(f"the output directory {run_dir} already exists (results are write-once)")
    run_dir.mkdir(parents=True)

    build_dir = Path(args.build_dir) if args.build_dir else B28_DEFAULT_BUILD_DIR
    if not build_dir.is_absolute():
        build_dir = repo / build_dir
    verify_base = False
    if any(v.name == "verify" for v in variants):
        verify_base = tools.verify_base if tools.verify_base is not None else verify_declares_base(
            tools.powershell or [], repo / "tools" / "verify.ps1")
    journal = ProbeJournal(run_dir, repo, git_dir, paths)
    ctx = RunContext(repo=repo, build_dir=build_dir, run_dir=run_dir, tools=tools, hooks=hooks, journal=journal,
                     files=files, runs=args.runs, warmup=args.warmup, budget=budget, verify_base=verify_base)

    conditions = {
        "schemaVersion": SCHEMA_VERSION, "budget": budget.id, "variants": [v.name for v in variants],
        "files": {key: probe_paths[key] for key in used_keys}, "runs": args.runs, "warmup": args.warmup,
        "started": _iso_now(), "repo": str(repo), "build_dir": str(build_dir), "commands": harness.commands(ctx),
        "git": {"sha": sha, "branch": branch, "status_porcelain": status},
        "machine": {"cpu": _cpu_name(), "logical_cpus": os.cpu_count(), "memory": _memory(),
                    "system_cpu_pct_3s": _system_cpu_percent(hooks.cpu_sample_s, hooks.sleep),
                    "power_scheme": _power_scheme(), "os": platform.platform(), "python": platform.python_version()},
        "versions": {"cmake": _first_line(tools.cmake, "--version"), "ninja": _first_line(tools.ninja, "--version"),
                     "clang_cl": _first_line(tools.clang_cl, "--version"),
                     "sccache": _first_line(tools.sccache, "--version")},
        "wsl_distro_running": wsl_running(tools.wsl), "verify_base_head": verify_base,
        "lock_record": _lock_record(args.lock_record),
    }
    write_json_atomic(run_dir / "conditions.json", conditions)
    print(f"perf: {budget.id} -> {run_dir}", flush=True)

    records: dict[str, list[dict[str, Any]]] = {v.name: [] for v in variants}
    invalid: str | None = None
    journal.begin()
    try:
        if harness.needs_session_precheck:
            ctx.precheck()
        with (run_dir / "runs.jsonl").open("a", encoding="utf-8", newline="\n") as runs_out:
            for variant in variants:
                for rec in harness.run_variant(ctx, variant):
                    records[variant.name].append(rec)
                    runs_out.write(json.dumps(rec, ensure_ascii=False) + "\n")
                    runs_out.flush()
                    print(f"perf: {variant.name} {rec.get('file') or ''} round={rec.get('round')} run={rec.get('run')} "
                          f"{rec['wall_s']} s exit {rec['exit']}{'' if rec['counted'] else ' (warm-up)'}", flush=True)
        if harness.needs_session_precheck:
            ctx.precheck()
    except RunInvalid as error:
        invalid = str(error)
    except KeyboardInterrupt:
        invalid = "interrupted"
    finally:
        problems = restore_journal(journal.path)
        if problems:
            for problem in problems:
                print(f"perf: {problem}", file=sys.stderr)
            invalid = (invalid + "; " if invalid else "") + "restoring the probe files failed"
    if invalid:
        print(f"perf: the run is invalid: {invalid}", file=sys.stderr)

    summaries = {}
    clauses = []
    for variant in variants:
        used = files or list(variant.files)
        summary = harness.summarize(variant, records[variant.name], budget, used, invalid is None, invalid)
        summaries[variant.name] = summary
        clauses.append(harness.describe(variant, summary))
    write_json_atomic(run_dir / "summary.json", {"schemaVersion": SCHEMA_VERSION, "budget": budget.id,
                                                 "finished": _iso_now(), "valid": invalid is None,
                                                 "invalid_reason": invalid, "variants": summaries})
    line = f"{budget.id}: " + "; ".join(clauses)
    if invalid:
        line += f" (INVALID: {invalid})"
    print(line, flush=True)
    if invalid:
        return EXIT_INVALID
    return EXIT_OVER if any(s["verdict"] == "over" for s in summaries.values()) else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
