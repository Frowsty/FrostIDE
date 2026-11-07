# SPDX-License-Identifier: Apache-2.0
#
# Tests for tools/corpora.py, tests/corpus/corpora.lock.json and the -Corpus tier of the verify
# scripts. Every clone comes from a local repository the test creates in a temporary directory;
# the real corpora are never fetched here, and no developer tree is read.

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

from tools_test_support import REPO, TOOLS, TempDirTestCase, clean_env, git, run_tool

import corpora


class LockTest(unittest.TestCase):
    def test_repository_lock_pins_the_frostengine_main_corpus(self) -> None:
        document = corpora.load_lock(REPO / corpora.LOCK_FILE)
        by_id = {entry["id"]: entry for entry in document["corpora"]}
        self.assertIn("frostengine", by_id)
        engine = by_id["frostengine"]
        self.assertEqual(engine["url"], "https://github.com/Frowsty/FrostEngine-AI-Claude")
        self.assertEqual(engine["branch"], "main")  # ADR 0077 decision 8: only main SHAs are pinned.
        self.assertRegex(engine["sha"], r"^[0-9a-f]{40}$")
        self.assertEqual(engine["presets"], {"windows": "windows-debug", "linux": "linux-debug"})
        # The corpus directory is named after the pin, under the corpora root.
        self.assertEqual(corpora.corpus_dir(Path("/c"), engine).name, "frostengine-" + engine["sha"][:12])

    def test_lock_text_is_what_pin_writes(self) -> None:
        # pin rewrites the file with json.dumps(indent=2); the checked-in text must already be in
        # that form so a re-pin changes only the pinned fields.
        path = REPO / corpora.LOCK_FILE
        data = path.read_bytes()
        self.assertEqual(data, (json.dumps(json.loads(data), indent=2, ensure_ascii=False) + "\n").encode("utf-8"))

    def test_invalid_locks_are_rejected(self) -> None:
        good = {"id": "x", "name": "x", "url": "u", "branch": "main", "sha": "a" * 40, "purpose": "p"}
        bad = [
            {"corpora": []},
            {"corpora": [{"id": "x"}]},
            {"corpora": [good | {"sha": "A" * 40}]},
            {"corpora": [good | {"id": "Bad Id"}]},
            {"corpora": [good, good]},
            [],
        ]
        for index, document in enumerate(bad):
            with self.subTest(case=index):
                path = Path(self._tmpfile(json.dumps(document)))
                with self.assertRaises(corpora.LockError):
                    corpora.load_lock(path)

    def _tmpfile(self, text: str) -> str:
        import tempfile
        handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8")
        handle.write(text)
        handle.close()
        self.addCleanup(Path(handle.name).unlink)
        return handle.name


class CorporaToolTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        work = self.tmp / "work"
        work.mkdir()
        git("init", "-q", cwd=work)
        (work / "a.txt").write_text("first\n", encoding="utf-8")
        git("add", "a.txt", cwd=work)
        git("commit", "-q", "-m", "first", cwd=work)
        self.first = git("rev-parse", "HEAD", cwd=work).strip()
        (work / "a.txt").write_text("second\n", encoding="utf-8")
        git("commit", "-q", "-am", "second", cwd=work)
        self.second = git("rev-parse", "HEAD", cwd=work).strip()
        self.work = work
        self.bare = self.tmp / "remote.git"
        git("clone", "-q", "--bare", str(work), str(self.bare), cwd=self.tmp)
        self.root = self.tmp / "corpora"

    def _lock(self, sha: str, url: str | None = None) -> Path:
        entry = {"id": "engine", "name": "fixture", "url": url or self.bare.as_uri(), "branch": "main",
                 "sha": sha, "purpose": "tests"}
        path = self.tmp / "corpora.lock.json"
        path.write_text(json.dumps({"schemaVersion": 1, "corpora": [entry]}, indent=2) + "\n", encoding="utf-8")
        return path

    def _run(self, lock: Path, *args: str):
        return run_tool("corpora.py", "--lock", str(lock), "--root", str(self.root), *args, env=clean_env())

    def _dir(self, sha: str) -> Path:
        return self.root / f"engine-{sha[:12]}"

    def test_fetch_clones_the_pin_in_full_and_verify_accepts_it(self) -> None:
        lock = self._lock(self.first)
        result = self._run(lock, "fetch")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        clone = self._dir(self.first)
        self.assertEqual(git("rev-parse", "HEAD", cwd=clone).strip(), self.first)
        self.assertEqual((clone / "a.txt").read_text(encoding="utf-8"), "first\n")
        self.assertEqual(git("rev-parse", "--is-shallow-repository", cwd=clone).strip(), "false")
        self.assertEqual(git("config", "core.autocrlf", cwd=clone).strip(), "false")
        self.assertFalse(clone.with_name(clone.name + ".partial").exists())
        verified = self._run(lock, "verify")
        self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
        self.assertIn("engine: ok at " + self.first, verified.stdout)
        again = self._run(lock, "fetch")
        self.assertEqual(again.returncode, 0)
        self.assertIn("up to date", again.stdout)
        # Build trees and caches inside the corpus are untracked files and allowed.
        (clone / "build").mkdir()
        (clone / "build" / "out.txt").write_text("x", encoding="utf-8")
        self.assertEqual(self._run(lock, "verify").returncode, 0)
        path = self._run(lock, "path", "engine")
        self.assertEqual(Path(path.stdout.strip()), clone)

    def test_a_missing_corpus_fails_verify(self) -> None:
        result = self._run(self._lock(self.first), "verify")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing:", result.stdout)
        listed = self._run(self._lock(self.first), "list")
        self.assertEqual(listed.returncode, 0)
        self.assertIn("missing:", listed.stdout)
        self.assertFalse(self.root.exists(), "list and verify create nothing")

    def test_verify_rejects_changed_tracked_files_another_commit_and_shallow_clones(self) -> None:
        lock = self._lock(self.first)
        self.assertEqual(self._run(lock, "fetch").returncode, 0)
        clone = self._dir(self.first)
        (clone / "a.txt").write_text("edited\n", encoding="utf-8")
        result = self._run(lock, "verify")
        self.assertEqual(result.returncode, 1)
        self.assertIn("changed tracked files (a.txt)", result.stdout)
        # fetch never deletes local work without --replace.
        refused = self._run(lock, "fetch")
        self.assertEqual(refused.returncode, 1)
        self.assertIn("--replace", refused.stdout)
        self.assertEqual((clone / "a.txt").read_text(encoding="utf-8"), "edited\n")
        replaced = self._run(lock, "fetch", "--replace")
        self.assertEqual(replaced.returncode, 0, replaced.stdout + replaced.stderr)
        self.assertEqual((clone / "a.txt").read_text(encoding="utf-8"), "first\n")

        git("-c", "advice.detachedHead=false", "checkout", "-q", "--detach", self.second, cwd=clone)
        moved = self._run(lock, "verify")
        self.assertEqual(moved.returncode, 1)
        self.assertIn("not the pinned " + self.first, moved.stdout)

        shallow_root = self.tmp / "shallow"
        shallow = shallow_root / f"engine-{self.second[:12]}"
        git("clone", "-q", "--depth", "1", self.bare.as_uri(), str(shallow), cwd=self.tmp)
        result = run_tool("corpora.py", "--lock", str(self._lock(self.second)), "--root", str(shallow_root),
                          "verify", env=clean_env())
        self.assertEqual(result.returncode, 1)
        self.assertIn("shallow", result.stdout)

    def test_the_local_fallback_is_used_only_when_the_url_fails_and_origin_points_at_the_url(self) -> None:
        broken_url = (self.tmp / "no-such-remote.git").as_uri()
        lock = self._lock(self.first, url=broken_url)
        without = self._run(lock, "fetch")
        self.assertEqual(without.returncode, 1, without.stdout + without.stderr)
        self.assertIn("cannot clone", without.stdout)
        self.assertFalse(self._dir(self.first).exists())
        with_fallback = self._run(lock, "fetch", "--fallback-source", str(self.work))
        self.assertEqual(with_fallback.returncode, 0, with_fallback.stdout + with_fallback.stderr)
        clone = self._dir(self.first)
        self.assertIn("from " + str(self.work), with_fallback.stdout)
        self.assertEqual(git("rev-parse", "HEAD", cwd=clone).strip(), self.first)
        self.assertEqual(git("remote", "get-url", "origin", cwd=clone).strip(), broken_url)
        # --no-hardlinks: the clone's objects are copies, so changing the source cannot reach it.
        alternates = clone / ".git" / "objects" / "info" / "alternates"
        self.assertFalse(alternates.exists())
        objects = [p for p in (clone / ".git" / "objects").rglob("*") if p.is_file()]
        self.assertTrue(objects)
        self.assertTrue(all(p.stat().st_nlink == 1 for p in objects), "an object file is hard-linked")
        # A fallback is never used while the url works: the clone then comes from the url.
        other_root = self.tmp / "other"
        good = run_tool("corpora.py", "--lock", str(self._lock(self.first)), "--root", str(other_root), "fetch",
                        "--fallback-source", str(self.work), env=clean_env())
        self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
        self.assertIn("from " + self.bare.as_uri(), good.stdout)

    def test_an_unknown_sha_fails_and_leaves_nothing(self) -> None:
        missing = "0123456789abcdef0123456789abcdef01234567"
        result = self._run(self._lock(missing), "fetch")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("has no commit " + missing, result.stdout)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_a_new_pin_prunes_older_clones_of_the_same_corpus_only(self) -> None:
        self.assertEqual(self._run(self._lock(self.first), "fetch").returncode, 0)
        unrelated = self.root / "other-0123456789ab"
        unrelated.mkdir()
        mine = self.root / "engine-notes"
        mine.mkdir()
        kept = self._run(self._lock(self.second), "fetch", "--keep-old")
        self.assertEqual(kept.returncode, 0, kept.stdout + kept.stderr)
        self.assertTrue(self._dir(self.first).exists())
        pruned = self._run(self._lock(self.second), "fetch")
        self.assertEqual(pruned.returncode, 0, pruned.stdout + pruned.stderr)
        self.assertIn("removed the older pin", pruned.stdout)
        self.assertFalse(self._dir(self.first).exists())
        self.assertTrue(self._dir(self.second).exists())
        self.assertTrue(unrelated.exists())
        self.assertTrue(mine.exists())

    def test_pin_takes_the_branch_tip_or_an_explicit_sha(self) -> None:
        lock = self._lock(self.first)
        result = self._run(lock, "pin")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"engine: {self.first} -> {self.second}", result.stdout)
        self.assertNotIn(b"\r", lock.read_bytes())  # LF line ends on every platform.
        document = json.loads(lock.read_text(encoding="utf-8"))
        self.assertEqual(document["corpora"][0]["sha"], self.second)
        self.assertRegex(document["corpora"][0]["pinned"], r"^\d{4}-\d{2}-\d{2}$")
        self.assertEqual(self._run(lock, "pin", "--sha", self.first).returncode, 0)
        self.assertEqual(json.loads(lock.read_text(encoding="utf-8"))["corpora"][0]["sha"], self.first)
        self.assertEqual(self._run(lock, "pin", "--sha", "nope").returncode, 1)

    def test_usage_errors(self) -> None:
        lock = self._lock(self.first)
        self.assertEqual(self._run(lock, "verify", "--only", "nope").returncode, 2)
        self.assertEqual(self._run(lock, "fetch", "--fallback-source", str(self.tmp / "absent")).returncode, 2)
        broken = self.tmp / "broken.json"
        broken.write_text("{", encoding="utf-8")
        self.assertEqual(self._run(broken, "list").returncode, 2)


