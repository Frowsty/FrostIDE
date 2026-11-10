#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# corpora.py: the pinned corpora of the -Corpus verify tier (FrostEngine, and R2a, the Notepad++
# MSVC solution of SLN-0), listed in tests/corpus/corpora.lock.json (ADR 0077 decision 7; ADR 0078;
# PLAN.md section 7, Roadmap v2).
#
#   python tools/corpora.py [--lock <file>] [--root <dir>] list
#   python tools/corpora.py ... fetch [--only <id>] [--fallback-source <path>] [--cpm-source <dir>] [--replace]
#                                     [--keep-old]
#   python tools/corpora.py ... verify [--only <id>]
#   python tools/corpora.py ... engine [--only <id>] [--preset <name>] [--configure]
#   python tools/corpora.py ... wsl-fetch [--only <id>] [--distro Ubuntu-24.04] [--configure]   (Windows)
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
#   engine  the one configured engine per pinned sha and preset (FND-6b): for every corpus with
#           'presets' (or --only), the preset of this platform (or --preset) must be configured in
#           <dir>/build/<preset> (CMakeCache.txt of this source directory, compile_commands.json and
#           the stamp .frost-corpus-engine.json naming the pinned sha); exit 1 when it is not. With
#           --configure a missing one is configured first with `cmake --preset <preset>` (the pinned
#           cmake and ninja from PATH, checked against sdk/sdk.lock.json; on Windows inside the
#           pinned MSVC toolset's vcvarsall environment, on Linux with CC/CXX the pinned clang of
#           FROST_LLVM_ROOT), logged to <root>/<id>-<sha12>.<preset>.configure.log. The lock's optional
#           cacheVariables.<platform> ({"NAME": "value"}) are passed as -DNAME=value, and a tree whose
#           cache holds another value is not the pin's engine (so --configure reconfigures it). Nothing is ever
#           built (a full engine build is about 49 GB): the corpus tests build single small targets.
#           Configure and the corpus tests that configure or build in place hold the build lock
#           <root>/<id>-<sha12>.build.lock (an exclusive lock on that file; frost::test's
#           tests/support/engine_lock.hpp and tests/journeys take the same lock).
#   CPM source cache: the configure fills <dir>/.cache/cpm (the engine's CPM_SOURCE_CACHE preset
#           variable) from the network; afterwards new packages are copied into
#           <root>/cpm-source-cache, and fetch seeds a new clone's .cache/cpm from that copy (or
#           from --cpm-source), so a re-pin or the WSL clone downloads nothing that is cached. The
#           cache only ever comes from a corpus clone's own fetch, never from a developer tree.
#   wsl-fetch  (Windows) runs `fetch` inside WSL (python3 and git of the distribution) for the
#           ext4 root ~/FrostBuilds/corpora, cloning from the url, else from this machine's corpus
#           clone (/mnt/<drive>/...) as the fallback source, with that clone's .cache/cpm as
#           --cpm-source; with --configure then `engine --configure` there (linux-debug).
#   path    prints the directory of corpus <id>.
#   platforms  a corpus may name the platforms it belongs to ("platforms": ["windows"]; default
#           both). fetch, verify and engine skip, and say so, a corpus of another platform, and
#           wsl-fetch one that is not for linux; list marks it. R2a is a Visual Studio solution, so
#           it is a Windows corpus only.
#   root    prints the corpora root. The verify scripts' corpus tier exports it as FROST_CORPORA_ROOT,
#           so the corpus tests (tests/app/corpus_test.cpp) name no developer machine path (FND-10).
#   pin     re-pins 'sha' (and 'pinned', today's date) to the tip of 'branch' (git ls-remote), or to
#           --sha, and rewrites the lock file.
#
# Side effects: git clones under <root>; fetch --replace and the pruning of older pins delete
# directories (and build lock files) under <root> that match <id>-<12 hex digits> only; engine
# --configure writes the build tree, the stamp, the log and the CPM cache copy named above. Exit
# status: 0 success, 1 a fetch, configure or verification failed, 2 usage error or invalid lock
# file. Python 3.11+, standard library only; needs git on PATH (and cmake, ninja for engine).

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
import time
from pathlib import Path, PureWindowsPath

