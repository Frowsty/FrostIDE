#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# corpora.py: the pinned FrostEngine corpora of the -Corpus verify tier, listed in
# tests/corpus/corpora.lock.json (ADR 0077 decision 7; ADR 0078; PLAN.md section 7, Roadmap v2).
#
#   python tools/corpora.py [--lock <file>] [--root <dir>] list
#   python tools/corpora.py ... fetch [--only <id>] [--fallback-source <path>] [--replace] [--keep-old]
#   python tools/corpora.py ... verify [--only <id>]
#   python tools/corpora.py ... path <id>
#   python tools/corpora.py ... root
#   python tools/corpora.py ... pin [--only <id>] [--sha <sha>]
#
# Each corpus is a full `git clone --no-hardlinks` of its 'url', checked out detached at its pinned
# 'sha', in <root>/<id>-<sha[:12]>. <root> is --root, else $FROST_CORPORA_ROOT, else
# E:\FrostBuilds\corpora on Windows and ~/FrostBuilds/corpora (ext4) on Linux.
#
#   list    prints every corpus with its pin, directory and state (ok, missing or the problem).
#   fetch   clones what is missing into <dir>.partial, checks out the pin, verifies it and renames it
#           into place; a corpus that already verifies is left alone. The clone comes from 'url'; only
#           when that clone fails and --fallback-source names a local repository (never by default)
#           is that repository cloned instead, with --no-hardlinks so the copy shares no file with it,
#           and its origin is then set back to 'url'. A directory that exists but does not verify
#           (another commit, changed tracked files) is refused unless --replace is given, so local
#           work is never deleted silently. Afterwards older pins of the same corpus
#           (<root>/<id>-<12 hex digits> other than the pinned one) are removed, unless --keep-old.
#   verify  exits 1 unless every corpus (or --only) is a git checkout whose HEAD is the pinned sha, with
#           no tracked file changed and a full (not shallow) history. Untracked files (build trees,
#           download caches) are allowed: the corpus tests configure and index the corpus in place.
#           A missing corpus is a failure, never a skip (the -Corpus tier relies on this).
#   path    prints the directory of corpus <id>.
#   root    prints the corpora root. The verify scripts' corpus tier exports it as FROST_CORPORA_ROOT,
#           so the corpus tests (tests/app/corpus_test.cpp) name no developer machine path (FND-10).
#   pin     re-pins 'sha' (and 'pinned', today's date) to the tip of 'branch' (git ls-remote), or to
#           --sha, and rewrites the lock file.
#
# Side effects: git clones under <root>; fetch --replace and the pruning of older pins delete
# directories under <root> that match <id>-<12 hex digits> only. Exit status: 0 success, 1 a fetch
# or verification failed, 2 usage error or invalid lock file. Python 3.11+, standard library only;
# needs git on PATH.

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

LOCK_FILE = "tests/corpus/corpora.lock.json"
REQUIRED_FIELDS = ("id", "name", "url", "branch", "sha", "purpose")
_SHA = re.compile(r"^[0-9a-f]{40}$")
_ID = re.compile(r"^[a-z0-9][a-z0-9_-]*$")


class LockError(ValueError):
    """Raised for a lock file that does not match the schema above."""


def default_root() -> Path:
    """The corpora root: $FROST_CORPORA_ROOT, else E:\\FrostBuilds\\corpora (Windows) or
    ~/FrostBuilds/corpora."""
    if os.environ.get("FROST_CORPORA_ROOT"):
        return Path(os.environ["FROST_CORPORA_ROOT"])
    return Path("E:/FrostBuilds/corpora") if sys.platform == "win32" else Path.home() / "FrostBuilds" / "corpora"


