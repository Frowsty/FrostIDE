# SPDX-License-Identifier: Apache-2.0
#
# Tests for the lldb-dap build recipes (tools/build-lldb.ps1 on Windows, tools/build-lldb.sh on
# Linux; ADR 0049) that run without building anything: the scripts parse, and their verify-only
# mode, which the bootstrap scripts use on an installed artifact, refuses a missing install and a
# tree whose files differ from the pin in sdk/sdk.lock.json, against a scratch SDK root. Each OS
# tests its own script.

from __future__ import annotations

import json
import ntpath
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

from tools_test_support import REPO, TOOLS, TempDirTestCase, clean_env

LOCK = json.loads((REPO / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))


def _powershell() -> str:
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return ntpath.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell 5.1")
class BuildLldbPowerShellTest(TempDirTestCase):
    def _verify(self, sdk: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
             str(TOOLS / "build-lldb.ps1"), "-SdkRoot", str(sdk), "-VerifyOnly"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False,
            timeout=60)

    def test_script_parses_and_declares_its_parameters(self):
        harness = self.tmp / "parse.ps1"
        harness.write_text(
            "param([string]$Path)\n$t = $null; $e = $null\n"
            "$ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$t, [ref]$e)\n"
            "foreach ($x in $e) { \"error: $x\" }\n"
            "foreach ($p in $ast.ParamBlock.Parameters) { \"param: $($p.Name.VariablePath.UserPath)\" }\n",
            encoding="utf-8-sig")
        result = subprocess.run(
            [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(harness),
             str(TOOLS / "build-lldb.ps1")],
            capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("error:", result.stdout)
        params = {line.split(": ", 1)[1] for line in result.stdout.splitlines() if line.startswith("param: ")}
        self.assertLessEqual({"SdkRoot", "Jobs", "LinkJobs", "KeepBuildTree", "VerifyAttestation", "PrintPin",
                              "VerifyOnly"}, params)

    def test_verify_only_refuses_a_missing_install(self):
        result = self._verify(self.tmp)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not installed", result.stdout + result.stderr)

    def test_verify_only_refuses_files_that_differ_from_the_pin(self):
        dest = self.tmp / f"lldb-{LOCK['windows']['lldb']['version']}"
        (dest / "bin").mkdir(parents=True)
        (dest / ".frost-installed").write_text("test\n", encoding="ascii")
        (dest / "bin" / "lldb-dap.exe").write_bytes(b"not the pinned binary")
        (dest / "bin" / "planted.dll").write_bytes(b"x")
        result = self._verify(self.tmp)
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("differs from windows.lldb.files", output)
        self.assertIn("planted.dll", output)
        pinned = LOCK["windows"]["lldb"].get("files", {})
        self.assertIn("differs: bin/lldb-dap.exe" if "bin/lldb-dap.exe" in pinned else "not pinned: bin/lldb-dap.exe",
                      output)


@unittest.skipIf(sys.platform == "win32", "bash and build-lldb.sh run in WSL")
class BuildLldbShellTest(TempDirTestCase):
    def _verify(self, sdk: Path) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(TOOLS / "build-lldb.sh"), "--verify-only"], capture_output=True, text=True,
                              env=clean_env(FROST_SDK=str(sdk)), check=False, timeout=60)

    def test_script_passes_bash_n(self):
        self.assertIsNotNone(shutil.which("bash"))
        result = subprocess.run(["bash", "-n", str(TOOLS / "build-lldb.sh")], capture_output=True, text=True,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_verify_only_refuses_a_missing_install(self):
        result = self._verify(self.tmp)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not installed", result.stderr)

    def test_verify_only_refuses_files_that_differ_from_the_pin(self):
        dest = self.tmp / f"lldb-{LOCK['linux']['lldb']['version']}"
        (dest / "bin").mkdir(parents=True)
        (dest / ".frost-installed").write_text("test\n", encoding="ascii")
        (dest / "bin" / "lldb-dap").write_bytes(b"not the pinned binary")
        (dest / "bin" / "planted.so").write_bytes(b"x")
        result = self._verify(self.tmp)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("differs from linux.lldb.files", result.stderr)
        self.assertIn("planted.so", result.stderr)
        pinned = LOCK["linux"]["lldb"].get("files", {})
        self.assertIn("differs: bin/lldb-dap" if "bin/lldb-dap" in pinned else "not pinned: bin/lldb-dap",
                      result.stderr)


if __name__ == "__main__":
    unittest.main()
