# SPDX-License-Identifier: Apache-2.0
#
# Tests for the pure helpers inside the verify scripts (ADR 0027), run without running a verification.
# On Windows the functions Split-WslSummary, ConvertTo-WslPath, Get-CtestCounts and Get-WslArgs are taken
# from tools/verify.ps1's syntax tree and called in a child Windows PowerShell 5.1, both PowerShell
# scripts must parse, and every ctest call keeps error boxes off (checked against a real ctest). On
# Linux tools/verify.sh must pass `bash -n`, and its parse_args, ctest_counts, join_items,
# run_tsan_quick and run_tidy functions are cut out of the script and run in a child bash
# (tidy and the TSan quick leg are off unless switched on, ADR 0027 decision 9). The two sides share the
# tables in tests/tools/fixtures/verify (ctest outputs, summary items, WSL paths), so the PowerShell and
# bash halves of the gate are checked against the same expectations.

from __future__ import annotations

import json
import ntpath
import os
import re
import shutil
import subprocess
import sys
import time
import unittest
from pathlib import Path

from tools_test_support import FIXTURES, REPO, TOOLS, TempDirTestCase, clean_env

VERIFY = FIXTURES / "verify"


def _table(name: str):
    return json.loads((VERIFY / name).read_text(encoding="utf-8"))


