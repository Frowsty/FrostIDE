# SPDX-License-Identifier: Apache-2.0
#
# Tests for the FND-2 footer check: tools/hooks/verify_stamp.py (tree hashes, stamps, the
# commit-msg decision) and the tools/hooks/commit-msg hook itself, run by a real git in scratch
# repositories with core.hooksPath pointing at tools/hooks (ADR 0078 decision 5). A stale stamp
# is rejected, a matching one accepted, and the real index is never touched.

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env

HOOKS = TOOLS / "hooks"
sys.path.insert(0, str(HOOKS))

import verify_stamp  # noqa: E402  (after the path change on purpose)

SUMMARY = "Verified: win debug 3/3 (1 skipped), format ok, frost_lint ok"


class ScratchRepo(TempDirTestCase):
    """A scratch repository on main with the hook installed and one commit."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        self.git_exe = shutil.which("git")
        if self.git_exe is None:
            raise AssertionError("git is required on PATH (part of the dev environment)")
        self.env = clean_env(FROST_HOOK_PYTHON=sys.executable, GIT_CONFIG_NOSYSTEM="1",
                             GIT_AUTHOR_NAME="FrostIDE tests", GIT_AUTHOR_EMAIL="tests@frostide.invalid",
                             GIT_COMMITTER_NAME="FrostIDE tests", GIT_COMMITTER_EMAIL="tests@frostide.invalid")
        self.git("init", "-q", "-b", "main")
        self.git("config", "core.autocrlf", "false")
        self.write("a.txt", "one\n")
        self.git("add", "a.txt")
        self.git("commit", "-q", "-m", "initial")
        self.git("config", "core.hooksPath", HOOKS.as_posix())

    def git(self, *args: str, check: bool = True, cwd: Path | None = None) -> subprocess.CompletedProcess:
        result = subprocess.run([self.git_exe, *args], cwd=cwd or self.repo, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", env=self.env, check=False)
        if check and result.returncode != 0:
            raise AssertionError(f"git {' '.join(args)} failed: {result.stdout}{result.stderr}")
        return result

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")

    def stamp(self, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-I", str(HOOKS / "verify_stamp.py"), *args,
                               "--repo", str(cwd or self.repo)],
                              capture_output=True, text=True, encoding="utf-8", env=self.env, check=False)

    def commit(self, message: str, *extra: str) -> subprocess.CompletedProcess:
        return self.git("commit", "-q", *extra, "-m", message, check=False)


class CommitMsgHookTest(ScratchRepo):
    def test_matching_stamp_is_accepted(self) -> None:
        self.write("a.txt", "two\n")
        self.git("add", "a.txt")
        written = self.stamp("write", "--summary", SUMMARY)
        self.assertEqual(written.returncode, 0, written.stdout + written.stderr)
        self.assertIn("verify-stamp: written", written.stdout)
        result = self.commit(f"feat: two\n\n{SUMMARY}\nCo-Authored-By: someone <x@example.invalid>")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("feat: two", self.git("log", "-1", "--format=%s").stdout)

    def test_stale_stamp_is_rejected(self) -> None:
        self.write("a.txt", "two\n")
        self.assertEqual(self.stamp("write", "--summary", SUMMARY).returncode, 0)
        # The tree changes after verification: the stamp no longer matches what is committed.
        self.write("a.txt", "three\n")
        self.git("add", "a.txt")
        head = self.git("rev-parse", "HEAD").stdout
        result = self.commit(f"feat: three\n\n{SUMMARY}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no verify-stamp for the tree being committed", result.stderr)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout, head)

    def test_footer_items_must_be_stamped(self) -> None:
        self.write("a.txt", "two\n")
        self.git("add", "a.txt")
        self.assertEqual(self.stamp("write", "--summary", SUMMARY).returncode, 0)
        result = self.commit("feat: two\n\nVerified: win debug 4/4, frost_lint ok")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("'win debug 4/4'", result.stderr)
        # A subset of the stamped items is still true.
        self.assertEqual(self.commit("feat: two\n\nVerified: win debug 3/3 (1 skipped)").returncode, 0)

    def test_main_needs_a_footer(self) -> None:
        self.write("a.txt", "two\n")
        self.git("add", "a.txt")
        result = self.commit("feat: two")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("need a 'Verified:' footer", result.stderr)

    def test_other_branches_pass_without_a_stamp(self) -> None:
        self.git("switch", "-q", "-c", "r0/slice")
        self.write("a.txt", "two\n")
        self.git("add", "a.txt")
        self.assertEqual(self.commit("feat: two\n\nVerified: anything").returncode, 0)

    def test_windows_and_wsl_stamps_back_one_footer(self) -> None:
        self.write("a.txt", "two\n")
        self.git("add", "a.txt")
        self.assertEqual(self.stamp("write", "--summary", "Verified: win debug 3/3, frost_lint ok").returncode, 0)
        self.assertEqual(self.stamp("write", "--script", "verify.sh", "--summary",
                                    "wsl: debug 2/2 (1 disabled), licensecheck ok").returncode, 0)
        footer = "Verified: win debug 3/3, wsl debug 2/2 (1 disabled), wsl licensecheck ok, frost_lint ok"
        self.assertEqual(self.commit(f"feat: two\n\n{footer}").returncode, 0)

    def test_commit_all_checks_the_index_git_commits(self) -> None:
        # `git commit -a` commits through a temporary index (GIT_INDEX_FILE); the hook must use it.
        self.write("a.txt", "two\n")
        self.assertEqual(self.stamp("write", "--summary", SUMMARY).returncode, 0)
        self.assertEqual(self.commit(f"feat: two\n\n{SUMMARY}", "-a").returncode, 0)

    def test_untracked_files_belong_to_the_verified_tree(self) -> None:
        # verify builds untracked files too, so a commit that leaves them out is not the verified tree.
        self.write("b.txt", "new\n")
        self.assertEqual(self.stamp("write", "--summary", SUMMARY).returncode, 0)
        self.assertNotEqual(self.commit(f"feat: a\n\n{SUMMARY}", "--allow-empty").returncode, 0)
        self.git("add", "b.txt")
        self.assertEqual(self.commit(f"feat: b\n\n{SUMMARY}").returncode, 0)


class StampTest(ScratchRepo):
    def test_tree_leaves_the_real_index_alone(self) -> None:
        self.write("b.txt", "new\n")
        before = self.git("status", "--porcelain").stdout
        tree = self.stamp("tree")
        self.assertEqual(tree.returncode, 0, tree.stderr)
        self.assertRegex(tree.stdout.strip(), r"^[0-9a-f]{40}$")
        self.assertEqual(self.git("status", "--porcelain").stdout, before)
        self.assertIn("?? b.txt", before)
        # The hash is the tree of committing everything.
        self.git("add", "-A")
        self.assertEqual(self.git("write-tree").stdout.strip(), tree.stdout.strip())

    def test_a_tree_changed_mid_run_is_not_stamped(self) -> None:
        before = self.stamp("tree").stdout.strip()
        self.write("a.txt", "changed while verifying\n")
        result = self.stamp("write", "--summary", SUMMARY, "--expect-tree", before)
        self.assertEqual(result.returncode, 1)
        self.assertIn("the tree changed during verification", result.stdout)
        stamps = self.repo / ".git" / "frost" / "verify-stamps"
        self.assertFalse(stamps.exists() and any(stamps.iterdir()))

    def test_failed_runs_and_other_lines_are_never_stamped(self) -> None:
        for line in ("FAILED: " + SUMMARY, "win debug 3/3"):
            result = self.stamp("write", "--summary", line)
            self.assertEqual(result.returncode, 1, line)
            self.assertIn("not written", result.stdout)

    def test_stamp_records_the_summary_and_its_items(self) -> None:
        self.assertEqual(self.stamp("write", "--summary", SUMMARY).returncode, 0)
        tree = self.stamp("tree").stdout.strip()
        data = json.loads((self.repo / ".git" / "frost" / "verify-stamps" / f"{tree}.json").read_text(encoding="utf-8"))
        self.assertEqual(data["tree"], tree)
        self.assertEqual([r["summary"] for r in data["records"]], [SUMMARY])
        self.assertEqual(data["records"][0]["items"], ["win debug 3/3 (1 skipped)", "format ok", "frost_lint ok"])
        # A second run of the same script replaces its record.
        self.assertEqual(self.stamp("write", "--summary", "Verified: win debug 3/3, format ok").returncode, 0)
        data = json.loads((self.repo / ".git" / "frost" / "verify-stamps" / f"{tree}.json").read_text(encoding="utf-8"))
        self.assertEqual([r["summary"] for r in data["records"]], ["Verified: win debug 3/3, format ok"])

    def test_worktrees_share_the_stamps(self) -> None:
        other = self.tmp / "other"
        self.git("worktree", "add", "-q", "-b", "r0/other", str(other))
        try:
            self.assertEqual(self.stamp("write", "--summary", SUMMARY, cwd=other).returncode, 0)
            # Same files, same tree: main sees the stamp the worktree wrote.
            tree = self.stamp("tree").stdout.strip()
            self.assertTrue((self.repo / ".git" / "frost" / "verify-stamps" / f"{tree}.json").is_file())
            self.write("a.txt", "one\n")
            self.assertEqual(self.commit(f"chore: same tree\n\n{SUMMARY}", "--allow-empty").returncode, 0)
        finally:
            self.git("worktree", "remove", "--force", str(other), check=False)


class SummaryItemsTest(unittest.TestCase):
    def test_items_split_outside_parentheses_and_wsl_items_are_prefixed(self) -> None:
        self.assertEqual(verify_stamp.summary_items("Verified: win debug 3/3 (1 skipped, 1 disabled), tidy ok (2 TUs)"),
                         ["win debug 3/3 (1 skipped, 1 disabled)", "tidy ok (2 TUs)"])
        self.assertEqual(verify_stamp.summary_items("wsl: debug 2/2, licensecheck ok"),
                         ["wsl debug 2/2", "wsl licensecheck ok"])
        with self.assertRaises(ValueError):
            verify_stamp.summary_items("debug 2/2")

    def test_footer_is_the_last_verified_line_outside_comments(self) -> None:
        message = "feat: x\n\nVerified: a, b\n# Verified: c\nCo-Authored-By: y\n"
        self.assertEqual(verify_stamp.footer_of(message), "a, b")
        self.assertIsNone(verify_stamp.footer_of("feat: x\n\nno footer\n"))


if __name__ == "__main__":
    unittest.main()
