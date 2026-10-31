#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# corpus_fetch.py: fetches the pinned third-party corpora listed in
# tests/corpus/manifest.json (PLAN.md section 4, Corpora).
#
#   python tools/corpus_fetch.py [--repo <path>] [--manifest <file>] [--dest <dir>] --list
#   python tools/corpus_fetch.py ... --only R3 [--only R2b]
#   python tools/corpus_fetch.py ... --all
#
# Each corpus is shallow-fetched at exactly its pinned commit into <dest>/<id> (dest
# defaults to $FROST_CORPUS_ROOT, else E:\corpus on Windows and ~/corpus elsewhere), then
# checked out detached and verified: HEAD must equal the pinned SHA. Servers that refuse
# a fetch by SHA are retried through the pinned tag, and the HEAD check still decides.
# An existing checkout already at the pinned commit is left alone. Without --only or
# --all nothing is fetched: the corpora are large (llvm-project is several GB), so a
# fetch is always an explicit choice, and tests never fetch the real ones.
#
# Corpora are test inputs only: never linked and never committed, and the clean-room
# rule (CLAUDE.md) means their sources are not read for implementation ideas.
#
# Side effects: creates <dest>/<id> git repositories (core.autocrlf=false so files stay
# byte-exact; core.longpaths=true on Windows). Exit status: 0 success, 1 a fetch or
# verification failed, 2 usage error or invalid manifest. Python 3.11+, standard
# library only; needs git on PATH.

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

MANIFEST_FILE = "tests/corpus/manifest.json"
REQUIRED_FIELDS = ("id", "name", "url", "tag", "sha", "licence", "approx_size_mb", "prerequisites", "purpose")
_SHA = re.compile(r"^[0-9a-f]{40}$")


class ManifestError(ValueError):
    """Raised for a manifest that does not match the schema above."""


def default_dest() -> Path:
    """The corpus root: $FROST_CORPUS_ROOT, else E:\\corpus (Windows) or ~/corpus."""
    if os.environ.get("FROST_CORPUS_ROOT"):
        return Path(os.environ["FROST_CORPUS_ROOT"])
    return Path("E:/corpus") if sys.platform == "win32" else Path.home() / "corpus"


