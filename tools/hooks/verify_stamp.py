#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# verify_stamp.py: the verify-stamps that tie a "Verified:" commit footer to the tree that was
# verified (ADR 0078 decision 5, FND-2). There is no CI (D24); this is the local check.
#
#   python tools/hooks/verify_stamp.py tree  [--repo R] [--git-dir D --work-tree W]
#   python tools/hooks/verify_stamp.py write [--repo R] [--git-dir D --work-tree W] --summary LINE
#                                            [--expect-tree T] [--script verify.ps1]
#   python tools/hooks/verify_stamp.py check-msg MESSAGE_FILE [--repo R]
#
# `tree` prints the tree hash of the working tree as a commit of everything would record it:
# tracked and untracked files that are not ignored, through a temporary copy of the index, so
# the real index is never touched (`git add -A` then `git write-tree` on the copy).
#
# `write` is run by tools/verify.ps1 and tools/verify.sh after a passing run. It records the
# summary line ("Verified: win debug 3/3, ...", or verify.sh's "wsl: debug 3/3, ..." whose items
# become "wsl debug 3/3", ...) under that tree hash in <git common dir>/frost/verify-stamps/<tree>.json,
# shared by every worktree of the repository. With --expect-tree, the tree taken before the run
# must still be the tree after it, or nothing is written (exit 1): files changed mid-run.
#
# `check-msg` is the commit-msg hook (tools/hooks/commit-msg). On the protected branches
# (PROTECTED_BRANCHES) a commit needs a "Verified:" footer, a stamp for the tree being committed
# (`git write-tree` of the index git commits), and every footer item among that stamp's items.
# Other branches pass. Install the hook in a checkout with
#   git config core.hooksPath tools/hooks
# A fast-forward merge creates no commit and runs no hook: merge to main with --no-ff after
# verifying the merged tree (git merge --no-ff --no-commit, verify, git commit).
#
# Exit status: 0 ok, 1 rejected (or, for write, not written), 2 usage or environment error.
# Python 3.11+, standard library only, self-contained (run with -I, so no sibling imports).

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Branches whose commits must carry a stamped footer (ADR 0078 decision 5: every commit on main).
PROTECTED_BRANCHES = ("main",)
STAMP_DIR = ("frost", "verify-stamps")
FOOTER = re.compile(r"^Verified:\s*(.*\S)\s*$")


class StampError(Exception):
    """git failed or is missing."""


class Git:
    """git in one working tree; git_dir and work_tree as verify.sh passes them for a Windows worktree."""

    def __init__(self, repo: Path, git_dir: str | None = None, work_tree: str | None = None) -> None:
        exe = shutil.which("git")
        if exe is None:
            raise StampError("git is not on PATH")
        self.base = [os.path.abspath(exe), "-C", str(repo)]
        if git_dir:
            self.base.append(f"--git-dir={git_dir}")
        if work_tree:
            self.base.append(f"--work-tree={work_tree}")
        self.repo = repo

    def run(self, *args: str, env: dict[str, str] | None = None) -> str:
        """Runs git (no shell); returns stdout without the trailing newline; raises StampError."""
        result = subprocess.run([*self.base, *args], capture_output=True, stdin=subprocess.DEVNULL, env=env,
                                check=False)
        if result.returncode != 0:
            lines = result.stderr.decode("utf-8", "replace").strip().splitlines()
            raise StampError(f"git {' '.join(args)} failed: {lines[-1] if lines else result.returncode}")
        return result.stdout.decode("utf-8", "replace").rstrip("\n")

    def path(self, option: str) -> Path:
        """An absolute path from `git rev-parse <option>` (--git-common-dir, --git-path index)."""
        value = Path(self.run("rev-parse", *option.split()))
        return value if value.is_absolute() else (self.repo / value)


def split_items(text: str) -> list[str]:
    """Splits a summary at each ', ' outside parentheses, as verify.ps1's Split-WslSummary does."""
    items, depth, current = [], 0, []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")" and depth > 0:
            depth -= 1
        if depth == 0 and text.startswith(", ", i):
            items.append("".join(current).strip())
            current = []
            i += 2
            continue
        current.append(c)
        i += 1
    items.append("".join(current).strip())
    return [item for item in items if item]


def summary_items(summary: str) -> list[str]:
    """The footer items a summary line backs: "Verified: a, b" gives [a, b]; "wsl: a, b" gives
    ["wsl a", "wsl b"] (verify.ps1 prefixes verify.sh's items the same way)."""
    summary = summary.strip()
    if summary.startswith("Verified:"):
        return split_items(summary[len("Verified:"):].strip())
    if summary.startswith("wsl:"):
        return ["wsl " + item for item in split_items(summary[len("wsl:"):].strip())]
    raise ValueError(f"not a verify summary line: {summary!r}")


