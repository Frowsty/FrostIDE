#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Builds and runs UnityCompilerArgumentsProbe (ProjectContext's csc.rsp / compiler-argument model)
# with the pinned .NET SDK in private scratch, like tests/managed/test_pdb_guard.py. Opt-in: skips
# (exit 77 through ctest's SKIP_RETURN_CODE) without FROST_MANAGED_TEST_DOTNET/_ROOT.
from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
DOTNET = os.environ.get("FROST_MANAGED_TEST_DOTNET", "")
SCRATCH = os.environ.get("FROST_MANAGED_TEST_ROOT", "")


def require_marker() -> None:
    # NuGet migration writes to the real LocalAppData Known Folder irrespective of profile
    # redirects; run only where it has already happened (as test_pdb_guard.py does).
    if os.name != "nt":
        return
    class Guid(ctypes.Structure):
        _fields_ = [("data1", wintypes.DWORD), ("data2", wintypes.WORD), ("data3", wintypes.WORD),
                    ("data4", ctypes.c_ubyte * 8)]
    identifier = Guid(0xF1B32785, 0x6FBA, 0x4FCF, (ctypes.c_ubyte * 8)(0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91))
    path = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(identifier), 0x4000, None, ctypes.byref(path)) != 0:
        raise RuntimeError("cannot inspect LocalAppData")
    local = Path(path.value)
    ctypes.windll.ole32.CoTaskMemFree(path)
    if not (local / "NuGet" / "Migrations" / "1").is_file():
        raise unittest.SkipTest("NuGet migration marker is absent")


@unittest.skipUnless(DOTNET and SCRATCH, "set absolute FROST_MANAGED_TEST_DOTNET and FROST_MANAGED_TEST_ROOT")
class CompilerArgumentsTest(unittest.TestCase):
    def setUp(self) -> None:
        require_marker()
        self.dotnet = Path(DOTNET)
        root = Path(SCRATCH)
        if not self.dotnet.is_absolute() or not self.dotnet.is_file() or not root.is_absolute() or not root.is_dir():
            raise ValueError("SDK executable and scratch root must be existing absolute paths")
        self.work = Path(tempfile.mkdtemp(prefix="frost-unity-args-", dir=root))
        self.addCleanup(shutil.rmtree, self.work, True)
        profile = self.work / "profile"
        folders = {name: self.work / name for name in ("nuget", "http", "plugins", "tmp")}
        for folder in (profile, *folders.values()):
            folder.mkdir()
        self.env = os.environ.copy()
        self.env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                         "LOCALAPPDATA": str(profile / "local"), "XDG_CONFIG_HOME": str(profile / "config"),
                         "XDG_DATA_HOME": str(profile / "data"), "XDG_CACHE_HOME": str(profile / "cache"),
                         "DOTNET_CLI_HOME": str(profile), "DOTNET_ROOT": str(self.dotnet.parent),
                         "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                         "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false", "NUGET_PACKAGES": str(folders["nuget"]),
                         "NUGET_HTTP_CACHE_PATH": str(folders["http"]),
                         "NUGET_PLUGINS_CACHE_PATH": str(folders["plugins"]),
                         "TMP": str(folders["tmp"]), "TEMP": str(folders["tmp"]), "TMPDIR": str(folders["tmp"])})
        self.env.pop("NUGET_PLUGIN_PATHS", None)
        self.config = self.work / "NuGet.Config"
        self.config.write_text("<configuration><packageSources><clear /></packageSources></configuration>",
                               encoding="utf-8")

    def test_probe_models_response_files_and_arguments(self) -> None:
        output = self.work / "out"
        obj = self.work / "obj"
        build = subprocess.run([str(self.dotnet), "build", str(REPO / "tests/unity/UnityCompilerArgumentsProbe.csproj"),
                                "--nologo", "--configfile", str(self.config),
                                f"-p:BaseIntermediateOutputPath={obj}{os.sep}", f"-p:BaseOutputPath={output}{os.sep}"],
                               cwd=self.work, env=self.env, capture_output=True, text=True, timeout=600)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        run = subprocess.run([str(self.dotnet), str(output / "Debug/net10.0/UnityCompilerArgumentsProbe.dll"),
                              str(self.work / "run")], cwd=self.work, env=self.env, capture_output=True, text=True,
                             timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("all checks passed", run.stdout)
        self.assertNotIn("FAIL", run.stdout)


if __name__ == "__main__":
    result = unittest.main(exit=False).result
    if not result.wasSuccessful():
        sys.exit(1)
    sys.exit(77 if result.testsRun == len(result.skipped) else 0)
