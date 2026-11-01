# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/asset_fetch.py (ADR 0015): the manifest schema, the offline --verify mode
# (including the committed resources/assets.lock.json), and --fetch/--pin against local file://
# archives (the test-only --allow-file-urls), covering archive and member hash checks,
# duplicate, missing and oversized members, extra files, cross-checks, fresh download
# directories, all-or-nothing writes and usage errors, plus the hardening: unpinned hashes
# refused by --fetch, https-only redirects, the download size limit, corrupt member data,
# reserved file names and temporary files, and broken or malformed network replies (through a
# stand-in for asset_fetch._OPENER). No network access.

from __future__ import annotations

import copy
import hashlib
import http.client
import json
import unittest
import urllib.error
import urllib.request
import warnings
import zipfile
from pathlib import Path

from tools_test_support import REPO, TempDirTestCase, run_tool

import asset_fetch


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


MEMBERS = {
    "pkg/fonts/Demo-Regular.ttf": b"\x00\x01\x00\x00synthetic font bytes",
    "pkg/OFL.txt": b"SIL OPEN FONT LICENSE Version 1.1 (synthetic)\n",
    "pkg/README.md": b"not extracted\n",
}
EXTRA = b"Synthetic extra licence file\n"


class FetchTestCase(TempDirTestCase):
    """A temporary repository, a zip served from a file:// URL and a manifest for it."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        (self.repo / "resources").mkdir(parents=True)
        (self.repo / "docs").mkdir()
        (self.repo / "docs" / "adr.txt").write_text("synthetic\n", encoding="utf-8")
        self.served = self.tmp / "served"
        self.served.mkdir()
        self.archive = self.served / "demo-1.0.zip"
        self.make_archive(MEMBERS)
        (self.served / "EXTRA.txt").write_bytes(EXTRA)
        self.downloads = self.tmp / "downloads"
        self.document = {
            "schemaVersion": 1,
            "assets": [{
                "id": "demo", "name": "Demo", "version": "1.0",
                "url": self.archive.as_uri(), "archive": "demo-1.0.zip", "sha256": _sha(self.archive.read_bytes()),
                "crossCheck": "synthetic", "licence": "OFL-1.1", "copyright": ["Copyright 2026 Demo"],
                "reservedFontNames": [], "modified": False, "homepage": "https://example.invalid",
                "dest": "resources/fonts/demo", "licenceFile": "OFL.txt",
                "files": [
                    {"member": "pkg/fonts/Demo-Regular.ttf", "path": "Demo-Regular.ttf",
                     "sha256": _sha(MEMBERS["pkg/fonts/Demo-Regular.ttf"])},
                    {"member": "pkg/OFL.txt", "path": "OFL.txt", "sha256": _sha(MEMBERS["pkg/OFL.txt"])},
                ],
                "extraFiles": [{"url": (self.served / "EXTRA.txt").as_uri(), "path": "EXTRA.txt",
                                "sha256": _sha(EXTRA)}],
                "adr": "docs/adr.txt",
            }],
        }
        self.write_manifest()

    @property
    def asset(self) -> dict:
        return self.document["assets"][0]

    @property
    def dest(self) -> Path:
        return self.repo / "resources" / "fonts" / "demo"

    def make_archive(self, members: dict[str, bytes], duplicate: str | None = None) -> None:
        self.archive.unlink(missing_ok=True)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")  # zipfile warns about the deliberate duplicate
            with zipfile.ZipFile(self.archive, "w") as zf:
                for name, data in members.items():
                    zf.writestr(name, data)
                if duplicate:
                    zf.writestr(duplicate, b"second copy")

    def write_manifest(self) -> None:
        (self.repo / "resources" / "assets.lock.json").write_text(json.dumps(self.document, indent=2),
                                                                   encoding="utf-8")

    def fetch(self, *args: str):
        return run_tool("asset_fetch.py", "--repo", str(self.repo), "--allow-file-urls",
                        "--download-dir", str(self.downloads), *args)


class ManifestSchemaTest(FetchTestCase):
    def assert_rejected(self, mutate, fragment: str) -> None:
        document = copy.deepcopy(self.document)
        mutate(document["assets"][0], document)
        with self.assertRaises(asset_fetch.ManifestError) as caught:
            asset_fetch.validate_manifest(document, "m", allow_file_urls=True)
        self.assertIn(fragment, str(caught.exception))

    def test_valid_manifest(self) -> None:
        assets = asset_fetch.validate_manifest(self.document, "m", allow_file_urls=True)
        self.assertEqual([a["id"] for a in assets], ["demo"])

    def test_file_urls_need_the_test_flag(self) -> None:
        with self.assertRaises(asset_fetch.ManifestError):
            asset_fetch.validate_manifest(self.document, "m")
        result = run_tool("asset_fetch.py", "--repo", str(self.repo))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("url must be https", result.stderr)

    def test_rejections(self) -> None:
        cases = [
            (lambda a, d: d.update(schemaVersion=2), "schemaVersion must be 1"),
            (lambda a, d: d.update(assets=[]), "non-empty list"),
            (lambda a, d: a.pop("licence"), "lacks licence"),
            (lambda a, d: a.update(id="Demo"), "invalid id"),
            (lambda a, d: d["assets"].append(copy.deepcopy(a)), "duplicate id"),
            (lambda a, d: a.update(url="http://example.invalid/x.zip"), "url must be https"),
            (lambda a, d: a.update(archive="dir/x.zip"), "basename of a .zip"),
            (lambda a, d: a.update(sha256="ABC"), "sha256 must be empty"),
            (lambda a, d: a.update(copyright=[]), "copyright must be"),
            (lambda a, d: a.update(modified="no"), "modified must be"),
            (lambda a, d: a.update(dest="../outside"), "dest must be"),
            (lambda a, d: a.update(dest="/abs"), "dest must be"),
            (lambda a, d: a.update(dest=".git/hooks"), "dest must be a directory below resources/"),
            (lambda a, d: a.update(dest="resources"), "dest must be a directory below resources/"),
            (lambda a, d: a.update(dest="resources/fonts/CON"), "dest must be a directory below resources/"),
            (lambda a, d: a.update(dest="resources/fonts./demo"), "dest must be a directory below resources/"),
            (lambda a, d: a["files"][1].update(path="OFL.txt."), "must be a basename"),
            (lambda a, d: a["extraFiles"][0].update(path="nul.txt"), "must be a basename"),
            (lambda a, d: a.update(maxMemberBytes=0), "maxMemberBytes"),
            (lambda a, d: a["files"][0].update(member="../x.ttf"), "without '..'"),
            (lambda a, d: a["files"][0].update(path="sub/x.ttf"), "must be a basename"),
            (lambda a, d: a["files"][0].update(path=".."), "must be a basename"),
            (lambda a, d: a["files"][1].update(path="Demo-Regular.ttf"), "listed twice"),
            (lambda a, d: a["files"][1].update(member="pkg/fonts/Demo-Regular.ttf"), "listed twice"),
            (lambda a, d: a.update(licenceFile="LICENSE"), "licenceFile"),
            (lambda a, d: a["extraFiles"][0].update(url="ftp://x"), "https url"),
            (lambda a, d: a["extraFiles"][0].update(path="OFL.txt"), "listed twice"),
            (lambda a, d: a.update(crossCheckBase="https://example.invalid/no-slash"), "crossCheckBase"),
        ]
        for mutate, fragment in cases:
            with self.subTest(fragment=fragment):
                self.assert_rejected(mutate, fragment)

    def test_format_manifest_round_trips(self) -> None:
        text = asset_fetch.format_manifest(self.document)
        self.assertEqual(json.loads(text), self.document)
        self.assertIn('    {"member": "pkg/OFL.txt"', text.replace("      ", "    "))


class VerifyTest(FetchTestCase):
    def test_committed_assets_verify(self) -> None:
        result = run_tool("asset_fetch.py", "--repo", str(REPO))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_changed_and_unpinned_files(self) -> None:
        self.assertEqual(self.fetch("--fetch").returncode, 0)
        self.assertEqual(self.fetch("--verify").returncode, 0)
        (self.dest / "OFL.txt").write_bytes(b"changed")
        (self.dest / "EXTRA.txt").unlink()
        self.asset["files"][0]["sha256"] = ""
        self.write_manifest()
        result = run_tool("asset_fetch.py", "--repo", str(self.repo), "--allow-file-urls")
        self.assertEqual(result.returncode, 1)
        self.assertIn("resources/fonts/demo/OFL.txt: SHA-256 differs", result.stdout)
        self.assertIn("resources/fonts/demo/EXTRA.txt: missing", result.stdout)
        self.assertIn("resources/fonts/demo/Demo-Regular.ttf: not pinned", result.stdout)

    def test_unpinned_archive_is_a_problem(self) -> None:
        self.assertEqual(self.fetch("--fetch").returncode, 0)
        self.asset["sha256"] = ""
        self.write_manifest()
        result = run_tool("asset_fetch.py", "--repo", str(self.repo), "--allow-file-urls")
        self.assertEqual(result.returncode, 1)
        self.assertIn("demo: archive demo-1.0.zip not pinned", result.stdout)


class FetchTest(FetchTestCase):
    def test_fetch_writes_listed_members_byte_exact(self) -> None:
        result = self.fetch("--fetch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()), ["Demo-Regular.ttf", "EXTRA.txt", "OFL.txt"])
        self.assertEqual((self.dest / "Demo-Regular.ttf").read_bytes(), MEMBERS["pkg/fonts/Demo-Regular.ttf"])
        self.assertEqual((self.dest / "EXTRA.txt").read_bytes(), EXTRA)
        # A second fetch needs a fresh download directory.
        result = self.fetch("--fetch")
        self.assertEqual(result.returncode, 1)
        self.assertIn("is not empty", result.stderr)

    def assert_fails_without_writing(self, fragment: str) -> None:
        result = self.fetch("--fetch")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(fragment, result.stderr)
        self.assertFalse(self.dest.exists(), "nothing is written when verification fails")

    def test_archive_hash_mismatch(self) -> None:
        self.asset["sha256"] = "0" * 64
        self.write_manifest()
        self.assert_fails_without_writing("differs from the pinned")

    def test_unpinned_archive_is_refused(self) -> None:
        self.asset["sha256"] = ""
        self.write_manifest()
        self.assert_fails_without_writing("not pinned")

    def test_unpinned_member_is_refused(self) -> None:
        self.asset["files"][1]["sha256"] = ""
        self.write_manifest()
        self.assert_fails_without_writing("demo: OFL.txt not pinned")

    def test_unpinned_extra_file_is_refused(self) -> None:
        # The archive pin does not cover extra files, so an empty pin would accept any download.
        self.asset["extraFiles"][0]["sha256"] = ""
        self.write_manifest()
        self.assert_fails_without_writing("demo: EXTRA.txt not pinned")

    def test_corrupt_member_data(self) -> None:
        data = b"compressible synthetic font bytes " * 64
        self.archive.unlink()
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("pkg/fonts/Demo-Regular.ttf", data)
            zf.writestr("pkg/OFL.txt", MEMBERS["pkg/OFL.txt"])
        with zipfile.ZipFile(self.archive) as zf:
            info = zf.getinfo("pkg/fonts/Demo-Regular.ttf")
        raw = bytearray(self.archive.read_bytes())
        start = info.header_offset + 30 + len(info.filename.encode()) + len(info.extra)
        for i in range(start + 2, start + info.compress_size - 2):
            raw[i] ^= 0x5A  # garble the deflate stream, keep the headers intact
        self.archive.write_bytes(bytes(raw))
        self.asset["sha256"] = _sha(bytes(raw))
        self.asset["files"][0]["sha256"] = _sha(data)
        self.write_manifest()
        self.assert_fails_without_writing("cannot read the archive")

    def test_foreign_temporary_names_are_left_alone(self) -> None:
        # A file that looks like an earlier run's temporary file is not this run's to delete.
        self.dest.mkdir(parents=True)
        foreign = self.dest / ".OFL.txt.12345.asset-fetch"
        foreign.write_bytes(b"not ours")
        result = self.fetch("--fetch")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(foreign.read_bytes(), b"not ours")
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()),
                         [".OFL.txt.12345.asset-fetch", "Demo-Regular.ttf", "EXTRA.txt", "OFL.txt"])

    def test_member_hash_mismatch(self) -> None:
        self.asset["files"][1]["sha256"] = "1" * 64
        self.write_manifest()
        self.assert_fails_without_writing("pkg/OFL.txt: SHA-256")

    def test_extra_file_hash_mismatch(self) -> None:
        self.asset["extraFiles"][0]["sha256"] = "2" * 64
        self.write_manifest()
        self.assert_fails_without_writing("EXTRA.txt: SHA-256")

    def test_duplicate_member_names(self) -> None:
        self.make_archive(MEMBERS, duplicate="pkg/OFL.txt")
        self.asset["sha256"] = _sha(self.archive.read_bytes())
        self.write_manifest()
        self.assert_fails_without_writing("duplicate member names pkg/OFL.txt")

    def test_missing_member(self) -> None:
        self.asset["files"][0]["member"] = "pkg/fonts/Absent.ttf"
        self.write_manifest()
        self.assert_fails_without_writing("no member pkg/fonts/Absent.ttf")

    def test_oversized_member(self) -> None:
        self.asset["maxMemberBytes"] = 8
        self.write_manifest()
        self.assert_fails_without_writing("over the 8-byte limit")

    def test_not_a_zip(self) -> None:
        self.archive.write_bytes(b"not a zip")
        self.asset["sha256"] = _sha(b"not a zip")
        self.write_manifest()
        self.assert_fails_without_writing("cannot read the archive")

    def test_every_asset_verified_before_any_write(self) -> None:
        second = copy.deepcopy(self.asset)
        second.update(id="second", dest="resources/fonts/second")
        second["files"][0]["sha256"] = "3" * 64
        self.document["assets"].append(second)
        self.write_manifest()
        self.assert_fails_without_writing("second: pkg/fonts/Demo-Regular.ttf")

    def test_download_directory_inside_the_repository(self) -> None:
        result = run_tool("asset_fetch.py", "--repo", str(self.repo), "--allow-file-urls", "--fetch",
                          "--download-dir", str(self.repo / "dl"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("inside the repository", result.stderr)

    def test_only_selects_assets(self) -> None:
        self.assertEqual(self.fetch("--fetch", "--only", "demo").returncode, 0)
        result = self.fetch("--fetch", "--only", "nope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown asset", result.stderr)


class PinTest(FetchTestCase):
    def setUp(self) -> None:
        super().setUp()
        # The cross-check tree mirrors the archive's members (a "tagged repository").
        self.tree = self.tmp / "tree"
        for name, data in MEMBERS.items():
            (self.tree / name).parent.mkdir(parents=True, exist_ok=True)
            (self.tree / name).write_bytes(data)
        self.asset["crossCheckBase"] = self.tree.as_uri() + "/"
        self.asset["sha256"] = ""
        for entry in self.asset["files"] + self.asset["extraFiles"]:
            entry["sha256"] = ""
        self.write_manifest()

    def manifest(self) -> dict:
        return json.loads((self.repo / "resources" / "assets.lock.json").read_text(encoding="utf-8"))

    def test_pin_fills_hashes_after_cross_checks(self) -> None:
        result = self.fetch("--pin", "--only", "demo")
        self.assertEqual(result.returncode, 0, result.stderr)
        pinned = self.manifest()["assets"][0]
        self.assertEqual(pinned["sha256"], _sha(self.archive.read_bytes()))
        self.assertEqual(pinned["files"][0]["sha256"], _sha(MEMBERS["pkg/fonts/Demo-Regular.ttf"]))
        self.assertEqual(pinned["extraFiles"][0]["sha256"], _sha(EXTRA))
        self.assertIn("matches", result.stderr)
        self.assertFalse(self.dest.exists(), "--pin never writes dest")

    def test_cross_check_mismatch_leaves_the_manifest_alone(self) -> None:
        (self.tree / "pkg" / "OFL.txt").write_bytes(b"a different licence file")
        before = (self.repo / "resources" / "assets.lock.json").read_bytes()
        result = self.fetch("--pin", "--only", "demo")
        self.assertEqual(result.returncode, 1)
        self.assertIn("differ", result.stderr)
        self.assertEqual((self.repo / "resources" / "assets.lock.json").read_bytes(), before)

    def test_pin_and_list_need_one_asset(self) -> None:
        self.assertEqual(self.fetch("--pin").returncode, 2)
        self.assertEqual(self.fetch("--list").returncode, 2)

    def test_list_prints_members(self) -> None:
        result = self.fetch("--list", "--only", "demo")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("pkg/README.md", result.stdout)


class HelperTest(TempDirTestCase):
    def test_basenames(self) -> None:
        for good in ("a.ttf", "LICENSE", "x-y_z.svg", "CONSOLE.txt", "nullable.svg", "com10.txt", "auxiliary"):
            self.assertTrue(asset_fetch.is_basename(good), good)
        for bad in ("", ".", "..", "a/b", "a\\b", "C:x", "a\0b", None, 3, "LICENSE.", " a", "a ", "CON",
                    "nul.txt", "Com1", "LPT9.log", "aux.tar.gz"):
            self.assertFalse(asset_fetch.is_basename(bad), repr(bad))

    def test_redirects_only_to_https(self) -> None:
        handler = asset_fetch.HttpsOnlyRedirects()
        request = urllib.request.Request("https://example.invalid/a.zip")
        for target in ("http://example.invalid/a.zip", "ftp://example.invalid/a.zip", "file:///etc/passwd"):
            with self.subTest(target=target), self.assertRaises(urllib.error.URLError) as caught:
                handler.redirect_request(request, None, 302, "Found", {}, target)
            self.assertIn("non-https", str(caught.exception))
        followed = handler.redirect_request(request, None, 302, "Found", {}, "https://cdn.example.invalid/a.zip")
        self.assertEqual(followed.full_url, "https://cdn.example.invalid/a.zip")

    def test_download_size_limit(self) -> None:
        source = self.tmp / "big.bin"
        source.write_bytes(b"x" * 64)
        with self.assertRaises(asset_fetch.FetchError) as caught:
            asset_fetch.download(source.as_uri(), self.tmp / "copy.bin", limit=63)
        self.assertIn("exceeds the 63-byte limit", str(caught.exception))
        self.assertEqual(asset_fetch.download(source.as_uri(), self.tmp / "copy2.bin", limit=64), _sha(b"x" * 64))

    def _with_reply(self, read) -> None:
        """Makes asset_fetch._OPENER answer every request with a response whose read() is `read`."""

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def read(self, *_):
                return read()

        class Opener:
            def open(self, *_args, **_kwargs):
                return Response()

        saved = asset_fetch._OPENER
        asset_fetch._OPENER = Opener()
        self.addCleanup(setattr, asset_fetch, "_OPENER", saved)

    def test_broken_replies_are_fetch_errors(self) -> None:
        # http.client's errors are not OSError: a truncated or dropped reply must still be a
        # FetchError (exit status 1), never a traceback.
        url = "https://github.com/o/r/releases/download/v1/a.zip"
        for error in (http.client.IncompleteRead(b"partial", 10), http.client.RemoteDisconnected("closed"),
                      http.client.BadStatusLine("HTTP/9")):
            with self.subTest(error=type(error).__name__):

                def fail(error=error):
                    raise error

                self._with_reply(fail)
                with self.assertRaises(asset_fetch.FetchError):
                    asset_fetch.download(url, self.tmp / f"{type(error).__name__}.bin")
                with self.assertRaises(asset_fetch.FetchError):
                    asset_fetch.github_digest(url)

    def test_github_reply_shapes(self) -> None:
        url = "https://github.com/o/r/releases/download/v1/a.zip"
        for reply in (b"[]", b"3", b"null", b'"release"', b"{}", b'{"assets": 3}', b'{"assets": {"a.zip": 1}}',
                      b'{"assets": [3]}', b'{"assets": [{"name": "b.zip"}, "a.zip"]}'):
            with self.subTest(reply=reply):
                self._with_reply(lambda reply=reply: reply)
                with self.assertRaises(asset_fetch.FetchError) as caught:
                    asset_fetch.github_digest(url)
                self.assertIn("o/r@v1", str(caught.exception))
        digest = "ab" * 32
        good = json.dumps({"assets": [{"name": "b.zip"}, {"name": "a.zip", "digest": f"sha256:{digest.upper()}"}]})
        self._with_reply(lambda: good.encode())
        self.assertEqual(asset_fetch.github_digest(url), digest)
        self._with_reply(lambda: b'{"assets": [{"name": "a.zip"}]}')
        self.assertIsNone(asset_fetch.github_digest(url), "an asset without a digest")
        self._with_reply(lambda: b'{"assets": []}')
        with self.assertRaises(asset_fetch.FetchError) as caught:
            asset_fetch.github_digest(url)
        self.assertIn("has no asset a.zip", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