class VerifyCorpusTierTest(unittest.TestCase):
    """The -Corpus tier: corpus tests run only there, a missing corpus fails it, -Quick skips them."""

    def test_verify_ps1_has_a_corpus_tier_that_quick_excludes(self) -> None:
        text = (TOOLS / "verify.ps1").read_text(encoding="utf-8")
        self.assertRegex(text, r"\[switch\]\$Corpus\b")
        # The default (Quick) Debug run excludes the corpus label, anchored.
        self.assertIn("'-LE', '^(stress|corpus)$'", text)
        # The tier verifies the corpora first (a missing corpus fails) and selects the label, anchored.
        self.assertIn("corpora.py", text)
        self.assertIn("-L '^corpus$'", text)
        self.assertRegex(text, r"corpus[^\n]*no-tests")

    def test_verify_sh_has_a_corpus_tier_that_quick_excludes(self) -> None:
        text = (TOOLS / "verify.sh").read_text(encoding="utf-8")
        self.assertIn("--corpus", text)
        self.assertIn("-LE '^(stress|corpus)$'", text)
        self.assertIn("-L '^corpus$'", text)
        self.assertIn("corpora.py", text)

    def test_corpus_tests_carry_the_corpus_label(self) -> None:
        text = (REPO / "tests" / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"frost_add_test\(app_corpus_tests\b.*?\)\n", text, re.S)
        self.assertIsNotNone(match, "app_corpus_tests is registered")
        self.assertRegex(match.group(0), r"LABELS[^\n]*\bcorpus\b")


if __name__ == "__main__":
    unittest.main()