def load_lock(path: Path) -> dict:
    """Reads and validates the lock file; returns the whole document. Raises LockError."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise LockError(f"{path}: {error}") from error
    corpora = document.get("corpora") if isinstance(document, dict) else None
    if not isinstance(corpora, list) or not corpora:
        raise LockError(f"{path}: 'corpora' must be a non-empty list")
    seen = set()
    for entry in corpora:
        if not isinstance(entry, dict):
            raise LockError(f"{path}: every corpus must be an object")
        missing = [k for k in REQUIRED_FIELDS if k not in entry]
        if missing:
            raise LockError(f"{path}: corpus {entry.get('id', '?')} lacks {', '.join(missing)}")
        if not _ID.match(str(entry["id"])):
            raise LockError(f"{path}: invalid id {entry['id']!r} (lower-case letters, digits, '-' and '_')")
        if entry["id"] in seen:
            raise LockError(f"{path}: duplicate id {entry['id']}")
        seen.add(entry["id"])
        if not _SHA.match(str(entry["sha"])):
            raise LockError(f"{path}: {entry['id']}: sha must be 40 lower-case hex digits")
    return document


def corpus_dir(root: Path, entry: dict) -> Path:
    """<root>/<id>-<sha[:12]>."""
    return root / f"{entry['id']}-{entry['sha'][:12]}"


class Git:
    """Runs git (resolved once to an absolute path) and returns completed processes."""

    def __init__(self) -> None:
        found = shutil.which("git")
        if not found:
            raise FileNotFoundError("git not found on PATH")
        self.exe = str(Path(found).resolve())

    def run(self, *args: str, cwd: Path | None = None, check: bool = True) -> subprocess.CompletedProcess:
        """Runs git with `args` (in `cwd` via -C); raises RuntimeError with git's stderr when check
        is true and git fails."""
        command = [self.exe]
        if cwd is not None:
            command += ["-C", str(cwd)]
        command += list(args)
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                check=False)
        if check and result.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)} failed ({result.returncode}): {result.stderr.strip()}")
        return result


def problem(git: Git, directory: Path, sha: str) -> str | None:
    """Why `directory` is not a usable clone at `sha`, or None when it is."""
    if not directory.is_dir():
        return f"missing: {directory} does not exist (run: python tools/corpora.py fetch)"
    if not (directory / ".git").exists():
        return f"{directory} is not a git checkout"
    head = git.run("rev-parse", "--verify", "-q", "HEAD", cwd=directory, check=False).stdout.strip()
    if head != sha:
        return f"{directory} is at {head or 'no commit'}, not the pinned {sha}"
    shallow = git.run("rev-parse", "--is-shallow-repository", cwd=directory, check=False).stdout.strip()
    if shallow == "true":
        return f"{directory} is a shallow clone; the corpus must be a full clone"
    changed = git.run("status", "--porcelain", "--untracked-files=no", cwd=directory, check=False)
    if changed.returncode != 0:
        return f"git status failed in {directory}: {changed.stderr.strip()}"
    if changed.stdout.strip():
        files = ", ".join(line[3:] for line in changed.stdout.splitlines()[:5])
        return f"{directory} has changed tracked files ({files})"
    return None


def _writable(function, path, _excinfo) -> None:
    # git marks pack files read-only on Windows; make the entry writable and retry once.
    os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    function(path)


def remove_tree(directory: Path) -> None:
    """Deletes `directory` and everything under it, read-only files included."""
    if sys.version_info >= (3, 12):
        shutil.rmtree(directory, onexc=_writable)
    else:  # pragma: no cover - Python 3.11
        shutil.rmtree(directory, onerror=_writable)


def _clone(git: Git, source: str, target: Path) -> subprocess.CompletedProcess:
    # core.autocrlf=false keeps the files byte-exact; core.longpaths for deep engine paths on Windows.
    return git.run("-c", "core.autocrlf=false", "clone", "-q", "--no-hardlinks", "--no-checkout",
                   "-c", "core.autocrlf=false", "-c", "core.longpaths=true", source, str(target), check=False)


def fetch(git: Git, entry: dict, root: Path, fallback: str | None, replace: bool) -> str:
    """Fetches one corpus; returns a status line or raises RuntimeError."""
    directory = corpus_dir(root, entry)
    sha = entry["sha"]
    why = problem(git, directory, sha)
    if why is None:
        return f"{entry['id']}: up to date at {sha} in {directory}"
    if directory.exists():
        if not replace:
            raise RuntimeError(f"{entry['id']}: {why}; move it away or pass --replace")
        remove_tree(directory)
    root.mkdir(parents=True, exist_ok=True)
    partial = directory.with_name(directory.name + ".partial")
    if partial.exists():
        remove_tree(partial)
    cloned = _clone(git, entry["url"], partial)
    source = entry["url"]
    if cloned.returncode != 0:
        first = cloned.stderr.strip()
        if partial.exists():
            remove_tree(partial)
        if not fallback:
            raise RuntimeError(f"{entry['id']}: cannot clone {entry['url']}: {first}")
        cloned = _clone(git, fallback, partial)
        if cloned.returncode != 0:
            if partial.exists():
                remove_tree(partial)
            raise RuntimeError(f"{entry['id']}: cannot clone {entry['url']} ({first}) or the fallback "
                               f"{fallback} ({cloned.stderr.strip()})")
        source = fallback
        git.run("remote", "set-url", "origin", entry["url"], cwd=partial)
    try:
        if git.run("cat-file", "-e", f"{sha}^{{commit}}", cwd=partial, check=False).returncode != 0:
            raise RuntimeError(f"{entry['id']}: {source} has no commit {sha}")
        git.run("-c", "advice.detachedHead=false", "checkout", "-q", "--detach", sha, cwd=partial)
        why = problem(git, partial, sha)
        if why is not None:
            raise RuntimeError(f"{entry['id']}: the new clone does not verify: {why}")
    except RuntimeError:
        remove_tree(partial)
        raise
    partial.rename(directory)
    return f"{entry['id']}: cloned {sha} from {source} into {directory}"


def stale_dirs(root: Path, entry: dict) -> list[Path]:
    """Older pins of `entry` under `root` (<id>-<12 hex>, with or without .partial), never the pinned one."""
    if not root.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(entry['id'])}-[0-9a-f]{{12}}(\.partial)?$")
    keep = corpus_dir(root, entry).name
    return sorted(p for p in root.iterdir() if p.is_dir() and pattern.match(p.name) and p.name != keep)


def pin(git: Git, lock_path: Path, document: dict, wanted: list[str], sha: str | None) -> list[str]:
    """Re-pins the `wanted` corpora and rewrites the lock file; returns status lines."""
    lines = []
    for entry in document["corpora"]:
        if entry["id"] not in wanted:
            continue
        new = sha
        if new is None:
            listed = git.run("ls-remote", entry["url"], f"refs/heads/{entry['branch']}").stdout.split()
            if not listed:
                raise RuntimeError(f"{entry['id']}: {entry['url']} has no branch {entry['branch']}")
            new = listed[0]
        if not _SHA.match(new):
            raise RuntimeError(f"{entry['id']}: {new!r} is not a 40-digit lower-case sha")
        lines.append(f"{entry['id']}: {entry['sha']} -> {new}")
        entry["sha"] = new
        entry["pinned"] = datetime.date.today().isoformat()
    # Bytes, so the file keeps LF line ends on Windows too.
    lock_path.write_bytes((json.dumps(document, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="The pinned FrostEngine corpora (ADR 0077 decision 7)")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository root (default: the checkout containing this script)")
    parser.add_argument("--lock", type=Path, default=None, help=f"lock file (default <repo>/{LOCK_FILE})")
    parser.add_argument("--root", type=Path, default=None,
                        help="corpora root (default $FROST_CORPORA_ROOT, else E:\\FrostBuilds\\corpora or "
                             "~/FrostBuilds/corpora)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="list the corpora and their state")
    fetch_parser = commands.add_parser("fetch", help="clone the missing corpora")
    fetch_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    fetch_parser.add_argument("--fallback-source", default=None, metavar="PATH",
                              help="a local repository to clone when the url cannot be cloned")
    fetch_parser.add_argument("--replace", action="store_true",
                              help="delete and re-clone a corpus directory that does not verify")
    fetch_parser.add_argument("--keep-old", action="store_true", help="keep the clones of older pins")
    verify_parser = commands.add_parser("verify", help="check every corpus against its pin")
    verify_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    path_parser = commands.add_parser("path", help="print the directory of a corpus")
    path_parser.add_argument("id")
    commands.add_parser("root", help="print the corpora root (the verify scripts export it as "
                                     "FROST_CORPORA_ROOT for the corpus tests)")
    pin_parser = commands.add_parser("pin", help="re-pin to the branch tip (or --sha)")
    pin_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    pin_parser.add_argument("--sha", default=None, help="pin this commit instead of the branch tip")
    args = parser.parse_args(argv)

    lock_path = args.lock or (args.repo / LOCK_FILE)
    try:
        document = load_lock(lock_path)
    except LockError as error:
        print(f"corpora: {error}", file=sys.stderr)
        return 2
    corpora = document["corpora"]
    by_id = {entry["id"]: entry for entry in corpora}
    root = (args.root or default_root()).absolute()
    if args.command == "root":
        print(root)
        return 0
    wanted = getattr(args, "only", None) or list(by_id)
    if args.command == "path":
        wanted = [args.id]
    unknown = [i for i in wanted if i not in by_id]
    if unknown:
        print(f"corpora: unknown corpus {', '.join(unknown)} (known: {', '.join(by_id)})", file=sys.stderr)
        return 2
    if args.command == "path":
        print(corpus_dir(root, by_id[args.id]))
        return 0
    try:
        git = Git()
    except FileNotFoundError as error:
        print(f"corpora: {error}", file=sys.stderr)
        return 2

    status = 0
    if args.command == "list":
        for entry in corpora:
            why = problem(git, corpus_dir(root, entry), entry["sha"])
            print(f"{entry['id']:12} {entry['sha']}  {why or 'ok'}  {corpus_dir(root, entry)}")
            print(f"             {entry['name']} ({entry['url']}, {entry['branch']}): {entry['purpose']}")
    elif args.command == "verify":
        for corpus_id in wanted:
            entry = by_id[corpus_id]
            why = problem(git, corpus_dir(root, entry), entry["sha"])
            if why is None:
                print(f"{corpus_id}: ok at {entry['sha']} in {corpus_dir(root, entry)}")
            else:
                print(f"{corpus_id}: {why}")
                status = 1
    elif args.command == "fetch":
        if args.fallback_source and not Path(args.fallback_source).is_dir():
            print(f"corpora: --fallback-source {args.fallback_source} is not a directory", file=sys.stderr)
            return 2
        for corpus_id in wanted:
            entry = by_id[corpus_id]
            try:
                print(fetch(git, entry, root, args.fallback_source, args.replace))
            except RuntimeError as error:
                print(f"{lock_path.as_posix()}: {error}")
                status = 1
                continue
            if not args.keep_old:
                for old in stale_dirs(root, entry):
                    remove_tree(old)
                    print(f"{corpus_id}: removed the older pin {old}")
    elif args.command == "pin":
        try:
            for line in pin(git, lock_path, document, wanted, args.sha):
                print(line)
        except RuntimeError as error:
            print(f"corpora: {error}", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