LOCK_FILE = "tests/corpus/corpora.lock.json"
SDK_LOCK_FILE = "sdk/sdk.lock.json"
# The shared copy of the corpora's CPM download caches under <root> (header comment).
CPM_CACHE_NAME = "cpm-source-cache"
# Written into a configured engine's build directory: which pin and preset configured it.
ENGINE_STAMP = ".frost-corpus-engine.json"
WINDOWS = sys.platform == "win32"
PLATFORM = "windows" if WINDOWS else "linux"
REQUIRED_FIELDS = ("id", "name", "url", "branch", "sha", "purpose")
_SHA = re.compile(r"^[0-9a-f]{40}$")
_ID = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
_CACHE_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")  # A CMake cache variable name of cacheVariables.
PLATFORMS = ("windows", "linux")  # The values of a corpus's optional 'platforms' (header comment).


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
        variables = entry.get("cacheVariables", {})
        if not isinstance(variables, dict) or not all(
                isinstance(by_name, dict) and all(isinstance(k, str) and _CACHE_NAME.match(k) and isinstance(v, str)
                                                  for k, v in by_name.items())
                for by_name in variables.values()):
            raise LockError(f"{path}: {entry['id']}: cacheVariables must map a platform to {{NAME: string}}")
        platforms = entry.get("platforms", list(PLATFORMS))
        if (not isinstance(platforms, list) or not platforms or
                not all(isinstance(name, str) and name in PLATFORMS for name in platforms)):
            raise LockError(f"{path}: {entry['id']}: platforms must be a non-empty list of {', '.join(PLATFORMS)}")
    return document


def on_platform(entry: dict, platform: str = PLATFORM) -> bool:
    """Whether the corpus of `entry` belongs to `platform` (its 'platforms', default every one)."""
    return platform in entry.get("platforms", PLATFORMS)


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
        # Never ask for credentials (a terminal prompt in WSL would wait forever, Git Credential
        # Manager would open a window): an unreachable url fails, and fetch falls back.
        env = dict(os.environ, GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="never")
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                stdin=subprocess.DEVNULL, env=env, check=False)
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


def seed_cpm_cache(root: Path, entry: dict, cpm_source: Path | None) -> str | None:
    """Copies the packages of `cpm_source` (else <root>/cpm-source-cache) that the corpus's
    .cache/cpm lacks into it; a status line, or None when there was nothing to copy from."""
    source = cpm_source or root / CPM_CACHE_NAME
    if not source.is_dir():
        return None
    copied = copy_new_packages(source, corpus_dir(root, entry) / ".cache" / "cpm")
    return f"{entry['id']}: CPM source cache: {len(copied)} packages copied from {source}"


def stale_dirs(root: Path, entry: dict) -> list[Path]:
    """Older pins of `entry` under `root` (<id>-<12 hex>, with or without .partial), never the pinned one."""
    if not root.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(entry['id'])}-[0-9a-f]{{12}}(\.partial)?$")
    keep = corpus_dir(root, entry).name
    return sorted(p for p in root.iterdir() if p.is_dir() and pattern.match(p.name) and p.name != keep)


def stale_lock_files(root: Path, entry: dict) -> list[Path]:
    """Build lock files and configure logs of older pins of `entry` under `root`."""
    if not root.is_dir():
        return []
    pattern = re.compile(rf"^{re.escape(entry['id'])}-([0-9a-f]{{12}})\.(build\.lock|[\w.-]+\.configure\.log)$")
    keep = entry["sha"][:12]
    return sorted(p for p in root.iterdir()
                  if p.is_file() and (m := pattern.match(p.name)) and m.group(1) != keep)


# ---------------------------------------------------------------------------------------------
# The CPM source cache copy
# ---------------------------------------------------------------------------------------------

def copy_new_packages(source: Path, target: Path) -> list[str]:
    """Copies every top-level package directory of the CPM cache `source` that `target` lacks
    (CPM names a package's cache <package>/<hash of its download>; existing entries are never
    overwritten). Returns the copied entries as "<package>/<hash>"."""
    copied = []
    if not source.is_dir():
        return copied
    for package in sorted(p for p in source.iterdir() if p.is_dir()):
        for version in sorted(p for p in package.iterdir() if p.is_dir()):
            destination = target / package.name / version.name
            if destination.exists():
                continue
            partial = destination.with_name(destination.name + ".partial")
            if partial.exists():
                remove_tree(partial)
            partial.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(version, partial, symlinks=True)
            partial.rename(destination)
            copied.append(f"{package.name}/{version.name}")
    return copied


# ---------------------------------------------------------------------------------------------
# The configured engine (one per pinned sha and preset) and its build lock
# ---------------------------------------------------------------------------------------------