def _powershell() -> str:
    """The absolute path of Windows PowerShell 5.1."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return ntpath.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


# Defines the named functions from verify.ps1's syntax tree (nothing else of the script runs), then
# evaluates each case of $CasesFile: {"fn": name, "arg": string or [lines]} and prints one JSON line
# per case. Files, not arguments, because -File cannot pass empty strings or line breaks reliably.
_HARNESS = r"""
param([string]$Script, [string]$CasesFile, [string]$OutFile)
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { throw "verify.ps1 does not parse: $($errors[0])" }
$names = @('Split-WslSummary', 'ConvertTo-WslPath', 'Get-CtestCounts', 'Get-WslArgs')
$text = ''
foreach ($name in $names) {
    $fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $n.Name -eq $name }, $true)
    if (-not $fn) { throw "verify.ps1 lacks $name" }
    $text += $fn.Extent.Text + "`n"
}
. ([scriptblock]::Create($text))
$cases = Get-Content -Raw -Encoding UTF8 -Path $CasesFile | ConvertFrom-Json
$out = New-Object System.Collections.Generic.List[string]
foreach ($case in $cases) {
    if ($case.fn -eq 'Get-CtestCounts') { $value = Get-CtestCounts ([string[]]@($case.arg)) }
    elseif ($case.fn -eq 'Split-WslSummary') { $value = Split-WslSummary $case.arg }
    elseif ($case.fn -eq 'Get-WslArgs') {
        # The case's arg is an object of named parameters; JSON null stays $null.
        $named = @{}
        foreach ($p in $case.arg.PSObject.Properties) { $named[$p.Name] = $p.Value }
        $value = Get-WslArgs @named
    }
    else { $value = & $case.fn $case.arg }
    # ConvertTo-Json flattens a one-element array; wrap so every result keeps its shape.
    $out.Add((ConvertTo-Json -Compress -InputObject @{ v = $value }))
}
[System.IO.File]::WriteAllLines($OutFile, $out, (New-Object System.Text.UTF8Encoding $false))
"""


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell 5.1")
class VerifyPs1Test(TempDirTestCase):
    """verify.ps1's helpers against the shared tables."""

    def _call(self, cases: list[dict]) -> list:
        harness = self.tmp / "harness.ps1"
        harness.write_text(_HARNESS, encoding="utf-8-sig")
        cases_file = self.tmp / "cases.json"
        cases_file.write_text(json.dumps(cases, ensure_ascii=False), encoding="utf-8")
        out = self.tmp / "out.jsonl"
        result = subprocess.run(
            [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(harness),
             str(TOOLS / "verify.ps1"), str(cases_file), str(out)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return [json.loads(line)["v"] for line in out.read_text(encoding="utf-8").splitlines()]

    def test_split_wsl_summary_matches_the_join_table(self):
        table = _table("summary_join.json")
        got = self._call([{"fn": "Split-WslSummary", "arg": row["line"]} for row in table])
        for row, value in zip(table, got):
            with self.subTest(line=row["line"]):
                self.assertEqual(value or [], row["items"])

    def test_convert_to_wsl_path_matches_the_path_table(self):
        table = _table("wsl_paths.json")
        got = self._call([{"fn": "ConvertTo-WslPath", "arg": row["windows"]} for row in table])
        for row, value in zip(table, got):
            with self.subTest(path=row["windows"]):
                self.assertEqual(value, row["wsl"])

    def test_get_ctest_counts_matches_the_fixture_table(self):
        table = _table("ctest_counts.json")
        names = sorted(table)
        cases = [{"fn": "Get-CtestCounts",
                  "arg": (VERIFY / "ctest" / name).read_text(encoding="utf-8").splitlines()} for name in names]
        for name, value in zip(names, self._call(cases)):
            with self.subTest(fixture=name):
                self.assertEqual(value, table[name])

    def test_get_wsl_args_passes_the_switches_and_the_list(self):
        # -Tidy and -TsanQuick are off by default (ADR 0027 decision 9): without them verify.sh gets no
        # switch and FROST_CHANGED_FILES stays unset, so it neither tidies nor makes a list.
        script = "/mnt/e/r/tools/verify.sh"
        base = {"Script": script, "Jobs": 8, "Base": "main", "ChangedList": None, "Mirror": "", "Full": False,
                "Tidy": False, "TsanQuick": False}
        head = ["-d", "Ubuntu-24.04", "--exec", "env", "FROST_JOBS=8", "FROST_VERIFY_BASE=main"]
        cases = [
            ({}, head + ["bash", script, "--no-checks"]),
            ({"ChangedList": "/mnt/e/r/build/verify-changed-1.txt", "Tidy": True, "TsanQuick": True, "Mirror": "~/m"},
             head + ["FROST_CHANGED_FILES=/mnt/e/r/build/verify-changed-1.txt", "FROST_MIRROR=~/m", "bash", script,
                     "--no-checks", "--tidy", "--tsan-quick"]),
            # The list was asked for but could not be made: an empty value, so verify.sh does not retry.
            ({"ChangedList": "", "TsanQuick": True}, head + ["FROST_CHANGED_FILES=", "bash", script, "--no-checks",
                                                             "--tsan-quick"]),
            ({"Full": True, "Tidy": True, "Jobs": 4}, head[:4] + ["FROST_JOBS=4", "FROST_VERIFY_BASE=main", "bash",
                                                                  script, "--no-checks", "--full", "--tidy"]),
        ]
        got = self._call([{"fn": "Get-WslArgs", "arg": {**base, **delta}} for delta, _ in cases])
        for (delta, expected), value in zip(cases, got):
            with self.subTest(case=delta):
                self.assertEqual(value, expected)

    def test_scripts_parse_and_declare_their_parameters(self):
        script = r"""
param([string]$Path)
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
foreach ($e in $errors) { "error: $e" }
if ($ast.ParamBlock) { foreach ($p in $ast.ParamBlock.Parameters) { "param: $($p.Name.VariablePath.UserPath)" } }
"""
        harness = self.tmp / "parse.ps1"
        harness.write_text(script, encoding="utf-8-sig")
        for name in ("verify.ps1", "perf.ps1"):
            with self.subTest(script=name):
                result = subprocess.run(
                    [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                     str(harness), str(TOOLS / name)],
                    capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("error:", result.stdout)
                if name == "verify.ps1":
                    params = set(re.findall(r"^param: (\w+)$", result.stdout, re.M))
                    # frost_perf.py passes -Base HEAD when the script declares it (verify_declares_base).
                    self.assertLessEqual({"Quick", "Full", "WindowsOnly", "BuildDir", "WslMirror", "Jobs", "TestJobs",
                                          "Tidy", "TsanQuick", "Base"}, params)

    def test_ctest_label_filters_are_anchored(self):
        text = (TOOLS / "verify.ps1").read_text(encoding="utf-8")
        # An unanchored -L fuzz would also select the mod.fuzz corpus replays (ADR 0027 decision 8).
        self.assertIn("'-L', '^fuzz$'", text)
        self.assertIn("'-LE', '^stress$'", text)
        self.assertNotRegex(text, r"'-LE?', '(?!\^)")

    def test_ctest_starts_tests_with_error_boxes_off(self):
        # ctest's default interactive mode starts every test with the default error mode, so a test
        # that fails to start or crashes raises a box on the user's desktop; verify.ps1 therefore
        # passes --interactive-debug-mode 0 to every ctest call (ADR 0027 decision 3).
        text = (TOOLS / "verify.ps1").read_text(encoding="utf-8")
        calls = [line for line in text.splitlines() if re.search(r"[{&]\s*ctest\b", line)]
        self.assertTrue(calls, "verify.ps1 runs ctest")
        for line in calls:
            self.assertIn("--interactive-debug-mode 0", line)
        # And ctest's mode 0 really does hand SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX (0x3) on.
        ctest = os.environ.get("FROST_CTEST") or shutil.which("ctest")
        if not ctest:
            self.skipTest("ctest is not on PATH")
        probe = self.tmp / "mode.py"
        probe.write_text("import ctypes\nprint(f'errormode={ctypes.windll.kernel32.GetErrorMode():#x}')\n",
                         encoding="utf-8")
        python = getattr(sys, "_base_executable", None) or sys.executable
        (self.tmp / "CTestTestfile.cmake").write_text(
            f'add_test(mode "{Path(python).as_posix()}" "-I" "{probe.as_posix()}")\n', encoding="utf-8")
        result = subprocess.run([ctest, "--test-dir", str(self.tmp), "-V", "--interactive-debug-mode", "0"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
                                check=False, env=clean_env())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.search(r"errormode=(0x[0-9a-f]+)", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        self.assertEqual(int(match.group(1), 16) & 0x3, 0x3, result.stdout)


def _bash() -> str | None:
    return shutil.which("bash")


def _bash_function(name: str) -> str:
    """The text of `name() { ... }` in verify.sh: from its first line to the next line that is just '}'."""
    lines = (TOOLS / "verify.sh").read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith(f"{name}() {{"))
    end = next(i for i in range(start, len(lines)) if lines[i] == "}")
    return "\n".join(lines[start:end + 1]) + "\n"


@unittest.skipIf(sys.platform == "win32", "bash and verify.sh run in WSL")
class VerifyShTest(TempDirTestCase):
    """verify.sh's helpers against the shared tables."""

    def _bash(self, script: str, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        path = self.tmp / "case.sh"
        path.write_text("set -euo pipefail\n" + script, encoding="utf-8")
        return subprocess.run([_bash() or "/bin/bash", str(path), *args], capture_output=True, text=True,
                              encoding="utf-8", env=clean_env(), cwd=cwd or self.tmp, check=False)

    def test_script_passes_bash_n(self):
        result = subprocess.run([_bash() or "/bin/bash", "-n", str(TOOLS / "verify.sh")], capture_output=True,
                                text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_mirror_replaces_older_and_same_time_changes_without_touching_identical_files(self):
        source, mirror = self.tmp / "source", self.tmp / "mirror"
        source.mkdir()
        mirror.mkdir()
        past = int(time.time()) - 180
        for name in ("older.hpp", "same_time.hpp", "unchanged.hpp"):
            (source / name).write_text("new\n", encoding="utf-8")
            (mirror / name).write_text("new\n" if name == "unchanged.hpp" else "old\n", encoding="utf-8")
            os.utime(source / name, (past, past))
            os.utime(mirror / name, (past + 60, past + 60))
        os.utime(mirror / "same_time.hpp", (past, past))
        unchanged_time = (mirror / "unchanged.hpp").stat().st_mtime_ns
        (mirror / "deleted.txt").write_text("gone", encoding="utf-8")
        (mirror / "build").mkdir()
        (mirror / "build" / "object.o").write_text("keep", encoding="utf-8")
        script = _bash_function("mirror_source") + 'mirror_source "$1" "$2"\n'
        result = self._bash(script, str(source), str(mirror))
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("older.hpp", "same_time.hpp"):
            self.assertEqual((mirror / name).read_text(encoding="utf-8"), "new\n")
            # A target built after the original snapshots must see the replacement as newer.
            self.assertGreater((mirror / name).stat().st_mtime, past + 120)
        self.assertEqual((mirror / "unchanged.hpp").stat().st_mtime_ns, unchanged_time)
        self.assertFalse((mirror / "deleted.txt").exists())
        self.assertTrue((mirror / "build" / "object.o").exists())

    def test_ctest_counts_matches_the_fixture_table(self):
        table = _table("ctest_counts.json")
        script = _bash_function("join_items") + _bash_function("ctest_counts") + 'ctest_counts < "$1"\n'
        for name, expected in sorted(table.items()):
            with self.subTest(fixture=name):
                result = self._bash(script, str(VERIFY / "ctest" / name))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected or "")

    def test_join_items_matches_the_join_table(self):
        script = _bash_function("join_items") + 'join_items "$@"\n'
        for row in _table("summary_join.json"):
            with self.subTest(line=row["line"]):
                result = self._bash(script, *row["items"])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, row["line"])

    def test_run_tsan_quick_selects_by_module_label(self):
        # run_config is replaced by a stub that prints its arguments; the selection itself is the real
        # tools/verify_support.py, found relative to the working directory as in the mirror.
        stub = 'run_config() { printf "run_config"; printf " [%s]" "$@"; echo; }\n'
        script = (stub + _bash_function("run_tsan_quick") +
                  'summary=(); status=0; TSAN_QUICK=1; changed_list="$1"; changed_reason="$2"\n'
                  'run_tsan_quick\n'
                  'printf "summary [%s]\\n" "${summary[@]+"${summary[@]}"}"\n'
                  'echo "status $status"\n')
        cases = [
            (["src/io/src/process_posix.cpp"],
             r"run_config [tsan-quick] [linux-clang-tsan] [linux-clang-tsan] [-LE] [^stress$] [-L] [^mod\.(io|text)$]"),
            (["docs/progress.md", "src/app/src/main.cpp"], "summary [tsan-quick not-needed]"),
            (["tests/support/test_env.hpp"],
             r"run_config [tsan-quick] [linux-clang-tsan] [linux-clang-tsan] [-LE] [^stress$] [-L] "
             r"[^mod\.(io|rpc|sched|text)$]"),
        ]
        for paths, expected in cases:
            with self.subTest(paths=paths):
                listing = self.tmp / "changed.txt"
                listing.write_text("".join(f"{p}\n" for p in paths), encoding="utf-8")
                result = self._bash(script, str(listing), "", cwd=REPO)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertIn("status 0", result.stdout)
        # Without a list every module runs, and the reason is printed.
        result = self._bash(script, "", "verify.ps1 had none", cwd=REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no changed-file list: verify.ps1 had none", result.stdout)
        self.assertIn(r"[^mod\.(io|rpc|sched|text)$]", result.stdout)
        # An invalid list fails the leg instead of skipping it.
        bad = self.tmp / "bad.txt"
        bad.write_text("/abs/path.cpp\n", encoding="utf-8")
        result = self._bash(script, str(bad), "", cwd=REPO)
        self.assertIn("summary [tsan-quick FAILED (selection)]", result.stdout)
        self.assertIn("status 1", result.stdout)
        # Without --tsan-quick (the default, ADR 0027 decision 9) nothing is selected or built.
        listing = self.tmp / "changed.txt"
        listing.write_text("src/io/src/process_posix.cpp\n", encoding="utf-8")
        result = self._bash(script.replace("TSAN_QUICK=1", "TSAN_QUICK=0"), str(listing), "", cwd=REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("run_config", result.stdout)
        self.assertIn("summary [tsan-quick not-run (off by default)]", result.stdout)
        self.assertIn("status 0", result.stdout)

    def test_parse_args_leaves_tidy_and_tsan_quick_off_by_default(self):
        script = _bash_function("parse_args") + 'parse_args "$@"\necho "$FULL $CHECKS $TIDY $TSAN_QUICK"\n'
        cases = [
            ([], "0 1 0 0"),
            (["--tidy", "--tsan-quick"], "0 1 1 1"),
            (["-Full", "-Tidy", "-TsanQuick", "--no-checks"], "1 0 1 1"),
            (["--full", "--quick"], "0 1 0 0"),
        ]
        for args, expected in cases:
            with self.subTest(args=args):
                result = self._bash(script, *args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
        result = self._bash(script, "--tsan")
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage: verify.sh", result.stderr)

    def test_run_tidy_reports_off_without_the_switch(self):
        script = (_bash_function("run_tidy") +
                  'summary=(); status=0; TIDY=0; debug_built=1\n'
                  'run_tidy\n'
                  'printf "summary [%s]\\n" "${summary[@]}"\n'
                  'echo "status $status"\n')
        result = self._bash(script, cwd=REPO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("=== tidy", result.stdout)
        self.assertIn("summary [tidy not-run (off by default)]", result.stdout)
        self.assertIn("status 0", result.stdout)

    def test_ctest_label_filters_are_anchored(self):
        text = (TOOLS / "verify.sh").read_text(encoding="utf-8")
        self.assertIn("-L '^fuzz$'", text)
        self.assertNotRegex(text, r"-LE? (?!'\^|\"\$regex\")")


if __name__ == "__main__":
    unittest.main()
