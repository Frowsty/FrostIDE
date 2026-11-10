# SPDX-License-Identifier: Apache-2.0
#
# The malicious-repository test (FND-8; PLAN.md section 3.5 "A malicious fixture repository", R3 exit
# "Opening the malicious-repo fixture before trust runs no process (verified by the spawn log)"): the
# real FrostIDE (Debug, offscreen, a fresh profile, the product's defaults: git, clangd, LuaLS and
# project evaluation on) is driven through its automation endpoint (ADR 0080, j1_cmake.py's
# machinery) against a copy of tests/journeys/fixtures/malicious-repo. The fixture carries every way a
# repository can make an IDE run its code: CMakePresets with a planted cmakeExecutable, toolchain file,
# make program, compiler and launcher (configure, build, test and workflow presets, an include and a
# user preset); a CMakeLists.txt that runs planted programs at configure, build and test time; a
# .sln/.vcxproj whose evaluation runs property functions that write files and whose targets
# (InitialTargets, BeforeTargets of Build/ClCompile/GetClCommandLines) and Directory.Build.props/.targets
# Exec planted programs; a .luarc.json naming a LuaLS plugin; a FrostEngine project (.frostproj);
# .frost/settings.json naming planted tool binaries and a .frost run configuration with a before-launch
# command; .clangd and .clang-tidy loading a planted compiler and plugin; .lldbinit and .gdbinit.
# The test adds what needs absolute paths or binaries: trust_canary copies planted under the names an
# attacker would use (git, cmake, cmd, pwsh, bash, clangd, ... in the root and in planted/), a
# compile_commands.json whose compiler is planted and which loads a planted -fplugin, a File API reply
# whose executable target is a planted binary, and a git repository whose config names a planted
# fsmonitor, hooks, pager, external diff and ssh command. The steps:
#   1. open the folder before trust (FrostEngine project, CMake folder), then a C++ file, two Lua
#      files and CMakeLists.txt in it;
#   2. ask for what needs trust: Reload CMake Project, a build and Run;
#   3. open the solution before trust, and the C++ file again (now in the solution);
#   4. from the first open until the observation window ends (OBSERVE_SECONDS after the last request,
#      the CMake service idle), every spawn-log record is a launch PLAN.md section 3.5's restricted mode
#      allows (read-only git with fsmonitor and hooks off, clangd with --enable-config=false, each at an
#      absolute path outside the repository, no plugin flag; restricted_launch_violation), with no
#      launch failure, and no planted program left a marker (*.ran) anywhere in the repository. Both
#      are the oracle: the spawn log is every launch of the process (frost/io/spawn_log.hpp), and a
#      canary marks every run of a planted copy, also one that bypassed the launcher. The R3 exit's
#      literal "the spawn log is empty" would also forbid restricted mode's git and clangd; that is a
#      question for the user (the slice's report), not something this test decides;
#   5. trusting the root through the trust dialog's grant starts tools (the spawn log records at
#      least one start, so the log is known to see launches), and none of them is a binary planted in
#      the root under a tool's name (the IDE resolves tools to absolute paths, never in the project).
#
# The evidence (one JSON file per run, under --evidence), as j1_cmake's. Run by ctest
# (tests/journeys/CMakeLists.txt, label integration: every -Quick leg):
#   trust_malicious_repo.py --frost <frost image> --config <ctest config> --canary <trust_canary image>
#                           --evidence <directory>
# A non-Debug tree, a missing git or a missing canary fails the test (exit 1); it never skips.

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake as j1  # noqa: E402  (the shared journey machinery, beside this file)
from j1_cmake import WINDOWS, EndpointError, corpora, spawn_records  # noqa: E402

JOURNEY = "trust_malicious_repo"
j1.JOURNEY = JOURNEY

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "malicious-repo"
EXE = ".exe" if WINDOWS else ""
# Planted in the root under a tool's name: what a PATH or current-directory search would find first.
ROOT_PLANTS = ("git", "cmake", "ctest", "ninja", "cmd", "pwsh", "powershell", "bash", "sh", "clangd", "clang-tidy",
               "clang-format", "cl", "clang-cl", "MSBuild", "vswhere", "dotnet", "lua-language-server", "lldb-dap",
               "FrostEditor")