def load_manifest(path: Path) -> list[dict]:
    """Reads and validates the manifest; raises ManifestError with the reason."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ManifestError(f"{path}: {error}") from error
    corpora = document.get("corpora")
    if not isinstance(corpora, list) or not corpora:
        raise ManifestError(f"{path}: 'corpora' must be a non-empty list")
    seen = set()
    for entry in corpora:
        missing = [k for k in REQUIRED_FIELDS if k not in entry]
        if missing:
            raise ManifestError(f"{path}: entry {entry.get('id', '?')} lacks {', '.join(missing)}")
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", str(entry["id"])):
            raise ManifestError(f"{path}: invalid id {entry['id']!r}")
        if entry["id"] in seen:
            raise ManifestError(f"{path}: duplicate id {entry['id']}")
        seen.add(entry["id"])
        if not _SHA.match(str(entry["sha"])):
            raise ManifestError(f"{path}: {entry['id']}: sha must be 40 lower-case hex digits")
        tag_object = entry.get("tag_object")
        if tag_object is not None and not _SHA.match(str(tag_object)):
            raise ManifestError(f"{path}: {entry['id']}: tag_object must be null or 40 hex digits")
    return corpora


def _entry_line(path: Path, corpus_id: str) -> int:
    text = path.read_text(encoding="utf-8")
    position = text.find(f'"id": "{corpus_id}"')
    return 1 if position < 0 else text.count("\n", 0, position) + 1


class Git:
    """Runs git (resolved once to an absolute path) and returns completed processes."""

    def __init__(self) -> None:
        found = shutil.which("git")
        if not found:
            raise FileNotFoundError("git not found on PATH")
        self.exe = str(Path(found).resolve())

    def run(self, *args: str, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess:
        command = [self.exe]
        if cwd is not None:
            command += ["-C", str(cwd)]
        command += list(args)
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if check and result.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}")
        return result


def head_of(git: Git, directory: Path) -> str | None:
    """The commit checked out in `directory`, or None when it is not a git repository."""
    if not (directory / ".git").exists():
        return None
    result = git.run("rev-parse", "--verify", "-q", "HEAD", cwd=directory, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def tree_size_mb(directory: Path) -> float:
    total = 0
    for root, dirs, files in os.walk(directory):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total / (1024 * 1024)


def fetch(git: Git, entry: dict, dest_root: Path) -> str:
    """Fetches one corpus; returns a status line or raises RuntimeError."""
    directory = dest_root / entry["id"]
    pinned = entry["sha"]
    if head_of(git, directory) == pinned:
        return f"{entry['id']}: up to date at {pinned}"
    if directory.exists() and not (directory / ".git").exists() and any(directory.iterdir()):
        raise RuntimeError(f"{directory} exists and is not a git checkout; move it away first")
    directory.mkdir(parents=True, exist_ok=True)
    if not (directory / ".git").exists():
        git.run("init", "-q", str(directory))
    git.run("config", "core.autocrlf", "false", cwd=directory)
    if sys.platform == "win32":
        git.run("config", "core.longpaths", "true", cwd=directory)
    remotes = git.run("remote", cwd=directory).stdout.split()
    git.run("remote", "set-url" if "origin" in remotes else "add", "origin", entry["url"], cwd=directory)
    by_sha = git.run("fetch", "-q", "--depth", "1", "--no-tags", "origin", pinned, cwd=directory, check=False)
    if by_sha.returncode != 0:
        tag_ref = f"refs/tags/{entry['tag']}"
        by_tag = git.run("fetch", "-q", "--depth", "1", "--no-tags", "origin", tag_ref, cwd=directory, check=False)
        if by_tag.returncode != 0:
            raise RuntimeError(f"{entry['id']}: cannot fetch {pinned} ({by_sha.stderr.strip()}) "
                               f"or tag {entry['tag']} ({by_tag.stderr.strip()})")
    git.run("-c", "advice.detachedHead=false", "checkout", "-q", "--force", "--detach", "FETCH_HEAD^{commit}",
            cwd=directory)
    head = head_of(git, directory)
    if head != pinned:
        raise RuntimeError(f"{entry['id']}: HEAD {head} does not match the pinned {pinned} "
                           f"(tag {entry['tag']} may have moved); not using this checkout")
    return f"{entry['id']}: fetched {pinned} into {directory} ({tree_size_mb(directory):.0f} MB checked out)"


def list_corpora(git: Git | None, corpora: list[dict], dest_root: Path) -> None:
    for entry in corpora:
        directory = dest_root / entry["id"]
        head = head_of(git, directory) if git else None
        state = "present" if head == entry["sha"] else ("other commit" if head else "missing")
        print(f"{entry['id']:5} {entry['tag']:18} {entry['sha']}  {entry['licence']:32} "
              f"~{entry['approx_size_mb']} MB  {state:12} {directory}")
        print(f"      {entry['name']}: {entry['purpose']}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch the pinned third-party corpora (PLAN.md section 4)")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("--manifest", type=Path, default=None, help=f"manifest (default <repo>/{MANIFEST_FILE})")
    parser.add_argument("--dest", type=Path, default=None,
                        help="corpus root (default $FROST_CORPUS_ROOT, else E:\\corpus or ~/corpus)")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--list", action="store_true", help="list the corpora and their state (the default)")
    action.add_argument("--only", action="append", metavar="ID", help="fetch this corpus (repeatable)")
    action.add_argument("--all", action="store_true", help="fetch every corpus")
    args = parser.parse_args(argv)
    manifest = args.manifest or (args.repo / MANIFEST_FILE)
    try:
        corpora = load_manifest(manifest)
    except ManifestError as error:
        print(f"corpus_fetch: {error}", file=sys.stderr)
        return 2
    dest_root = (args.dest or default_dest()).absolute()
    try:
        git = Git()
    except FileNotFoundError as error:
        print(f"corpus_fetch: {error}", file=sys.stderr)
        return 2
    if not args.only and not args.all:
        list_corpora(git, corpora, dest_root)
        if not args.list:
            print("corpus_fetch: nothing fetched; pass --only <id> or --all", file=sys.stderr)
        return 0
    by_id = {entry["id"]: entry for entry in corpora}
    wanted = list(by_id) if args.all else args.only
    unknown = [i for i in wanted if i not in by_id]
    if unknown:
        print(f"corpus_fetch: unknown corpus {', '.join(unknown)} (known: {', '.join(by_id)})", file=sys.stderr)
        return 2
    status = 0
    for corpus_id in wanted:
        try:
            print(fetch(git, by_id[corpus_id], dest_root))
        except RuntimeError as error:
            print(f"{manifest.as_posix()}:{_entry_line(manifest, corpus_id)}: {error}")
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
