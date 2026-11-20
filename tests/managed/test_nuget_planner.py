# SPDX-License-Identifier: Apache-2.0
"""Mandatory read-only Microsoft.Build package proposal acceptance. Missing SDK fails; no skip."""
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]


class NuGetPlanner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dotnet = Path(os.environ.get("FROST_MANAGED_TEST_DOTNET", ""))
        if not dotnet.is_absolute() or not dotnet.is_file():
            raise AssertionError("Required configured .NET 10 SDK executable is missing")
        parent = Path(os.environ.get("FROST_MANAGED_TEST_ROOT", ""))
        if not parent.is_absolute() or not parent.is_dir():
            raise AssertionError("Required private managed test build root is missing")
        cls.temporary = tempfile.TemporaryDirectory(prefix="nuget-planner-test-", dir=parent)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.dotnet = dotnet
        cls.environment = dict(os.environ)
        for name in ("DOTNET_CLI_HOME", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "NUGET_PACKAGES",
                     "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH"):
            directory = cls.root / "profile" / name
            directory.mkdir(parents=True)
            cls.environment[name] = str(directory)
        cls.environment.update(DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
                               DOTNET_GENERATE_ASPNET_CERTIFICATE="0", DOTNET_ADD_GLOBAL_TOOLS_TO_PATH="0")
        version = subprocess.run([str(dotnet), "--version"], cwd=cls.root, env=cls.environment,
                                 capture_output=True, text=True, check=True).stdout.strip()
        sdks = subprocess.run([str(dotnet), "--list-sdks"], cwd=cls.root, env=cls.environment,
                              capture_output=True, text=True, check=True).stdout
        selected = [Path(match.group(1)) / version for line in sdks.splitlines()
                    if line.startswith(version + " ") and (match := re.search(r"\[(.+)\]", line))]
        if len(selected) != 1:
            raise AssertionError("SDK enumeration did not identify the selected SDK directory")
        cls.sdk = selected[0]
        cls.output = cls.root / "tool"
        subprocess.run([os.sys.executable, str(REPO / "tools/managed_nuget.py"), "--dotnet", str(dotnet),
                        "--output", str(cls.output), "--work", str(cls.root / "build")],
                       cwd=cls.root, env=cls.environment, check=True, capture_output=True, text=True)

    def project(self, name, contents):
        directory = self.root / name
        directory.mkdir()
        project = directory / "App.csproj"
        project.write_bytes(contents)
        return directory, project

    def plan(self, root, project, operation, **values):
        request = dict(trusted=True, root=str(root), project=str(project), package="Fixture.Package",
                       operation=operation, version="1.2.3", configuration="Debug", targetFramework="net10.0", **values)
        result = subprocess.run([str(self.dotnet), str(self.output / "NuGetPlanner.dll"), "--sdk", str(self.sdk)],
                                input=json.dumps(request) + "\n", cwd=self.root, env=self.environment,
                                capture_output=True, text=True, timeout=30)
        return result.returncode, json.loads(result.stdout)

    def test_direct_images_are_exact_and_do_not_write_source(self):
        original = b'\xef\xbb\xbf<Project Sdk="Microsoft.NET.Sdk">\r\n<!-- keep -->\r\n</Project>\r\n'
        root, project = self.project("direct", original)
        code, plan = self.plan(root, project, "install")
        self.assertEqual(code, 0, plan)
        self.assertEqual(project.read_bytes(), original)
        self.assertEqual(len(plan["files"]), 1)
        row = plan["files"][0]
        self.assertEqual(base64.b64decode(row["beforeBase64"], validate=True), original)
        after = base64.b64decode(row["afterBase64"], validate=True)
        self.assertIn(b"<!-- keep -->", after)
        document = ET.fromstring(after)
        package = document.find("./ItemGroup/PackageReference")
        self.assertEqual(package.attrib["Include"], "Fixture.Package")
        self.assertEqual(package.attrib["Version"], "1.2.3")

    def test_central_install_and_remove_keep_shared_ownership(self):
        root, project = self.project("central", b'<Project Sdk="Microsoft.NET.Sdk"/>')
        central = root / "Directory.Packages.props"
        original = b'<Project><PropertyGroup><ManagePackageVersionsCentrally>true</ManagePackageVersionsCentrally></PropertyGroup><ItemGroup><PackageVersion Include="Other" Version="9.0.0"/></ItemGroup></Project>'
        central.write_bytes(original)
        code, plan = self.plan(root, project, "install", centralFile=str(central))
        self.assertEqual(code, 0, plan)
        self.assertEqual(len(plan["files"]), 2)
        self.assertEqual(central.read_bytes(), original)
        for row in plan["files"]:
            if row["path"] == str(project):
                document = ET.fromstring(base64.b64decode(row["afterBase64"], validate=True))
                self.assertNotIn("Version", document.find("./ItemGroup/PackageReference").attrib)
        project.write_text('<Project Sdk="Microsoft.NET.Sdk"><ItemGroup><PackageReference Include="Fixture.Package"/></ItemGroup></Project>', encoding="utf-8")
        code, removed = self.plan(root, project, "remove", centralFile=str(central))
        self.assertEqual(code, 0, removed)
        self.assertEqual([row["path"] for row in removed["files"]], [str(project)])
        self.assertEqual(central.read_bytes(), original)

    def test_conditional_expression_and_outside_ownership_refuse(self):
        root, project = self.project("conditional", b'<Project><ItemGroup Condition="true"><PackageReference Include="Fixture.Package" Version="1.0.0"/></ItemGroup></Project>')
        self.assertNotEqual(self.plan(root, project, "update")[0], 0)
        project.write_text('<Project><ItemGroup><PackageReference Include="Fixture.Package" Version="$(SharedVersion)"/></ItemGroup></Project>', encoding="utf-8")
        self.assertNotEqual(self.plan(root, project, "update")[0], 0)
        self.assertNotEqual(self.plan(root, self.root / "Other.csproj", "install")[0], 0)
        self.assertNotEqual(self.plan(Path("relative"), project, "install")[0], 0)
        if os.name == "nt":
            drive = "Z" if root.drive[:1].upper() != "Z" else "Y"
            self.assertNotEqual(self.plan(root, Path(drive + ":/outside/App.csproj"), "install")[0], 0)


if __name__ == "__main__":
    unittest.main()
