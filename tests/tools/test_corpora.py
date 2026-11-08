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

    def test_root_prints_the_root_the_corpus_tests_read(self) -> None:
        # --root wins; without it $FROST_CORPORA_ROOT. The verify scripts export what `root` prints.
        lock = self._lock(self.first)
        given = self._run(lock, "root")
        self.assertEqual(given.returncode, 0, given.stdout + given.stderr)
        self.assertEqual(Path(given.stdout.strip()), self.root.absolute())
        elsewhere = self.tmp / "elsewhere"
        from_env = run_tool("corpora.py", "--lock", str(lock), "root",
                            env=clean_env(FROST_CORPORA_ROOT=str(elsewhere)))
        self.assertEqual(from_env.returncode, 0, from_env.stdout + from_env.stderr)
        self.assertEqual(Path(from_env.stdout.strip()), elsewhere.absolute())

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
        self.assertEqual(self._run(lock, "fetch", "--cpm-source", str(self.tmp / "absent")).returncode, 2)
        broken = self.tmp / "broken.json"
        broken.write_text("{", encoding="utf-8")
        self.assertEqual(self._run(broken, "list").returncode, 2)


class EngineTest(TempDirTestCase):
    """The configured engine (one per pin and preset), its build lock and the CPM source cache copy.
    Fixture trees only: no cmake runs here (configure_engine runs in the corpus tier)."""

    def setUp(self) -> None:
        super().setUp()
        self.root = self.tmp / "corpora"
        self.entry = {"id": "engine", "sha": "ab" * 20, "presets": {"windows": "win-dbg", "linux": "lin-dbg"}}
        self.corpus = corpora.corpus_dir(self.root, self.entry)

    def _configured(self, preset: str, *, sha: str | None = None, source: Path | None = None) -> Path:
        build = self.corpus / "build" / preset
        build.mkdir(parents=True)
        home = (source or self.corpus).as_posix()
        (build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={home}\n", encoding="utf-8")
        (build / "compile_commands.json").write_text("[]\n", encoding="utf-8")
        stamp = {"sha": sha or self.entry["sha"], "preset": preset}
        (build / corpora.ENGINE_STAMP).write_text(json.dumps(stamp), encoding="utf-8")
        return build

    def test_the_platform_preset_comes_from_the_lock_unless_named(self) -> None:
        self.assertEqual(corpora.engine_preset(self.entry, None), self.entry["presets"][corpora.PLATFORM])
        self.assertEqual(corpora.engine_preset(self.entry, "other"), "other")
        self.assertIsNone(corpora.engine_preset({"id": "x"}, None))

    def test_engine_problem_names_each_missing_or_foreign_part(self) -> None:
        self.assertIn("is not configured", corpora.engine_problem(self.corpus, self.entry, "win-dbg"))
        build = self._configured("win-dbg")
        self.assertIsNone(corpora.engine_problem(self.corpus, self.entry, "win-dbg"))
        # Another preset, another pin, another source directory, no database, no stamp.
        self.assertIn("is not configured", corpora.engine_problem(self.corpus, self.entry, "lin-dbg"))
        other = self._configured("old", sha="cd" * 20)
        self.assertIn("names " + "cd" * 20, corpora.engine_problem(self.corpus, self.entry, "old"))
        self._configured("foreign", source=self.tmp / "elsewhere")
        self.assertIn("another source directory", corpora.engine_problem(self.corpus, self.entry, "foreign"))
        (build / "compile_commands.json").unlink()
        self.assertIn("no compile_commands.json", corpora.engine_problem(self.corpus, self.entry, "win-dbg"))
        (other / corpora.ENGINE_STAMP).unlink()
        self.assertIn("configured outside", corpora.engine_problem(self.corpus, self.entry, "old"))

    def test_the_engine_command_reports_a_missing_engine_as_a_failure(self) -> None:
        # A lock whose corpus is absent: engine fails (the -Corpus tier's "pinned engine missing").
        entry = {"id": "engine", "name": "fixture", "url": "file:///absent", "branch": "main", "sha": "ab" * 20,
                 "purpose": "tests", "presets": {"windows": "win-dbg", "linux": "lin-dbg"}}
        lock = self.tmp / "lock.json"
        lock.write_text(json.dumps({"schemaVersion": 1, "corpora": [entry]}), encoding="utf-8")
        result = run_tool("corpora.py", "--lock", str(lock), "--root", str(self.root), "engine", env=clean_env())
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("engine: " + corpora.engine_preset(entry, None) + ":", result.stdout)

    def test_the_build_lock_is_exclusive_and_named_after_the_pin(self) -> None:
        path = corpora.build_lock_path(self.root, self.entry)
        self.assertEqual(path, self.root / ("engine-" + "ab" * 6 + ".build.lock"))
        with corpora.BuildLock(path, timeout=5):
            with self.assertRaises(TimeoutError):
                with corpora.BuildLock(path, timeout=0):
                    pass
        with corpora.BuildLock(path, timeout=5):  # Released on exit.
            pass

    def test_stale_lock_files_are_the_older_pins_only(self) -> None:
        self.root.mkdir(parents=True)
        names = ["engine-" + "ab" * 6 + ".build.lock", "engine-" + "ab" * 6 + ".win-dbg.configure.log",
                 "engine-" + "cd" * 6 + ".build.lock", "engine-" + "cd" * 6 + ".lin-dbg.configure.log",
                 "other-" + "cd" * 6 + ".build.lock"]
        for name in names:
            (self.root / name).write_text("", encoding="utf-8")
        self.assertEqual([p.name for p in corpora.stale_lock_files(self.root, self.entry)], sorted(names[2:4]))

    def test_new_cpm_packages_are_copied_and_existing_ones_kept(self) -> None:
        source, target = self.tmp / "source", self.tmp / "target"
        for package, version in (("fmt", "h1"), ("glm", "h2"), ("glm", "h3")):
            (source / package / version).mkdir(parents=True)
            (source / package / version / "f.txt").write_text("new", encoding="utf-8")
        (target / "glm" / "h2").mkdir(parents=True)
        (target / "glm" / "h2" / "f.txt").write_text("kept", encoding="utf-8")
        (target / "fmt" / "h1.partial").mkdir(parents=True)  # An interrupted copy is redone.
        self.assertEqual(corpora.copy_new_packages(source, target), ["fmt/h1", "glm/h3"])
        self.assertEqual((target / "glm" / "h2" / "f.txt").read_text(encoding="utf-8"), "kept")
        self.assertEqual((target / "fmt" / "h1" / "f.txt").read_text(encoding="utf-8"), "new")
        self.assertFalse((target / "fmt" / "h1.partial").exists())
        self.assertEqual(corpora.copy_new_packages(source, target), [])
        self.assertEqual(corpora.copy_new_packages(self.tmp / "absent", target), [])

    def test_a_clone_is_seeded_from_the_shared_cpm_cache(self) -> None:
        self.assertIsNone(corpora.seed_cpm_cache(self.root, self.entry, None))  # Nothing to copy from.
        shared = self.root / corpora.CPM_CACHE_NAME / "fmt" / "h1"
        shared.mkdir(parents=True)
        (shared / "f.txt").write_text("x", encoding="utf-8")
        self.assertIn("1 packages copied", corpora.seed_cpm_cache(self.root, self.entry, None))
        self.assertTrue((self.corpus / ".cache" / "cpm" / "fmt" / "h1" / "f.txt").is_file())

    def test_wsl_paths(self) -> None:
        self.assertEqual(corpora.to_wsl_path(Path("F:/Builds/corpora")), "/mnt/f/Builds/corpora")


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
        # The corpus tests get the root from corpora.py, not from a path of their own (FND-10).
        self.assertIn("$env:FROST_CORPORA_ROOT = (& $python -I (Join-Path $PSScriptRoot 'corpora.py') "
                      "--repo $repo root", text)
        # A missing pinned engine fails the tier (FND-6b): the configured engine is checked first.
        self.assertIn("--repo $repo engine --configure", text)
        self.assertIn("pinned engine not configured", text)

    def test_verify_sh_has_a_corpus_tier_that_quick_excludes(self) -> None:
        text = (TOOLS / "verify.sh").read_text(encoding="utf-8")
        self.assertIn("--corpus", text)
        self.assertIn("-LE '^(stress|corpus)$'", text)
        self.assertIn("-L '^corpus$'", text)
        self.assertIn("corpora.py", text)
        self.assertIn('FROST_CORPORA_ROOT="$(python3 -I tools/corpora.py --repo "$MIRROR" root)"', text)
        self.assertIn("export FROST_CORPORA_ROOT", text)
        self.assertIn('tools/corpora.py --repo "$MIRROR" engine --configure', text)

    def test_corpus_tests_carry_the_corpus_label(self) -> None:
        text = (REPO / "tests" / "app" / "CMakeLists.txt").read_text(encoding="utf-8")
        match = re.search(r"frost_add_test\(app_corpus_tests\b.*?\)\n", text, re.S)
        self.assertIsNotNone(match, "app_corpus_tests is registered")
        self.assertRegex(match.group(0), r"LABELS[^\n]*\bcorpus\b")
        # The journeys are corpus tests too (FND-7a).
        journeys = (REPO / "tests" / "journeys" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(journeys, r"add_test\(NAME journeys\.j1_cmake\b")
        self.assertIn('LABELS "corpus;', journeys)


if __name__ == "__main__":
    unittest.main()
