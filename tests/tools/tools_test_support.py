# SPDX-License-Identifier: Apache-2.0
#
# Shared helpers for the repository-tool tests in tests/tools: repository paths, fixture
# copies in temporary directories, and running a tool or git as a child process with an
# environment that cannot see the developer's SDK settings (FROST_QT_ROOT and friends).

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
FIXTURES = Path(__file__).resolve().parent / "fixtures"

# The tools are importable as modules for in-process unit tests.
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

# Environment variables that would let a test read the real SDK or corpus root.
_SCRUBBED = ("FROST_QT_ROOT", "FROST_CORPUS_ROOT", "SOURCE_DATE_EPOCH")


def clean_env(**extra: str) -> dict[str, str]:
    """The current environment without the FROST_* locations, plus `extra`."""
    env = {k: v for k, v in os.environ.items() if k not in _SCRUBBED}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.update(extra)
    return env


def run_tool(script: str, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    """Runs tools/<script> with this interpreter; returns the completed process (text mode)."""
    return subprocess.run(
        [sys.executable, "-B", str(TOOLS / script), *args],
        capture_output=True, text=True, encoding="utf-8", env=env or clean_env(), check=False,
    )


def git(*args: str, cwd: Path) -> str:
    """Runs git in `cwd` with a fixed identity; raises on failure and returns stdout."""
    exe = shutil.which("git")
    if exe is None:
        raise AssertionError("git is required on PATH (part of the dev environment)")
    result = subprocess.run(
        [exe, "-c", "user.name=FrostIDE tests", "-c", "user.email=tests@frostide.invalid",
         "-c", "init.defaultBranch=main", "-c", "core.autocrlf=false", *args],
        cwd=cwd, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(args)} failed: {result.stderr}")
    return result.stdout


class TempDirTestCase(unittest.TestCase):
    """A test case with a fresh temporary directory in self.tmp, removed afterwards."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="frost-tools-")
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        # git marks pack files read-only on Windows; make them writable so cleanup works.
        for root, _, files in os.walk(self.tmp):
            for name in files:
                try:
                    os.chmod(os.path.join(root, name), 0o666)
                except OSError:
                    pass
        self._tmp.cleanup()

    def copy_fixture(self, name: str) -> Path:
        """Copies tests/tools/fixtures/<name> into the temporary directory; returns the copy."""
        target = self.tmp / Path(name).name
        shutil.copytree(FIXTURES / name, target)
        return target
