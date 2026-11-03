# SPDX-License-Identifier: Apache-2.0
# Verifies source pinning and safe license extraction without network or SDK access.

from __future__ import annotations

import io
import tarfile
import unittest

from tools_test_support import TempDirTestCase
import package_notices as notices


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


if __name__ == "__main__":
    unittest.main()