def build_lock_path(root: Path, entry: dict) -> Path:
    """<root>/<id>-<sha12>.build.lock: held while the engine is configured or built in place."""
    return root / f"{entry['id']}-{entry['sha'][:12]}.build.lock"


class BuildLock:
    """An exclusive lock on the build lock file (msvcrt.locking of its first byte on Windows, flock
    elsewhere), waiting up to `timeout` seconds. A context manager; raises TimeoutError."""

    def __init__(self, path: Path, timeout: float = 7200.0) -> None:
        self.path, self.timeout, self.handle = path, timeout, None

    def __enter__(self) -> "BuildLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = open(self.path, "a+b")  # noqa: SIM115 (closed in __exit__)
        deadline = time.monotonic() + self.timeout
        told = False
        while True:
            try:
                if WINDOWS:
                    import msvcrt
                    self.handle.seek(0)
                    msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.monotonic() >= deadline:
                    self.handle.close()
                    raise TimeoutError(f"{self.path} stayed locked for {self.timeout:.0f} s") from None
                if not told:
                    print(f"corpora: waiting for the build lock {self.path}", flush=True)
                    told = True
                time.sleep(1.0)  # Polling a lock held by another process; nothing to wait on instead.

    def __exit__(self, *_exc) -> None:
        if self.handle is None:
            return
        if WINDOWS:
            import msvcrt
            self.handle.seek(0)
            try:
                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
        self.handle.close()
        self.handle = None


def engine_preset(entry: dict, preset: str | None) -> str | None:
    """The preset to configure: `preset`, else the lock's presets[PLATFORM], else None."""
    if preset:
        return preset
    presets = entry.get("presets")
    return presets.get(PLATFORM) if isinstance(presets, dict) else None


def engine_cache_variables(entry: dict) -> dict[str, str]:
    """The lock's cacheVariables of this platform (header comment, engine); {} when it has none."""
    return dict(entry.get("cacheVariables", {}).get(PLATFORM, {}))


def _same_dir(a: str, b: Path) -> bool:
    left = os.path.normpath(a.replace("/", os.sep))
    right = os.path.normpath(str(b))
    return os.path.normcase(left) == os.path.normcase(right)


def _same_file(a: str, b: str) -> bool:
    """Whether two spellings name one file (8.3 short names and separators included)."""
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.normcase(os.path.normpath(a.replace("/", os.sep))) == \
            os.path.normcase(os.path.normpath(b.replace("/", os.sep)))