# Planted under planted/: the programs the fixture's files name.
NAMED_PLANTS = ("cmake-hook", "build-hook", "test-hook", "toolchain-hook", "msbuild-hook", "luals-hook", "run-hook",
                "before-launch-hook", "lldb-hook", "gdb-hook", "clangd-compiler", "launcher", "cl", "ninja", "cmake",
                "git", "clangd", "clang-tidy", "clang-format", "lua-language-server", "lldb-dap", "FrostEditor",
                "MSBuild", "fsmonitor-hook", "pager", "diff-hook", "ssh-hook", "gpg-hook")
GIT_HOOKS = ("pre-commit", "post-checkout", "post-index-change", "pre-auto-gc", "reference-transaction",
             "fsmonitor-watchman")
# How long nothing may start after the last request before trust. Not a synchronisation: the
# negative ("no launch") is asserted over this window. The product's own timers (the 1 s trust
# watch, the 500 ms git poll, configure-on-open, the file watcher) tick several times within it.
OBSERVE_SECONDS = 15.0
TRUST_START_SECONDS = 300.0
POLL_SECONDS = 0.25
READ_RETRY_SECONDS = 30.0


def plant(canary: Path, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(canary, path)
    if not WINDOWS:
        path.chmod(0o755)


def markers(root: Path) -> list[str]:
    """Every canary marker (*.ran) below `root`, relative, sorted: a planted program or an evaluated
    MSBuild property function ran."""
    return sorted(str(p.relative_to(root)).replace("\\", "/") for p in root.rglob("*.ran"))


def inside(path: str, root: Path) -> bool:
    """Whether `path` names `root` or something below it (case-folded on Windows)."""
    if not path:
        return False
    full = os.path.normcase(os.path.normpath(os.path.abspath(path)))
    base = os.path.normcase(os.path.normpath(str(root)))
    return full == base or full.startswith(base.rstrip("\\/") + os.sep)


# Read-only git subcommands restricted mode runs (PLAN.md section 3.5: "read-only git with -c
# core.fsmonitor=false -c core.hooksPath=<empty dir>"). hash-object is read-only only without -w.
READ_ONLY_GIT = {"rev-parse", "config", "log", "for-each-ref", "cat-file", "symbolic-ref", "ls-tree", "ls-files",
                 "hash-object", "show", "status", "diff", "rev-list", "merge-base", "describe", "show-ref"}
PLUGIN_FLAGS = ("-fplugin", "-fpass-plugin", "-load", "--load", "--query-driver", "-query-driver")


def restricted_launch_violation(record: dict, root: Path) -> str:
    """Why a spawn-log record is not a launch PLAN.md section 3.5 allows before trust, or "" when it
    is: restricted mode may start only git (read-only, with fsmonitor and hooks off) and clangd (with
    --enable-config=false, no --query-driver), each at an absolute path outside the repository, with
    no argument loading a plugin. Anything else (another tool, a launch failure, a path in the root,
    a writing git command) is a violation."""
    event = record.get("event")
    if event == "exit":
        return ""
    if event != "start":
        return f"event {event!r} ({record.get('error', '')})"
    executable = record.get("executable", "")
    argv = [str(a) for a in record.get("argv", [])][1:]
    if not os.path.isabs(executable) or inside(executable, root):
        return f"{executable} is not an absolute path outside the repository"
    if any(a.startswith(PLUGIN_FLAGS) for a in argv):
        return "an argument loads a plugin or a query driver"
    name = Path(executable).stem.lower()
    if name == "git":
        options: dict[str, str] = {}
        i = 0
        while i + 1 < len(argv) and argv[i] in ("-c", "--no-pager"):
            if argv[i] == "-c":
                key, _, value = argv[i + 1].partition("=")
                options[key] = value
                i += 2
            else:
                i += 1
        hooks = options.get("core.hooksPath", "")
        if "--no-pager" not in argv[:i] or options.get("core.fsmonitor") != "false" or not hooks or \
                inside(hooks, root):
            return "git without --no-pager, core.fsmonitor=false and a hooks path outside the repository"
        command = argv[i] if i < len(argv) else ""
        if command not in READ_ONLY_GIT or (command == "hash-object" and "-w" in argv[i:]) or \
                (command == "config" and not {"--get", "--get-regexp", "--list", "-l"} & set(argv[i:])):
            return f"git {command} is not a read-only command"
        return ""
    if name == "clangd":
        if "--enable-config=false" not in argv:
            return "clangd without --enable-config=false"
        return ""
    return f"{Path(executable).name} is not a restricted-mode tool"


def write_file_api_reply(root: Path) -> None:
    """A File API reply in the preset's build directory whose executable target is a planted binary
    (restricted mode may show it, never build or run it)."""
    build = root / "build" / "pwn"
    reply = build / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True, exist_ok=True)
    (build / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={root.as_posix()}\nCMAKE_COMMAND:INTERNAL={(root / ('cmake' + EXE)).as_posix()}\n"
        f"CMAKE_GENERATOR:INTERNAL=Ninja\n", encoding="utf-8")
    target = {"name": "malicious", "id": "malicious::@6890427a1f51a3e7e1df", "type": "EXECUTABLE",
              "artifacts": [{"path": (root / "planted" / ("run-hook" + EXE)).as_posix()}],
              "nameOnDisk": "malicious" + EXE, "paths": {"source": ".", "build": "."},
              "sources": [{"path": "src/main.cpp", "compileGroupIndex": 0}],
              "compileGroups": [{"language": "CXX", "sourceIndexes": [0],
                                 "compileCommandFragments": [{"fragment": "-fplugin=planted/plugin.dll"}]}]}
    (reply / "target-malicious-Debug.json").write_text(json.dumps(target), encoding="utf-8")
    codemodel = {"kind": "codemodel", "version": {"major": 2, "minor": 6},
                 "paths": {"source": root.as_posix(), "build": build.as_posix()},
                 "configurations": [{"name": "Debug", "projects": [{"name": "Malicious", "directoryIndexes": [0],
                                                                    "targetIndexes": [0]}],
                                     "directories": [{"source": ".", "build": ".", "projectIndex": 0,
                                                      "targetIndexes": [0]}],
                                     "targets": [{"name": "malicious", "id": target["id"], "directoryIndex": 0,
                                                  "projectIndex": 0, "jsonFile": "target-malicious-Debug.json"}]}]}
    (reply / "codemodel-v2-planted.json").write_text(json.dumps(codemodel), encoding="utf-8")
    index = {"cmake": {"generator": {"name": "Ninja", "multiConfig": False},
                       "paths": {"cmake": (root / ("cmake" + EXE)).as_posix()}, "version": {"string": "4.4.0"}},
             "objects": [{"kind": "codemodel", "version": {"major": 2, "minor": 6},
                          "jsonFile": "codemodel-v2-planted.json"}],
             "reply": {}}
    (reply / "index-2099-01-01T00-00-00-0000.json").write_text(json.dumps(index), encoding="utf-8")


