# SPDX-License-Identifier: Apache-2.0
# Tests package digest admission and safe extraction for the Lua coroutine provider.
"""Tests for local Lua debugger package admission and extraction."""

from __future__ import annotations

import gzip
import hashlib
import io
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import build_lua_debug_coroutines as provider


def archive(*files: tuple[str, bytes, int]) -> bytes:
    """Small ZIP inputs for testing the installer, not an adapter substitute."""
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as output:
        for name, body, mode in files:
            entry = zipfile.ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = mode << 16
            output.writestr(entry, body)
    return stream.getvalue()


class LuaCoroutineProviderTest(unittest.TestCase):
    def test_digest_covers_vsix_payload_inside_optional_transport_wrapper(self):
        data = archive(("extension/LICENSE.txt", b"example notice", stat.S_IFREG | 0o644))
        digest = hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory() as root:
            input_file = Path(root) / "sample.vsix"
            for representation in (data, gzip.compress(data)):
                with self.subTest(gzip=representation != data):
                    input_file.write_bytes(representation)
                    self.assertEqual(provider.checked_archive(input_file, digest), data)
            input_file.write_bytes(data + b"modified")
            with self.assertRaisesRegex(ValueError, "pinned package digest"):
                provider.checked_archive(input_file, digest)

    def test_extension_extraction_preserves_bytes_and_executable_mode(self):
        payload = b"binary bytes 0x03\x00"
        data = archive(("extension/bin/lua-debug", payload, stat.S_IFREG | 0o755),
                       ("extension/script/worker.lua", b"return 1\r\n", stat.S_IFREG | 0o644))
        with tempfile.TemporaryDirectory() as root:
            target = Path(root)
            provider.extract_extension(data, target)
            binary = target / "bin" / "lua-debug"
            self.assertEqual(binary.read_bytes(), payload)
            self.assertEqual((target / "script" / "worker.lua").read_bytes(), b"return 1\r\n")
            if sys.platform != "win32":
                self.assertTrue(binary.stat().st_mode & stat.S_IXUSR)

    def test_unsafe_members_are_rejected_before_any_extraction(self):
        valid = ("extension/valid.txt", b"unchanged", stat.S_IFREG | 0o644)
        invalid = (
            ("extension/../escape", b"", stat.S_IFREG | 0o644),
            ("extension/link", b"elsewhere", stat.S_IFLNK | 0o777),
            ("extension/VALID.TXT", b"collision", stat.S_IFREG | 0o644),
        )
        for entry in invalid:
            with self.subTest(name=entry[0]), tempfile.TemporaryDirectory() as root:
                target = Path(root)
                with self.assertRaises(ValueError):
                    provider.extract_extension(archive(valid, entry), target)
                self.assertEqual(list(target.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
