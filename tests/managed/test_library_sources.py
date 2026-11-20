#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Deferred real Roslyn emit/PDB/ILSpy library-source acceptance over an already verified local payload.
from __future__ import annotations
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import roslyn_host

class LibrarySources(unittest.TestCase):
    def test_real_portable_pdb_decompiler_and_il(self):
        dotnet = Path(os.environ.get("FROST_TEST_CSHARP_DOTNET", ""))
        server = Path(os.environ.get("FROST_TEST_CSHARP_SERVER", ""))
        scratch = Path(os.environ.get("FROST_MANAGED_TEST_ROOT", ""))
        self.assertTrue(dotnet.is_absolute() and dotnet.is_file(), "Required pinned FROST_TEST_CSHARP_DOTNET is unavailable")
        self.assertTrue(server.is_absolute() and server.is_file(), "Required reviewed FROST_TEST_CSHARP_SERVER is unavailable")
        self.assertTrue(scratch.is_absolute() and scratch.is_dir(), "Required owned FROST_MANAGED_TEST_ROOT is unavailable")
        roslyn_host.verify_payload(server.parent, ROOT)
        with tempfile.TemporaryDirectory(prefix="frost-library-source-", dir=scratch) as work:
            directory = Path(work)
            profile = directory / "profile"
            profile.mkdir()
            environment = os.environ.copy()
            environment.update(DOTNET_CLI_HOME=str(profile), DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_GENERATE_ASPNET_CERTIFICATE="false", DOTNET_ADD_GLOBAL_TOOLS_TO_PATH="false", NUGET_PACKAGES=str(directory / "packages"))
            (directory / "global.json").write_text((ROOT / "integrations/roslyn-host/global.json").read_text(encoding="utf-8"), encoding="utf-8")
            project = directory / "source"
            shutil.copytree(ROOT / "tests/managed/LibrarySourceProbe", project)
            # Replace only the reviewed relative source link in a private copy; repository never receives build output.
            source = project / "LibrarySourceProbe.csproj"
            source.write_text(source.read_text(encoding="utf-8").replace("../../../integrations/roslyn-host/LibrarySources.cs", str(ROOT / "integrations/roslyn-host/LibrarySources.cs")), encoding="utf-8")
            configuration = directory / "NuGet.Config"
            configuration.write_text("<configuration><packageSources><clear /></packageSources></configuration>", encoding="utf-8")
            output = directory / "out"
            subprocess.run([str(dotnet), "build", str(source), "--nologo", "--configfile", str(configuration), "-p:FrostManagedAssets=" + str(server.parent), "--output", str(output)], cwd=directory, env=environment, check=True, timeout=120)
            result = subprocess.run([str(dotnet), str(output / "LibrarySourceProbe.dll"), str(directory)], cwd=directory, env=environment, check=True, capture_output=True, text=True, timeout=90)
            self.assertIn("LibrarySourceProbe passed", result.stdout)
if __name__ == "__main__":
    unittest.main()