def engine_problem(directory: Path, entry: dict, preset: str, tools: dict[str, str] | None = None) -> str | None:
    """Why <directory>/build/<preset> is not the configured engine of the pin, or None when it is.
    With `tools` ({"cmake": path, "ninja": path}, the pinned ones), a build tree whose cache names
    another cmake (CMAKE_COMMAND) or make program (CMAKE_MAKE_PROGRAM) is not the pin's either: a
    configure with Visual Studio's bundled tools would otherwise pass for the pinned engine."""
    build = directory / "build" / preset
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return f"missing: {build} is not configured (run: python tools/corpora.py engine --configure)"
    text = cache.read_text(encoding="utf-8", errors="replace")
    home = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", text, re.M)
    if not home or not _same_dir(home.group(1).strip(), directory):
        return f"{cache} was configured for another source directory ({home.group(1) if home else 'none'})"
    for name, variable in (("cmake", "CMAKE_COMMAND"), ("ninja", "CMAKE_MAKE_PROGRAM")):
        if tools is None or name not in tools:
            continue
        cached = re.search(rf"^{variable}:[A-Z]+=(.*)$", text, re.M)
        value = cached.group(1).strip() if cached else ""
        if not value or not _same_file(value, tools[name]):
            return (f"{cache} names {variable}={value or '(none)'}, not the pinned {name} {tools[name]} "
                    f"(run: python tools/corpora.py engine --configure)")
    for variable, wanted in engine_cache_variables(entry).items():
        cached = re.search(rf"^{re.escape(variable)}:[A-Z]+=(.*)$", text, re.M)
        if not cached or cached.group(1).strip() != wanted:
            return (f"{cache} names {variable}={cached.group(1).strip() if cached else '(none)'}, not the lock's "
                    f"{wanted} (run: python tools/corpora.py engine --configure)")
    commands = build / "compile_commands.json"
    if not commands.is_file() or commands.stat().st_size < 3:
        return f"{build} has no compile_commands.json"
    try:
        stamp = json.loads((build / ENGINE_STAMP).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return f"{build} has no {ENGINE_STAMP} (configured outside corpora.py engine)"
    if stamp.get("sha") != entry["sha"] or stamp.get("preset") != preset:
        return f"{build / ENGINE_STAMP} names {stamp.get('sha')} {stamp.get('preset')}, not {entry['sha']} {preset}"
    return None


def pinned_tool(name: str, repo: Path) -> str:
    """The absolute path of `name` on PATH, which must report the version sdk/sdk.lock.json pins for
    this platform (cmake, ninja). Raises RuntimeError naming what was found instead."""
    found = shutil.which(name)
    if not found:
        raise RuntimeError(f"{name} is not on PATH (tools/env.ps1, ~/sdk/env.sh)")
    sdk = json.loads((repo / SDK_LOCK_FILE).read_text(encoding="utf-8"))
    version = sdk[PLATFORM][name]["version"]
    out = subprocess.run([found, "--version"], capture_output=True, text=True, errors="replace", check=False)
    if version not in out.stdout + out.stderr:
        raise RuntimeError(f"{found} is not the pinned {name} {version}: {(out.stdout + out.stderr).strip()[:200]}")
    return str(Path(found).resolve())


def msvc_environment(repo: Path) -> dict[str, str]:
    """The environment vcvarsall.bat x64 sets up for the pinned MSVC toolset (sdk.lock.json msvc).
    The same capture as tools/s10_dap_spike.py's. Raises RuntimeError."""
    version = json.loads((repo / SDK_LOCK_FILE).read_text(encoding="utf-8"))["msvc"]["vcToolsVersion"]
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    listed = subprocess.run([str(vswhere), "-all", "-products", "*", "-property", "installationPath"],
                            capture_output=True, text=True, check=False).stdout
    installs = [Path(p.strip()) for p in listed.splitlines() if p.strip()]
    install = next((p for p in installs if (p / "VC" / "Tools" / "MSVC" / version / "include").is_dir()), None)
    if install is None:
        raise RuntimeError(f"the pinned MSVC toolset {version} was not found by vswhere")
    vcvarsall = install / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    cmd = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    # /s strips the outer quotes, so cmd runs: "<vcvarsall>" x64 -vcvars_ver=<v> >nul 2>&1 && set
    line = f'"{cmd}" /d /s /c ""{vcvarsall}" x64 -vcvars_ver={version} >nul 2>&1 && set"'
    result = subprocess.run(line, capture_output=True, text=True, errors="replace",
                            creationflags=0x08000000, check=False)  # CREATE_NO_WINDOW
    env = {}
    for entry in result.stdout.splitlines():
        name, sep, value = entry.partition("=")
        if sep and name:
            env[name] = value
    if result.returncode != 0 or "INCLUDE" not in env:
        raise RuntimeError(f"vcvarsall failed ({result.returncode}) for {vcvarsall}")
    return env


def configure_engine(repo: Path, root: Path, entry: dict, preset: str) -> str:
    """Configures <corpus>/build/<preset> with `cmake --preset` under the build lock unless it is
    already the pin's (header comment, engine). Returns a status line; raises RuntimeError."""
    directory = corpus_dir(root, entry)
    with BuildLock(build_lock_path(root, entry)):
        cmake = pinned_tool("cmake", repo)
        ninja = pinned_tool("ninja", repo)
        tools = {"cmake": cmake, "ninja": ninja}
        if engine_problem(directory, entry, preset, tools) is None:
            return f"{entry['id']}: {preset} reused, configured in {directory / 'build' / preset}"
        env = msvc_environment(repo) if WINDOWS else dict(os.environ)
        # The pinned cmake and ninja first: the Visual Studio environment brings its own. Windows
        # spells the variable "Path"; one entry, whatever its case.
        path_key = next((key for key in env if key.upper() == "PATH"), "PATH")
        env[path_key] = os.pathsep.join([str(Path(cmake).parent), str(Path(ninja).parent), env.get(path_key, "")])
        if not WINDOWS and os.environ.get("FROST_LLVM_ROOT"):
            llvm = Path(os.environ["FROST_LLVM_ROOT"]) / "bin"
            env["CC"], env["CXX"] = str(llvm / "clang"), str(llvm / "clang++")
        cpm = directory / ".cache" / "cpm"
        seeded = copy_new_packages(root / CPM_CACHE_NAME, cpm)
        log = root / f"{entry['id']}-{entry['sha'][:12]}.{preset}.configure.log"
        started = time.monotonic()
        with open(log, "wb") as out:
            # CMAKE_MAKE_PROGRAM is a cache entry, so a tree configured with another Ninja keeps it
            # unless the pinned one is named.
            defines = [f"-D{name}={value}" for name, value in engine_cache_variables(entry).items()]
            result = subprocess.run([cmake, "--preset", preset, f"-DCMAKE_MAKE_PROGRAM={ninja}", *defines],
                                    cwd=directory,
                                    env=env, stdout=out,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=3600, check=False)
        if result.returncode != 0:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-15:]
            raise RuntimeError(f"{entry['id']}: cmake --preset {preset} failed ({result.returncode}); see {log}:\n"
                               + "\n".join(tail))
        build = directory / "build" / preset
        version = subprocess.run([cmake, "--version"], capture_output=True, text=True, check=False).stdout
        (build / ENGINE_STAMP).write_text(json.dumps({
            "sha": entry["sha"], "preset": preset, "configured": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "cmake": version.splitlines()[0] if version else "", "seconds": round(time.monotonic() - started, 1),
        }, indent=2) + "\n", encoding="utf-8")
        problem_now = engine_problem(directory, entry, preset, tools)
        if problem_now is not None:
            raise RuntimeError(f"{entry['id']}: configured, but {problem_now}")
        shared = copy_new_packages(cpm, root / CPM_CACHE_NAME)
        return (f"{entry['id']}: {preset} configured in {build} ({time.monotonic() - started:.0f} s; CPM cache: "
                f"{len(seeded)} seeded, {len(shared)} copied to {root / CPM_CACHE_NAME})")