def make_repository(canary: Path, root: Path) -> None:
    """The fixture copy with its planted binaries, compile database, File API reply and git config."""
    shutil.copytree(FIXTURE, root)
    for name in ROOT_PLANTS:
        plant(canary, root / (name + EXE))
    if not WINDOWS:
        for name in ("git.exe", "cmd.exe", "cmake.exe"):  # Windows spellings a sloppy search would try.
            plant(canary, root / name)
    for name in NAMED_PLANTS:
        plant(canary, root / "planted" / (name + EXE))
    for name in ("msbuild-hook.exe", "plugin.dll", "plugin.so"):
        plant(canary, root / "planted" / name)
    for hook in GIT_HOOKS:
        plant(canary, root / "planted" / "hooks" / hook)
    planted_cl = (root / "planted" / ("cl" + EXE)).as_posix()
    plugin = (root / "planted" / "plugin.dll").as_posix()
    database = [{"directory": root.as_posix(), "file": "src/main.cpp",
                 "arguments": [planted_cl, f"-fplugin={plugin}", "-Xclang", "-load", "-Xclang", plugin, "-c",
                               "src/main.cpp"]}]
    (root / "compile_commands.json").write_text(json.dumps(database, indent=2), encoding="utf-8")
    write_file_api_reply(root)
    # The repository: committed with this test's git (fsmonitor and hooks off for its own commands), then
    # the planted config, and a change in the work tree so a status has work to do.
    git = corpora.Git()
    safe = ("-c", "core.fsmonitor=false", "-c", f"core.hooksPath={(root / '.no-hooks').as_posix()}")
    git.run(*safe, "init", "-q", cwd=root)
    git.run(*safe, "add", "-A", cwd=root)
    git.run(*safe, "-c", "user.name=FND-8", "-c", "user.email=fnd8@example.invalid", "-c", "commit.gpgsign=false",
            "commit", "-q", "-m", "the malicious fixture", cwd=root)
    planted = root / "planted"
    for key, value in (("core.fsmonitor", (planted / ("fsmonitor-hook" + EXE)).as_posix()),
                       ("core.hooksPath", (planted / "hooks").as_posix()),
                       ("core.pager", (planted / ("pager" + EXE)).as_posix()),
                       ("core.sshCommand", (planted / ("ssh-hook" + EXE)).as_posix()),
                       ("diff.external", (planted / ("diff-hook" + EXE)).as_posix()),
                       ("gpg.program", (planted / ("gpg-hook" + EXE)).as_posix()),
                       ("commit.gpgsign", "true")):
        git.run("config", key, value, cwd=root)
    (root / "src" / "main.cpp").write_text((root / "src" / "main.cpp").read_text(encoding="utf-8") +
                                           "// changed in the work tree\n", encoding="utf-8")
    (root / "src" / "untracked.cpp").write_text("int untracked() { return 1; }\n", encoding="utf-8")
    if markers(root):
        raise RuntimeError(f"a canary ran while the fixture was made: {markers(root)}")


