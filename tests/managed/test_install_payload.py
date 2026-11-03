#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Exercise the managed-debug manifest gate and non-overwriting installation in private scratch.
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SCRATCH = os.environ.get("FROST_MANAGED_TEST_ROOT", "")
STAGE = os.environ.get("FROST_MANAGED_TEST_ADAPTER_STAGE", "")
HELPER = os.environ.get("FROST_MANAGED_TEST_HELPER_STAGE", "")


@unittest.skipUnless(SCRATCH and STAGE and HELPER, "set private root and the audited scratch adapter/helper stages")
class InstallPayloadTest(unittest.TestCase):
    def setUp(self) -> None:
        root, stage, helper = Path(SCRATCH), Path(STAGE), Path(HELPER)
        if not all(path.is_absolute() and path.is_dir() for path in (root, stage, helper)):
            raise ValueError("test paths must be existing absolute directories")
        self.work = Path(tempfile.mkdtemp(prefix="managed-payload-test-", dir=root))
        self.payload = self.work / "payload"
        self.payload.mkdir()
        shutil.copytree(stage, self.payload / "adapter")
        shutil.copytree(helper, self.payload / "PdbGuard")
        files = {path.relative_to(self.payload).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in self.payload.rglob("*") if path.is_file()}
        record = {"schema": 1, "purpose": "candidate-source-build", "rid": "win-x64" if os.name == "nt" else "linux-x64",
                  "runtimeCommit": "6aa72daa24f66c01c3d7b6a32519e9d21396deec",
                  "netcoredbgCommit": "9744e1f051866215611b8440c638042aa2aa2f72",
                  "patchSha256": "6379dd5d3fd68f4bb2cbc5ba9252e43bc8650f84bb22c9790968a4396b87a50b",
                  "helperSourceSha256": "cc562d800e8cdbd095827b1b17c1e8c863632490733a2c03bad1158cb2af58dd",
                  "sdkVersion": "10.0.403", "files": files,
                  "toolSha256": {str(i): "a" * 64 for i in range(5)},
                  "recipeSha256": {str(i): "a" * 64 for i in range(11)}}
        if os.name == "nt":
            record["systemImports"] = {"netcoredbg.exe": ["kernel32.dll", "oleaut32.dll", "wsock32.dll"],
                "dbgshim.dll": sorted(("kernel32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "version.dll",
                    "api-ms-win-crt-stdio-l1-1-0.dll", "api-ms-win-crt-string-l1-1-0.dll",
                    "api-ms-win-crt-runtime-l1-1-0.dll", "api-ms-win-crt-heap-l1-1-0.dll"))}
        else:
            record["systemImports"] = {"netcoredbg": sorted(("libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6")),
                "libdbgshim.so": sorted(("ld-linux-x86-64.so.2", "libc.so.6", "libgcc_s.so.1", "libm.so.6",
                    "libstdc++.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2"))}
        manifest = self.payload / "manifest.json"
        manifest.write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
        self.manifest = hashlib.sha256(manifest.read_bytes()).hexdigest()
        self.app = self.work / "app"
        self.app.mkdir()

    def install(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(REPO / "tools/managed-debug/install_payload.py"),
                               "--payload", str(self.payload), "--manifest-sha256", self.manifest,
                               "--application-dir", str(self.app)], capture_output=True, text=True)

    def test_installs_only_verified_files_and_refuses_overwrite(self) -> None:
        first = self.install()
        self.assertEqual(first.returncode, 0, first.stderr)
        installed = self.app / "FrostManagedDebug"
        self.assertTrue((installed / "PdbGuard/PdbGuard.dll").is_file())
        self.assertEqual(hashlib.sha256((installed / "manifest.json").read_bytes()).hexdigest(), self.manifest)
        self.assertNotEqual(self.install().returncode, 0)
        self.assertTrue((installed / "PdbGuard/PdbGuard.dll").is_file())

    def test_tampered_or_extra_file_is_rejected_before_install(self) -> None:
        target = self.payload / "PdbGuard/PdbGuard.dll"
        target.write_bytes(target.read_bytes() + b"tampered")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertFalse((self.app / "FrostManagedDebug").exists())
        shutil.copyfile(Path(HELPER) / "PdbGuard.dll", target)
        (self.payload / "unexpected.txt").write_text("not in manifest", encoding="utf-8")
        self.assertNotEqual(self.install().returncode, 0)
        self.assertFalse((self.app / "FrostManagedDebug").exists())

    def test_manifest_with_unreviewed_system_import_is_rejected(self) -> None:
        manifest = self.payload / "manifest.json"
        record = json.loads(manifest.read_text(encoding="utf-8"))
        next(iter(record["systemImports"].values())).append("unexpected-system-library")
        manifest.write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
        self.manifest = hashlib.sha256(manifest.read_bytes()).hexdigest()
        self.assertNotEqual(self.install().returncode, 0)
        self.assertFalse((self.app / "FrostManagedDebug").exists())


if __name__ == "__main__":
    unittest.main()
