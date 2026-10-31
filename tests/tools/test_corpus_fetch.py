# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/corpus_fetch.py and tests/corpus/manifest.json. Every fetch goes to a
# local bare repository the test creates; the real corpora are never fetched here.

from __future__ import annotations

import json
import unittest
from pathlib import Path

from tools_test_support import REPO, TempDirTestCase, clean_env, git, run_tool

import corpus_fetch


class ManifestTest(unittest.TestCase):
    def test_repository_manifest_is_valid_and_pinned(self) -> None:
        corpora = {c["id"]: c for c in corpus_fetch.load_manifest(REPO / corpus_fetch.MANIFEST_FILE)}
        self.assertEqual(sorted(corpora), ["R2a", "R2b", "R3"])
        self.assertEqual(corpora["R3"]["tag"], "llvmorg-23.1.4")
        self.assertEqual(corpora["R3"]["sha"], "7a941dfb2877a0b8cf30323fefa85ad019e0b0f5")
        self.assertEqual(corpora["R3"]["licence"], "Apache-2.0 WITH LLVM-exception")
        self.assertEqual(corpora["R2a"]["licence"], "GPL-3.0-or-later")
        self.assertEqual(corpora["R2b"]["licence"], "MIT")
        for entry in corpora.values():
            self.assertTrue(entry["url"].startswith("https://github.com/"), entry["url"])

    def test_invalid_manifests_are_rejected(self) -> None:
        bad = [
            {"corpora": []},
            {"corpora": [{"id": "X", "sha": "abc"}]},
            {"corpora": [dict.fromkeys(corpus_fetch.REQUIRED_FIELDS, "x") | {"sha": "A" * 40}]},
        ]
        for document in bad:
            path = Path(self._tmpfile(json.dumps(document)))
            with self.assertRaises(corpus_fetch.ManifestError):
                corpus_fetch.load_manifest(path)

    def _tmpfile(self, text: str) -> str:
        import tempfile
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        return handle.name


class FetchTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        work = self.tmp / "work"
        work.mkdir()
        git("init", "-q", cwd=work)
        (work / "a.txt").write_text("first\n", encoding="utf-8")
        git("add", "a.txt", cwd=work)
        git("commit", "-q", "-m", "first", cwd=work)
        self.first = git("rev-parse", "HEAD", cwd=work).strip()
        git("tag", "-a", "v1", "-m", "release 1", cwd=work)
        (work / "a.txt").write_text("second\n", encoding="utf-8")
        git("commit", "-q", "-am", "second", cwd=work)
        self.second = git("rev-parse", "HEAD", cwd=work).strip()
        self.bare = self.tmp / "remote.git"
        git("clone", "-q", "--bare", str(work), str(self.bare), cwd=self.tmp)
        self.dest = self.tmp / "corpus"

    def _manifest(self, sha: str, tag: str = "v1") -> Path:
        entry = {"id": "T1", "name": "fixture", "url": self.bare.as_uri(), "tag": tag, "sha": sha,
                 "licence": "MIT", "approx_size_mb": 1, "prerequisites": "none", "purpose": "tests"}
        path = self.tmp / "manifest.json"
        path.write_text(json.dumps({"corpora": [entry]}, indent=2), encoding="utf-8")
        return path

    def _run(self, manifest: Path, *args: str):
        return run_tool("corpus_fetch.py", "--manifest", str(manifest), "--dest", str(self.dest), *args,
                        env=clean_env())

    def test_fetches_a_commit_that_is_not_a_branch_tip_and_verifies_head(self) -> None:
        result = self._run(self._manifest(self.first), "--only", "T1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git("rev-parse", "HEAD", cwd=self.dest / "T1").strip(), self.first)
        self.assertEqual((self.dest / "T1" / "a.txt").read_text(encoding="utf-8"), "first\n")
        self.assertEqual(git("rev-parse", "--is-shallow-repository", cwd=self.dest / "T1").strip(), "true")
        again = self._run(self._manifest(self.first), "--only", "T1")
        self.assertEqual(again.returncode, 0)
        self.assertIn("up to date", again.stdout)

    def test_moves_an_existing_checkout_to_a_new_pin(self) -> None:
        self.assertEqual(self._run(self._manifest(self.first), "--all").returncode, 0)
        result = self._run(self._manifest(self.second), "--all")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(git("rev-parse", "HEAD", cwd=self.dest / "T1").strip(), self.second)

    def test_head_mismatch_after_tag_fallback_fails(self) -> None:
        missing = "0123456789abcdef0123456789abcdef01234567"
        result = self._run(self._manifest(missing), "--only", "T1")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("does not match the pinned", result.stdout)
        self.assertRegex(result.stdout, r"manifest\.json:\d+: ")

    def test_list_is_the_default_and_fetches_nothing(self) -> None:
        result = self._run(self._manifest(self.first))
        self.assertEqual(result.returncode, 0)
        self.assertIn("T1", result.stdout)
        self.assertIn("missing", result.stdout)
        self.assertFalse(self.dest.exists())

    def test_unknown_id_is_a_usage_error(self) -> None:
        self.assertEqual(self._run(self._manifest(self.first), "--only", "nope").returncode, 2)

    def test_refuses_to_overwrite_a_non_git_directory(self) -> None:
        (self.dest / "T1").mkdir(parents=True)
        (self.dest / "T1" / "keep.txt").write_text("mine\n", encoding="utf-8")
        result = self._run(self._manifest(self.first), "--only", "T1")
        self.assertEqual(result.returncode, 1)
        self.assertTrue((self.dest / "T1" / "keep.txt").exists())


if __name__ == "__main__":
    unittest.main()