def working_tree_hash(git: Git) -> str:
    """The tree a commit of the whole working tree would record; the real index stays untouched."""
    index = git.path("--git-path index")
    with tempfile.TemporaryDirectory(prefix="frost-stamp-") as tmp:
        scratch = Path(tmp) / "index"
        if index.is_file():
            shutil.copyfile(index, scratch)
        env = dict(os.environ, GIT_INDEX_FILE=str(scratch))
        # Minimal stat checks: an index written by Git for Windows is read from WSL too.
        git.run("-c", "core.checkStat=minimal", "-c", "core.trustctime=false", "add", "-A", env=env)
        return git.run("write-tree", env=env)


def stamp_path(git: Git, tree: str) -> Path:
    return git.path("--git-common-dir").joinpath(*STAMP_DIR, f"{tree}.json")


def load_stamp(git: Git, tree: str) -> dict:
    """The stamp for `tree` ({"tree", "records": [...]}), or an empty one."""
    path = stamp_path(git, tree)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(data, dict) and isinstance(data.get("records"), list):
            return data
    except (OSError, ValueError):
        pass
    return {"tree": tree, "records": []}


def write_stamp(git: Git, summary: str, script: str, expect_tree: str | None) -> tuple[bool, str]:
    """Records `summary` for the working tree; (written, message)."""
    if summary.lstrip().startswith("FAILED"):
        return False, "a failed run is never stamped"
    try:
        items = summary_items(summary)
    except ValueError as error:
        return False, str(error)
    tree = working_tree_hash(git)
    if expect_tree and tree != expect_tree:
        return False, f"the tree changed during verification ({expect_tree[:12]} before, {tree[:12]} after)"
    stamp = load_stamp(git, tree)
    record = {
        "script": script,
        "summary": summary.strip(),
        "items": items,
        "head": git.run("rev-parse", "--verify", "--quiet", "HEAD^{commit}") if _has_head(git) else None,
        "time": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    }
    stamp["records"] = [r for r in stamp["records"] if r.get("script") != script] + [record]
    path = stamp_path(git, tree)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    return True, f"tree {tree}"


def _has_head(git: Git) -> bool:
    try:
        git.run("rev-parse", "--verify", "--quiet", "HEAD^{commit}")
        return True
    except StampError:
        return False


def current_branch(git: Git) -> str | None:
    """The branch HEAD names (also during a merge), or None when detached."""
    try:
        ref = git.run("symbolic-ref", "-q", "HEAD")
    except StampError:
        return None
    return ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else None


def footer_of(message: str) -> str | None:
    """The text after the last "Verified:" line of a commit message (comment lines dropped)."""
    footer = None
    for line in message.splitlines():
        if line.startswith("#"):
            continue
        m = FOOTER.match(line)
        if m:
            footer = m.group(1)
    return footer


def check_message(git: Git, message: str) -> tuple[bool, str]:
    """(accepted, reason) for a commit message about to be committed in `git`."""
    branch = current_branch(git)
    if branch not in PROTECTED_BRANCHES:
        return True, f"branch {branch or '(detached)'} is not protected"
    footer = footer_of(message)
    if footer is None:
        return False, (f"commits on {branch} need a 'Verified:' footer with the real verify summary "
                       f"(ADR 0078 decision 5)")
    tree = git.run("write-tree")
    stamp = load_stamp(git, tree)
    if not stamp["records"]:
        return False, (f"no verify-stamp for the tree being committed ({tree}); run tools\\verify.ps1 "
                       f"(or tools/verify.sh) on exactly this tree, then commit it unchanged")
    backed = {item for record in stamp["records"] for item in record.get("items", [])}
    unbacked = [item for item in split_items(footer) if item not in backed]
    if unbacked:
        return False, (f"the footer claims {', '.join(repr(i) for i in unbacked)}, which no verify-stamp of tree "
                       f"{tree[:12]} records; copy the summary line verify printed")
    return True, f"stamped tree {tree}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="FrostIDE verify-stamps and the commit-msg check (FND-2)")
    parser.add_argument("command", choices=("tree", "write", "check-msg"))
    parser.add_argument("message", nargs="?", help="check-msg: the commit message file git passes the hook")
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="working tree (default: the current directory)")
    parser.add_argument("--git-dir", default=None)
    parser.add_argument("--work-tree", default=None)
    parser.add_argument("--summary", help="write: the summary line verify printed last")
    parser.add_argument("--script", default="verify.ps1", help="write: which script verified (one record each)")
    parser.add_argument("--expect-tree", default=None, help="write: the tree taken before the run")
    args = parser.parse_args(argv)
    try:
        git = Git(args.repo.resolve(), args.git_dir, args.work_tree)
        if args.command == "tree":
            print(working_tree_hash(git))
            return 0
        if args.command == "write":
            if not args.summary:
                parser.error("write needs --summary")
            written, message = write_stamp(git, args.summary, args.script, args.expect_tree)
            print(f"verify-stamp: {'written' if written else 'not written'} ({message})")
            return 0 if written else 1
        if not args.message:
            parser.error("check-msg needs the message file")
        text = Path(args.message).read_text(encoding="utf-8", errors="replace")
        accepted, reason = check_message(git, text)
        if not accepted:
            print(f"commit-msg (FND-2): rejected: {reason}", file=sys.stderr)
            return 1
        return 0
    except (StampError, OSError) as error:
        print(f"verify-stamp: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
