# SPDX-License-Identifier: Apache-2.0
# Verifies source pinning and safe license extraction without network or SDK access.

from __future__ import annotations

import io
import hashlib
import json
from pathlib import Path
import tarfile
import unittest
from unittest.mock import patch

from tools_test_support import TempDirTestCase
import package_notices as notices
import managed_stage_inventory as managed


class SourceNoticesTest(TempDirTestCase):
    def archive(self, extra: tuple[str, bytes] | None = None):
        path = self.tmp / "qt.tar.xz"
        entries = [("LGPL-3.0-only.txt", b"lgpl fixture"), ("GPL-3.0-only.txt", b"gpl fixture")]
        if extra:
            entries.append(extra)
        with tarfile.open(path, "w:xz") as output:
            for name, data in entries:
                member = tarfile.TarInfo("qt/LICENSES/" + name)
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))
        return path, {"directory": "qt", "file": "qt.tar.xz", "sha256": notices.digest(path)}

    def test_preserves_source_and_license_bytes(self):
        archive, pin = self.archive()
        stage = self.tmp / "stage"
        paths = notices.copy_qt_source(archive, stage, pin)
        self.assertEqual(len(paths), 3)
        self.assertEqual((stage / "sources/qt.tar.xz").read_bytes(), archive.read_bytes())
        self.assertEqual((stage / "licenses/qtbase/LGPL-3.0-only.txt").read_bytes(), b"lgpl fixture")

    def test_wrong_hash_refuses_before_writing(self):
        archive, pin = self.archive()
        pin["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            notices.copy_qt_source(archive, self.tmp / "stage", pin)
        self.assertFalse((self.tmp / "stage").exists())

    def test_traversal_refused_without_extracting(self):
        archive, pin = self.archive(("../../outside.txt", b"bad"))
        with self.assertRaisesRegex(ValueError, "invalid Qt license member"):
            notices.copy_qt_source(archive, self.tmp / "stage", pin)
        self.assertFalse((self.tmp / "stage").exists())
        self.assertFalse((self.tmp / "outside.txt").exists())

    def test_duplicate_license_refused(self):
        archive, pin = self.archive(("GPL-3.0-only.txt", b"duplicate"))
        with self.assertRaisesRegex(ValueError, "invalid Qt license member"):
            notices.copy_qt_source(archive, self.tmp / "stage", pin)

    def test_installed_packages_are_target_scoped_and_deduplicated(self):
        installed = self.tmp / "installed"
        (installed / "vcpkg").mkdir(parents=True)
        (installed / "vcpkg/status").write_text(
            "Package: one\nArchitecture: target\nStatus: install ok installed\n\n"
            "Package: host-tool\nArchitecture: host\nStatus: install ok installed\n\n"
            "Package: one\nFeature: extra\nArchitecture: target\nStatus: install ok installed\n\n"
            "Package: removed\nArchitecture: target\nStatus: deinstall ok not-installed\n", encoding="utf-8")
        self.assertEqual(notices.installed_ports(installed, "target"), ["one"])

    def test_spdx_records_local_notice_path_and_checksums_with_runtime_role(self):
        repo = Path(__file__).resolve().parents[2]
        stage = self.tmp / "stage"
        directory = stage / "roslyn/FrostIDE-Notices"
        directory.mkdir(parents=True)
        notice = directory / "example.txt"
        notice.write_bytes(b"actual staged notice bytes")
        staged_path = notice.relative_to(stage).as_posix()
        component = {"name": "FrostRoslynHost", "version": "1.0.0", "license": "Apache-2.0",
                     "role": "Owned Roslyn language host", "source": "integrations/roslyn-host",
                     "files": [], "notices": [staged_path]}
        report = notices.licensecheck.Report()
        notices.licensecheck.check_sdk_managed(repo, report)
        self.assertEqual(report.violations, [])
        with patch.object(managed, "verified_components", return_value=([component], {})):
            paths, meta = notices.managed_inventory(repo, stage, report)
        self.assertEqual(paths, [notice])
        self.assertEqual(meta["managedComponents"][0]["notices"],
                         [{"path": staged_path, "sha256": notices.digest(notice)}])
        inventory = self.tmp / "stage.spdx.json"
        notices.licensecheck.write_sbom(repo, report, inventory)
        sbom = json.loads(inventory.read_text(encoding="utf-8"))
        packages = {item["name"]: item for item in sbom["packages"]}
        files = {item["fileName"]: item for item in sbom["files"]}
        file = files["./" + staged_path]
        self.assertIn({"algorithm": "SHA256", "checksumValue": notices.digest(notice)}, file["checksums"])
        file_sha1 = hashlib.sha1(notice.read_bytes(), usedforsecurity=False).hexdigest()
        self.assertTrue(packages["FrostRoslynHost"]["filesAnalyzed"])
        self.assertFalse(packages["csharp-ls"]["filesAnalyzed"])
        self.assertEqual(packages["FrostRoslynHost"]["packageVerificationCode"]["packageVerificationCodeValue"],
                         hashlib.sha1(file_sha1.encode("ascii"), usedforsecurity=False).hexdigest())
        edges = {(item["spdxElementId"], item["relationshipType"], item["relatedSpdxElement"])
                 for item in sbom["relationships"]}
        host, carrier = (packages[name]["SPDXID"] for name in ("FrostRoslynHost", "csharp-ls"))
        self.assertIn(("SPDXRef-FrostIDE", "CONTAINS", host), edges)
        self.assertIn((host, "CONTAINS", file["SPDXID"]), edges)
        self.assertIn((carrier, "BUILD_DEPENDENCY_OF", "SPDXRef-FrostIDE"), edges)
        self.assertNotIn(("SPDXRef-FrostIDE", "CONTAINS", carrier), edges)

    def test_exact_stage_receipt_rejects_changed_managed_payload(self):
        stage = self.tmp / "stage"
        roslyn = stage / managed.HOST
        debug = stage / managed.DEBUG
        for path in (roslyn, debug):
            path.mkdir(parents=True)
        host_manifest = roslyn / managed.roslyn_host.RECORD
        host_manifest.write_text('{"files": {}}', encoding="utf-8")
        debug_manifest = debug / "manifest.json"
        debug_manifest.write_bytes(b"the debugger manifest")
        for name in ("LICENSE", "NOTICE"):
            (stage / name).write_text(name, encoding="utf-8")
        files = {p.relative_to(stage).as_posix(): notices.digest(p) for p in
                 (host_manifest, debug_manifest, stage / "LICENSE", stage / "NOTICE")}
        receipt = self.tmp / "assemble-evidence.json"
        receipt.write_text(json.dumps({"schema": 1, "kind": "unsigned-release-packaging",
                                       "sourceCommit": "1" * 40, "files": files,
                                       "managedDebugManifestSha256": files[managed.DEBUG_MANIFEST]}), encoding="utf-8")
        host_manifest.write_text('{"files": {}, "new": true}', encoding="utf-8")
        with (patch.object(managed.roslyn_host, "verify_payload"),
              patch.object(managed.install_payload, "verify", return_value={"files": {}})):
            with self.assertRaisesRegex(ValueError, "Windows stage receipt digest differs"):
                managed.verified_components(self.tmp, stage, receipt)


if __name__ == "__main__":
    unittest.main()
