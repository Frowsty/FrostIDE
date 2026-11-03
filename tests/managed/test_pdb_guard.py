#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Real .NET portable-PDB/source identity tests, opt-in with private scratch and SDK paths.
from __future__ import annotations

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
DOTNET = os.environ.get("FROST_MANAGED_TEST_DOTNET", "")
SCRATCH = os.environ.get("FROST_MANAGED_TEST_ROOT", "")


def require_marker() -> None:
    # NuGet migration uses the real LocalAppData Known Folder, irrespective of profile redirects.
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
class PdbGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_marker()
        cls.dotnet = Path(DOTNET)
        root = Path(SCRATCH)
        if not cls.dotnet.is_absolute() or not cls.dotnet.is_file() or not root.is_absolute() or not root.is_dir():
            raise ValueError("SDK executable and scratch root must be existing absolute paths")
        cls.work = Path(tempfile.mkdtemp(prefix="frost-pdb-test-", dir=root))
        profile = cls.work / "profile"
        folders = {name: cls.work / name for name in ("nuget", "http", "plugins", "tmp")}
        for folder in (profile, *folders.values()):
            folder.mkdir()
        cls.env = os.environ.copy()
        cls.env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                        "LOCALAPPDATA": str(profile / "local"), "XDG_CONFIG_HOME": str(profile / "config"),
                        "XDG_DATA_HOME": str(profile / "data"), "XDG_CACHE_HOME": str(profile / "cache"),
                        "DOTNET_CLI_HOME": str(profile), "DOTNET_ROOT": str(cls.dotnet.parent),
                        "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                        "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false", "NUGET_PACKAGES": str(folders["nuget"]),
                        "NUGET_HTTP_CACHE_PATH": str(folders["http"]), "NUGET_PLUGINS_CACHE_PATH": str(folders["plugins"]),
                        "TMP": str(folders["tmp"]), "TEMP": str(folders["tmp"]), "TMPDIR": str(folders["tmp"])})
        cls.env.pop("NUGET_PLUGIN_PATHS", None)
        cls.config = cls.work / "NuGet.Config"
        cls.config.write_text("<configuration><packageSources><clear /></packageSources></configuration>", encoding="utf-8")
        fixture = cls.work / "fixture"
        shutil.copytree(REPO / "tests/managed/fixtures/PdbFixture", fixture)
        cls.source = fixture / "Program.cs"
        for name, project in (("fixture", fixture / "PdbFixture.csproj"),
                              ("guard", REPO / "integrations/managed-debug/PdbGuard/PdbGuard.csproj")):
            output = cls.work / (name + "-out")
            obj = cls.work / (name + "-obj")
            subprocess.run([str(cls.dotnet), "build", str(project), "--nologo", "--configfile", str(cls.config),
                            f"-p:BaseIntermediateOutputPath={obj}{os.sep}",
                            f"-p:BaseOutputPath={output}{os.sep}"], cwd=cls.work, env=cls.env, check=True,
                           capture_output=True, text=True)
        cls.pe = cls.work / "fixture-out/Debug/net10.0/PdbFixture.dll"
        cls.pdb = cls.work / "fixture-out/Debug/net10.0/PdbFixture.pdb"
        cls.guard = cls.work / "guard-out/Debug/net10.0/PdbGuard.dll"

    def guard_result(self, pe: Path, pdb: Path, source: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(self.dotnet), str(self.guard), str(pe), str(pdb), str(source)],
                              cwd=self.work, env=self.env, capture_output=True, text=True)

    def test_matching_source_and_assembly_succeed(self) -> None:
        self.assertEqual(self.guard_result(self.pe, self.pdb, self.source).returncode, 0)

    def test_all_documents_are_checked_and_reported(self) -> None:
        result = self.guard_result(self.pe, self.pdb, Path("--all"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.source), json.loads(result.stdout))

    def test_manifest_binds_hashes_to_the_validated_bytes(self) -> None:
        result = self.guard_result(self.pe, self.pdb, Path("--manifest"))
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(result.stdout)
        self.assertEqual(manifest["assemblySha256"], hashlib.sha256(self.pe.read_bytes()).hexdigest())
        self.assertEqual(manifest["pdbSha256"], hashlib.sha256(self.pdb.read_bytes()).hexdigest())
        documents = {entry["path"]: entry["sha256"] for entry in manifest["sources"]}
        self.assertEqual(documents[str(self.source)], hashlib.sha256(self.source.read_bytes()).hexdigest())
        self.assertRegex(manifest["moduleId"], r"^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$")

    def test_other_assembly_and_tampered_pdb_fail(self) -> None:
        wrong_pe = self.guard_result(self.guard, self.pdb, self.source)
        self.assertNotEqual(wrong_pe.returncode, 0)
        self.assertIn("assembly and portable PDB do not match", wrong_pe.stderr)
        altered = bytearray(self.pdb.read_bytes())
        altered[-1] ^= 1
        bad = self.work / "tampered.pdb"
        bad.write_bytes(altered)
        tampered = self.guard_result(self.pe, bad, self.source)
        self.assertNotEqual(tampered.returncode, 0)
        self.assertIn("assembly and portable PDB do not match", tampered.stderr)

    def test_modified_source_is_refused(self) -> None:
        original = self.source.read_bytes()
        changed = original + b"\n// changed by owned test\n"
        try:
            self.source.write_bytes(changed)
            mismatch = self.guard_result(self.pe, self.pdb, self.source)
            self.assertNotEqual(mismatch.returncode, 0)
            self.assertIn("source document checksum does not match", mismatch.stderr)
            self.assertNotEqual(self.guard_result(self.pe, self.pdb, Path("--all")).returncode, 0)
        finally:
            if self.source.read_bytes() == changed:
                self.source.write_bytes(original)
            else:
                raise RuntimeError("fixture changed concurrently; preserving current bytes")
        self.assertEqual(self.guard_result(self.pe, self.pdb, self.source).returncode, 0)

    def test_truncated_pdb_is_refused(self) -> None:
        truncated = self.work / "truncated.pdb"
        truncated.write_bytes(self.pdb.read_bytes()[:64])
        self.assertNotEqual(self.guard_result(self.pe, truncated, self.source).returncode, 0)

    def test_sha1_document_checksum_is_refused(self) -> None:
        output = self.work / "sha1-out"
        obj = self.work / "sha1-obj"
        subprocess.run([str(self.dotnet), "build", str(self.source.parent / "PdbFixture.csproj"), "--nologo",
                        "--configfile", str(self.config), "-p:ChecksumAlgorithm=SHA1",
                        f"-p:BaseIntermediateOutputPath={obj}{os.sep}", f"-p:BaseOutputPath={output}{os.sep}"],
                       cwd=self.work, env=self.env, check=True, capture_output=True, text=True)
        image = output / "Debug/net10.0/PdbFixture.dll"
        symbols = output / "Debug/net10.0/PdbFixture.pdb"
        result = self.guard_result(image, symbols, self.source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source document checksum does not match", result.stderr)

    @unittest.skipUnless(os.name == "nt", "Windows path aliases")
    def test_differently_cased_source_alias_is_refused(self) -> None:
        alias = self.source.with_name("program.cs")
        self.assertNotEqual(self.guard_result(self.pe, self.pdb, alias).returncode, 0)


if __name__ == "__main__":
    if not DOTNET or not SCRATCH:
        print("managed PDB test: SDK or scratch root not configured")
        raise SystemExit(77)
    try:
        require_marker()
    except unittest.SkipTest as reason:
        print(f"managed PDB test: {reason}")
        raise SystemExit(77)
    unittest.main()
