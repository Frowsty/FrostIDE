# SPDX-License-Identifier: Apache-2.0
#
# Tests for the Windows PowerShell scripts in tools/ that can run without installing
# anything: bootstrap.ps1's Python version check, taken from the script's syntax tree
# so the bootstrap itself never runs, and env.ps1's cache redirection (D1: no tool
# caches on the system drive), dot-sourced in a child PowerShell. Windows only: the
# scripts target Windows PowerShell 5.1. tests/tools/CMakeLists.txt registers this
# suite on Windows only.

from __future__ import annotations

import json
import ntpath
import os
import subprocess
import sys
import unittest
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env


def _powershell() -> str:
    """The absolute path of Windows PowerShell 5.1."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return ntpath.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def run_powershell(script: Path, *args: str) -> subprocess.CompletedProcess:
    """Runs `script` with -File (no profile, non-interactive); returns the completed process."""
    return subprocess.run(
        [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script), *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False,
    )


# Defines $MinPython and Assert-PythonVersion from bootstrap.ps1's syntax tree, then calls
# the function once per version in the JSON array $CasesFile and prints "ok <input>" or
# "fail <input>: <message>" per case (a file, because -File cannot pass an empty string
# or a line break reliably).
_ASSERT_PYTHON_VERSION = r"""
param([string]$Script, [string]$CasesFile)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 emits a JSON array as one object, so no @() here: that would nest it.
$Versions = Get-Content -Raw -Path $CasesFile | ConvertFrom-Json
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { throw "bootstrap.ps1 does not parse: $($errors[0])" }
$minimum = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -eq '$MinPython' }, $false)
$function = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $n.Name -eq 'Assert-PythonVersion' }, $true)
if (-not $minimum -or -not $function) { throw 'bootstrap.ps1 lacks $MinPython or Assert-PythonVersion' }
. ([scriptblock]::Create($minimum.Extent.Text + "`n" + $function.Extent.Text))
foreach ($version in $Versions) {
    $shown = $version -replace "`r", '\r' -replace "`n", '\n'
    try { Assert-PythonVersion $version 'X:\venv\Scripts\python.exe' 'X:\venv'; "ok $shown" }
    catch { "fail ${shown}: $($_.Exception.Message)" }
}
"""

# Dot-sources env.ps1 with the given SDK root and prints the variables it sets as NAME=VALUE.
_ENV_DUMP = r"""
param([string]$EnvScript, [string]$SdkRoot)
$ErrorActionPreference = 'Stop'
. $EnvScript -SdkRoot $SdkRoot 3>$null
foreach ($name in @('FROST_SDK', 'DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'NUGET_HTTP_CACHE_PATH',
                    'NUGET_PLUGINS_CACHE_PATH', 'SCCACHE_DIR', 'VCPKG_DOWNLOADS', 'VCPKG_DEFAULT_BINARY_CACHE',
                    'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH', 'DOTNET_GENERATE_ASPNET_CERTIFICATE')) {
    "$name=$([Environment]::GetEnvironmentVariable($name))"
}
"""


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell scripts")
class BootstrapPythonCheckTest(TempDirTestCase):
    def test_versions_below_3_11_or_unreadable_stop_the_bootstrap(self) -> None:
        script = self.tmp / "assert_python_version.ps1"
        script.write_text(_ASSERT_PYTHON_VERSION, encoding="utf-8", newline="\n")
        accepted = ["3.11.0", "3.14.8", "3.12.1\r\n"]
        too_old = ["3.10.12", "2.7.18"]
        unreadable = ["", "Python 3.12.1", "3.12"]
        cases_file = self.tmp / "cases.json"
        cases_file.write_text(json.dumps(accepted + too_old + unreadable), encoding="utf-8", newline="\n")
        result = run_powershell(script, str(TOOLS / "bootstrap.ps1"), str(cases_file))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), len(accepted + too_old + unreadable), result.stdout)
        for line in lines[:len(accepted)]:
            self.assertTrue(line.startswith("ok "), line)
        for line in lines[len(accepted):len(accepted + too_old)]:
            self.assertTrue(line.startswith("fail "), line)
            self.assertIn("need Python 3.11 or newer", line)
            self.assertIn(r"delete X:\venv", line)
        for line in lines[len(accepted + too_old):]:
            self.assertTrue(line.startswith("fail "), line)
            self.assertIn("could not read the Python version", line)
            self.assertIn(r"delete X:\venv", line)


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell scripts")
class EnvCacheRedirectionTest(TempDirTestCase):
    def test_every_tool_cache_is_under_the_sdk_root(self) -> None:
        sdk = os.environ.get("FROST_SDK")
        if not sdk:
            self.skipTest("FROST_SDK is not set (dot-source tools/env.ps1 first)")
        system_drive = os.environ.get("SystemDrive", "C:").upper()
        if ntpath.splitdrive(sdk)[0].upper() == system_drive:
            self.skipTest("the SDK itself is on the system drive")
        script = self.tmp / "env_dump.ps1"
        script.write_text(_ENV_DUMP, encoding="utf-8", newline="\n")
        result = run_powershell(script, str(TOOLS / "env.ps1"), sdk)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        sdk_drive = ntpath.splitdrive(sdk)[0].upper()
        for name in ("DOTNET_CLI_HOME", "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH",
                     "SCCACHE_DIR", "VCPKG_DOWNLOADS"):
            self.assertTrue(ntpath.normcase(values[name]).startswith(ntpath.normcase(sdk) + "\\"),
                            f"{name}={values[name]} is not under {sdk}")
            self.assertTrue(Path(values[name]).is_dir(), f"{name}: {values[name]} was not created")
        # The vcpkg binary cache sits at the root of the SDK drive.
        self.assertEqual(ntpath.splitdrive(values["VCPKG_DEFAULT_BINARY_CACHE"])[0].upper(), sdk_drive)
        # A new .NET CLI home must not edit the user PATH or create a certificate on first use.
        self.assertEqual(values["DOTNET_ADD_GLOBAL_TOOLS_TO_PATH"], "0")
        self.assertEqual(values["DOTNET_GENERATE_ASPNET_CERTIFICATE"], "0")


if __name__ == "__main__":
    unittest.main()