class TrustJourney(j1.Journey):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__(args)
        self.repo_tmp: Path | None = None

    def _main(self) -> int:
        args = self.args
        if args.config != "Debug":
            self.evidence.step("setup", ok=False, note=f"the automation endpoint is in Debug builds only, not "
                                                       f"{args.config}")
            return 1
        canary = Path(args.canary)
        if not canary.is_file():
            self.evidence.step("setup", ok=False, note=f"no canary program at {canary}")
            return 1
        self.repo_tmp = Path(tempfile.mkdtemp(prefix="frost-fnd8-"))
        repo = self.repo_tmp / "malicious-repo"

        def setup() -> tuple[bool, Any, Any, str]:
            make_repository(canary, repo)
            return True, None, {"repository": str(repo)}, ""

        if not self.run_step("make the repository", setup):
            return 1
        self.evidence.data["repository"] = str(repo)
        self._launch(repo)
        self._before_trust(repo)
        self._after_trust(repo)
        self._quit()
        return 0 if all(step["ok"] for step in self.evidence.data["steps"]) else 1

    def _records(self) -> list[dict]:
        """The spawn log so far. frost appends to it while it runs, and on Windows a read can meet the
        writer's open handle (a sharing violation, PermissionError): the read is retried until it
        succeeds, for at most READ_RETRY_SECONDS, then the error stands."""
        deadline = time.monotonic() + READ_RETRY_SECONDS
        while True:
            try:
                return spawn_records(self.spawn_dir)
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

    def _before_trust(self, repo: Path) -> None:
        ep = self.endpoint
        replies: dict[str, Any] = {}

        def request(name: str, method: str, **params: Any) -> None:
            try:
                replies[name] = ep.call(method, **params)
            except EndpointError as error:  # A refusal is an outcome here, recorded.
                replies[name] = {"refused": str(error)}

        def open_folder() -> tuple[bool, Any, Any, str]:
            request("open folder", "open", path=str(repo))
            for relative in ("src/main.cpp", "scripts/main.lua", "Assets/Scripts/enemy.lua", "CMakeLists.txt"):
                request("open " + relative, "open", path=str(repo / relative))
            state = ep.call("state")
            return (j1.same_path(state.get("project", ""), repo), {"project": str(repo)},
                    {"project": state.get("project"), "replies": dict(replies)}, "")

        self.run_step("open the folder and its files before trust", open_folder)

        def ask() -> tuple[bool, Any, Any, str]:
            request("Reload CMake Project", "cmake.configure")
            request("build", "cmake.build", action="build")
            request("run", "run.start")
            return True, None, {name: replies[name] for name in ("Reload CMake Project", "build", "run")}, ""

        self.run_step("ask for a configure, a build and a run before trust", ask)

        def open_solution() -> tuple[bool, Any, Any, str]:
            request("open solution", "open", path=str(repo / "Malicious.sln"))
            request("open src/main.cpp again", "open", path=str(repo / "src" / "main.cpp"))
            state = ep.call("state")
            return True, None, {"project": state.get("project"), "open solution": replies["open solution"]}, ""

        self.run_step("open the solution before trust", open_solution)

        def nothing_ran() -> tuple[bool, Any, Any, str]:
            # The CMake service first, then the window: nothing but restricted mode may start while it lasts.
            ep.call("wait", **{"for": "cmake", "timeoutMs": 10000})
            deadline = time.monotonic() + OBSERVE_SECONDS
            while time.monotonic() < deadline:
                time.sleep(POLL_SECONDS)
                if any(restricted_launch_violation(r, repo) for r in self._records()):
                    break
            records = self._records()
            violations = [{"record": r, "why": why} for r in records
                          if (why := restricted_launch_violation(r, repo))]
            planted = markers(repo)
            starts = [r for r in records if r.get("event") == "start"]
            observed = {"restrictedStarts": len(starts),
                        "restrictedExecutables": sorted({r.get("executable", "") for r in starts}),
                        "violations": violations, "markers": planted, "windowSeconds": OBSERVE_SECONDS}
            ok = not violations and not planted
            return (ok, {"violations": [], "markers": []}, observed,
                    "" if ok else f"{len(violations)} launches outside restricted mode, markers {planted}")

        self.run_step("before trust only restricted-mode tools start and no planted program runs", nothing_ran)

    def _after_trust(self, repo: Path) -> None:
        ep = self.endpoint

        def trust() -> tuple[bool, Any, Any, str]:
            ep.call("trust", root=str(repo))
            deadline = time.monotonic() + TRUST_START_SECONDS
            starts: list[dict] = []
            while time.monotonic() < deadline:
                starts = [r for r in self._records() if r.get("event") == "start"]
                if starts:
                    break
                time.sleep(POLL_SECONDS)
            # Wait out what the grant started, then read the whole log once more.
            ep.call("wait", **{"for": "cmake", "timeoutMs": 60000})
            starts = [r for r in self._records() if r.get("event") == "start"]
            root_plants = {os.path.normcase(str(repo / (name + EXE))) for name in ROOT_PLANTS}
            hijacked = [r["executable"] for r in starts
                        if os.path.normcase(os.path.normpath(r.get("executable", ""))) in root_plants]
            executables = sorted({r.get("executable", "") for r in starts})
            return (bool(starts) and not hijacked, {"starts": ">= 1", "rootPlantsStarted": []},
                    {"executables": executables, "rootPlantsStarted": hijacked},
                    "" if starts else f"nothing started within {TRUST_START_SECONDS:.0f} s of the trust grant")

        self.run_step("trusting the root starts tools, none planted in the root", trust)

    def _cleanup(self) -> None:
        super()._cleanup()
        if self.repo_tmp is not None:
            remove_tree(self.repo_tmp)
            self.repo_tmp = None


def remove_tree(path: Path) -> None:
    """Removes `path`, also git's read-only object files on Windows."""
    def writable_and_again(function: Any, name: str, *_: Any) -> None:
        try:
            os.chmod(name, 0o700)
            function(name)
        except OSError:
            pass

    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=writable_and_again)
    else:
        shutil.rmtree(path, onerror=writable_and_again)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frost", required=True, help="the frost image (Debug)")
    parser.add_argument("--config", required=True, help="the ctest configuration")
    parser.add_argument("--canary", required=True, help="the trust_canary image")
    parser.add_argument("--evidence", required=True, help="the directory for the evidence JSON")
    args = parser.parse_args()
    try:
        return TrustJourney(args).main()
    except SystemExit as stop:
        return int(stop.code or 1)


if __name__ == "__main__":
    sys.exit(main())