def to_wsl_path(path: Path | str) -> str:
    """
    /mnt/<drive>/... for a Windows drive path (tools/verify.ps1's ConvertTo-WslPath). On Windows a
    relative path is resolved first; elsewhere (the unit tests' Linux runs) the path is taken as
    written, parsed with Windows rules, because Path.resolve() there would treat "F:/x" as relative.
    Raises ValueError for a path without a drive letter (UNC or POSIX), which WSL cannot mount.
    """
    native = Path(path).resolve() if os.name == "nt" else path
    text = str(PureWindowsPath(native))
    if not re.match(r"^[A-Za-z]:\\", text):  # PureWindowsPath writes backslashes
        raise ValueError(f"not a drive path: {text!r}")
    return "/mnt/" + text[0].lower() + text[2:].replace("\\", "/")


def problem_or_none(entry: dict, root: Path) -> str | None:
    """problem() for the corpus of `entry` under `root`, with git resolved here; a missing git is a problem."""
    try:
        git = Git()
    except FileNotFoundError as error:
        return str(error)
    return problem(git, corpus_dir(root, entry), entry["sha"])


def wsl_fetch(args, root: Path, by_id: dict, wanted: list[str]) -> int:
    """`wsl-fetch` (header comment): fetch (and with --configure, engine --configure) inside WSL,
    from this machine's corpus clones. Returns the exit status of the last WSL run that failed, or 0."""
    wsl = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "wsl.exe"
    script = to_wsl_path(Path(__file__))
    lock = to_wsl_path(args.lock or (args.repo / LOCK_FILE))
    repo = to_wsl_path(args.repo)
    status = 0
    for corpus_id in wanted:
        entry = by_id[corpus_id]
        local = corpus_dir(root, entry)
        why = problem_or_none(entry, root)
        if why is not None:
            print(f"{corpus_id}: the Windows clone is not usable as the source ({why}); run fetch here first")
            status = 1
            continue
        steps = [["fetch", "--only", corpus_id, "--fallback-source", to_wsl_path(local)]]
        if (local / ".cache" / "cpm").is_dir():
            steps[0] += ["--cpm-source", to_wsl_path(local / ".cache" / "cpm")]
        if args.configure:
            steps.append(["engine", "--only", corpus_id, "--configure"])
        for step in steps:
            # --exec runs bash without a shell parsing these arguments; bash sources the SDK
            # environment (FROST_LLVM_ROOT, the pinned cmake and ninja) and then runs python3.
            command = [str(wsl), "-d", args.distro, "--exec", "bash", "-c",
                       'source "$HOME/sdk/env.sh" && exec python3 -I "$0" "$@"', script, "--repo", repo, "--lock",
                       lock, *step]
            print(f"corpora: wsl {' '.join(step)}", flush=True)
            result = subprocess.run(command, check=False)
            if result.returncode != 0:
                status = result.returncode
                break
    return status


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
    fetch_parser.add_argument("--cpm-source", type=Path, default=None, metavar="DIR",
                              help="a corpus clone's .cache/cpm to seed the new clone's from (default: "
                                   f"<root>/{CPM_CACHE_NAME} when it exists)")
    verify_parser = commands.add_parser("verify", help="check every corpus against its pin")
    verify_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    engine_parser = commands.add_parser("engine", help="check (or --configure) the configured engine per preset")
    engine_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    engine_parser.add_argument("--preset", default=None, help=f"the configure preset (default: presets.{PLATFORM})")
    engine_parser.add_argument("--configure", action="store_true", help="configure it when it is missing")
    wsl_parser = commands.add_parser("wsl-fetch", help="(Windows) fetch the corpora into WSL's ext4 root")
    wsl_parser.add_argument("--only", action="append", metavar="ID", help="only this corpus (repeatable)")
    wsl_parser.add_argument("--distro", default="Ubuntu-24.04", help="the WSL distribution (default Ubuntu-24.04)")
    wsl_parser.add_argument("--configure", action="store_true", help="then configure the engine there")
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
    if args.command in ("fetch", "verify", "engine", "wsl-fetch"):
        # A corpus of another platform is skipped, never fetched or failed (header comment).
        target = "linux" if args.command == "wsl-fetch" else PLATFORM
        for corpus_id in wanted:
            if not on_platform(by_id[corpus_id], target):
                print(f"{corpus_id}: skipped, not a {target} corpus (platforms: "
                      f"{', '.join(by_id[corpus_id]['platforms'])})")
        wanted = [corpus_id for corpus_id in wanted if on_platform(by_id[corpus_id], target)]
    if args.command == "engine":
        status = 0
        for corpus_id in wanted:
            entry = by_id[corpus_id]
            preset = engine_preset(entry, args.preset)
            if preset is None:
                continue  # A corpus without presets has no engine leg.
            why = problem_or_none(entry, root)
            if why is None and args.configure:
                try:
                    print(configure_engine(args.repo, root, entry, preset))
                    continue
                except (RuntimeError, TimeoutError, subprocess.TimeoutExpired, OSError) as error:
                    why = str(error)
            elif why is None:
                try:
                    tools = {"cmake": pinned_tool("cmake", args.repo), "ninja": pinned_tool("ninja", args.repo)}
                    why = engine_problem(corpus_dir(root, entry), entry, preset, tools)
                except RuntimeError as error:
                    why = str(error)
            if why is None:
                print(f"{corpus_id}: {preset} ok in {corpus_dir(root, entry) / 'build' / preset}")
            else:
                print(f"{corpus_id}: {preset}: {why}")
                status = 1
        return status
    if args.command == "wsl-fetch":
        if not WINDOWS:
            print("corpora: wsl-fetch runs on Windows; in WSL run fetch", file=sys.stderr)
            return 2
        return wsl_fetch(args, root, by_id, wanted)
    try:
        git = Git()
    except FileNotFoundError as error:
        print(f"corpora: {error}", file=sys.stderr)
        return 2

    status = 0
    if args.command == "list":
        for entry in corpora:
            if on_platform(entry):
                why = problem(git, corpus_dir(root, entry), entry["sha"])
            else:
                why = f"not a {PLATFORM} corpus"
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
        if args.cpm_source and not args.cpm_source.is_dir():
            print(f"corpora: --cpm-source {args.cpm_source} is not a directory", file=sys.stderr)
            return 2
        for corpus_id in wanted:
            entry = by_id[corpus_id]
            try:
                print(fetch(git, entry, root, args.fallback_source, args.replace))
                seeded = seed_cpm_cache(root, entry, args.cpm_source)
                if seeded:
                    print(seeded)
            except (RuntimeError, OSError) as error:
                print(f"{lock_path.as_posix()}: {error}")
                status = 1
                continue
            if not args.keep_old:
                for old in stale_dirs(root, entry):
                    remove_tree(old)
                    print(f"{corpus_id}: removed the older pin {old}")
                for old in stale_lock_files(root, entry):
                    old.unlink()
                    print(f"{corpus_id}: removed {old.name} of an older pin")
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
