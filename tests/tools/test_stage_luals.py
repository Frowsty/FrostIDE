# SPDX-License-Identifier: Apache-2.0
# Verifies pinned LuaLS ZIP delivery with generated SDK metadata and a vendor meta directory.

import hashlib
import json
import unittest
from unittest.mock import patch
import zipfile

from tools_test_support import TempDirTestCase

from stage_luals import stage_luals


class StageLuaLsTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        self.lock = self.repo / "sdk/sdk.lock.json"
        self.lock.parent.mkdir(parents=True)
        self.sdk = self.tmp / "sdk"
        self.archive = self.sdk / "_downloads/luals.zip"
        self.archive.parent.mkdir(parents=True)
        self.members = {
            "bin/lua-language-server.exe": b"vendor executable",
            "bin/runtime.dll": b"vendor runtime",
            "LICENSE": b"vendor notice",
            "main.lua": b"vendor entrypoint",
            "meta/template/math.lua": b"vendor meta",
        }

    def write_archive(self) -> None:
        with zipfile.ZipFile(self.archive, "w") as bundle:
            for name, content in self.members.items():
                bundle.writestr(name, content)
        pin = {"file": self.archive.name, "sha256": hashlib.sha256(self.archive.read_bytes()).hexdigest()}
        self.lock.write_text(json.dumps({"windows": {"luals": pin}}), encoding="utf-8")

    def test_only_exact_vendor_members_are_bundled(self) -> None:
        self.write_archive()
        installed = self.sdk / "luals-3.19.1"
        generated = installed / "meta/Lua 5.4 en-us utf8/math.lua"
        generated.parent.mkdir(parents=True)
        generated.write_bytes(b"generated definitions")
        (installed / "LICENSE").write_bytes(b"locally modified notice")
        destination = self.tmp / "stage"

        stage_luals(self.repo, self.sdk, "windows", None)
        self.assertFalse(destination.exists())
        stage_luals(self.repo, self.sdk, "windows", destination)

        actual = {file.relative_to(destination).as_posix(): file.read_bytes()
                  for file in destination.rglob("*") if file.is_file()}
        self.assertEqual(actual, self.members)
        self.assertEqual(generated.read_bytes(), b"generated definitions")
        self.assertEqual((installed / "LICENSE").read_bytes(), b"locally modified notice")

    def test_unsafe_and_colliding_archive_paths_are_refused_before_writing(self) -> None:
        destination = self.tmp / "stage"
        for path, message in (("../escape", "unsafe LuaLS archive path"),
                              ("META/template/math.lua", "duplicate LuaLS archive path"),
                              ("main.lua/child", "archive file is also a parent directory")):
            with self.subTest(path=path):
                self.members[path] = b"invalid archive entry"
                self.write_archive()
                with self.assertRaisesRegex(ValueError, message):
                    stage_luals(self.repo, self.sdk, "windows", destination)
                self.assertFalse(destination.exists())
                del self.members[path]

    def test_copy_failure_leaves_no_partial_stage_and_existing_stage_is_preserved(self) -> None:
        self.write_archive()
        destination = self.tmp / "stage"
        with patch("stage_luals.shutil.copyfileobj", side_effect=OSError("injected copy failure")):
            with self.assertRaisesRegex(OSError, "injected copy failure"):
                stage_luals(self.repo, self.sdk, "windows", destination)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.tmp.glob(".luals-stage-*")), [])

        destination.mkdir()
        marker = destination / "marker"
        marker.write_bytes(b"existing stage")
        with self.assertRaisesRegex(FileExistsError, "LuaLS destination already exists"):
            stage_luals(self.repo, self.sdk, "windows", destination)
        self.assertEqual(list(destination.iterdir()), [marker])
        self.assertEqual(marker.read_bytes(), b"existing stage")


if __name__ == "__main__":
    unittest.main()
