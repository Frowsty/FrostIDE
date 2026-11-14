# SPDX-License-Identifier: Apache-2.0
#
# Journey j2_msvc (FND-7b; PLAN.md section 7 R3 and Appendix C): the real FrostIDE (Debug, offscreen,
# on a fresh profile) driven through its automation endpoint (src/app/src/automation_endpoint.hpp,
# ADR 0080) through the four steps of UAT-1 on an MSVC solution: open, edit, build and debug. It
# reuses j1_cmake.py's launch, endpoint client, evidence, spawn-log reading and stall check. Every
# expected value comes from a real tool that ran here (this machine's MSBuild.exe, found with
# vswhere; the program the build made; git), never from a constant. Two solutions:
#
#   --solution synthetic: a scratch copy of tests/project/fixtures/msvc_generated (SLN-0's solution
#     shaped like the user's generated ones: solution folders, property sheets, a sparse .sln matrix
#     and its .slnx twin), whose App/main.cpp the journey writes first (STL containers, two throws),
#     and whose App builds like a Visual Studio Debug configuration (/Od, no inlining) and links with
#     LinkIncremental=false: SLN-0's Debug|x64 compiles /O2, and lldb 23.1.4 cannot read the PDB an
#     incremental relink leaves (ADR 0081 item 8) while the fixture's custom build step relinks App
#     on every build, so F5 would otherwise meet those limits instead of exercising FrostIDE
#     (debuggable());
#   --solution r2a: a shallow scratch clone of the pinned Notepad++ clone (R2a,
#     tests/corpus/corpora.lock.json); the pinned clone is only read.
#
# The steps (synthetic; r2a runs 1, 2, 4-6, Lexilla rebuild and full Notepad++ build/debug below):
#   1. open the solution before trust (.slnx first on synthetic): it opens, its root is untrusted;
#   2. trust it: ProjectHost evaluates every project, and the Solution Explorer's files per project
#      equal the distinct FullPath values `MSBuild.exe <project> -getItem:<every file item type>`
#      prints for the project configuration the solution maps Debug|x64 to (parsed from the .sln or
#      .slnx itself); then the same for the .sln (both formats load);
#   3. clangd runs on the GetClCommandLines database of Debug|x64 (SLN-5); switching the solution
#      configuration to Release|x64 restarts clangd on Release|x64's database (SLN-7), and back;
#   4. edit: a line typed into the editor at the top of a source and saved is on disk;
#   5. Build > Compile (Ctrl+F7) of that file runs MSBuild on it: the error list's build rows equal
#      what MSBuild's own errors-only and warnings-only file loggers record for the same compile
#      (MSBuild.exe -t:ClCompile -p:SelectedFiles=<the item>), the seeded line among them;
#   6. Undo and save put the file back byte for byte, and Compile then succeeds with no build error;
#   7. Rebuild of the solution: the error list equals MSBuild's own record of the same Rebuild; Clean
#      removes the program (the file system says so); Build makes it again;
#   8. debug: Set as Startup Project App; breakpoints on main's first and last lines; Debug (F5)
#      builds App and stops at the first; then a C++ throw filter for std::runtime_error only (the
#      Exceptions list and its type list, DBG-2b): continuing stops once, at the std::runtime_error
#      throw in journeyThrow (the std::logic_error thrown before it must not stop), then at the last
#      line's breakpoint, where Locals show the std::vector's, std::string's and std::map's elements
#      (the values main.cpp builds; lldb-dap's replies); continuing ends the program with the exit
#      code it has when it runs alone (the oracle runs it);
#   9. no UI-thread stall over 250 ms in the run (FND-9b; the freeze reports frost wrote).
# R2a also performs an untouched full Debug|x64 solution Build, including its upstream version-header
# prebuild event, then F5 stops in the actual Notepad++ source and Stop ends the exact owned native
# session. Selected-file ClCompile diagnostics do not establish whether the full Build can link.
#
# The evidence (one JSON file per run, under --evidence), as j1_cmake's. Run by ctest
# (tests/journeys/CMakeLists.txt; Windows only, Visual Studio's MSBuild required):
#   j2_msvc.py --frost <frost image> --config <ctest config> --evidence <directory> --solution synthetic|r2a
# synthetic is label integration (every Windows -Quick leg); r2a is label corpus (FROST_CORPORA_ROOT).
# A non-Debug tree, a missing MSBuild, corpus or tool fails the journey (exit 1); it never skips.

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake as j1  # noqa: E402  (the shared journey machinery, beside this file)
from j1_cmake import REPO, WINDOWS, EndpointError, corpora, same_path, spawn_records  # noqa: E402

JOURNEY = "j2_msvc"

FIXTURE = REPO / "tests" / "project" / "fixtures" / "msvc_generated"
DEBUG_X64 = "Debug|x64"
RELEASE_X64 = "Release|x64"
EVALUATION_SECONDS = 600
CLANGD_SECONDS = 600
COMPILE_SECONDS = 900
BUILD_SECONDS = 3600
DEBUG_SECONDS = 900
POLL_SECONDS = 0.25

# The file item types FrostIDE shows (frost::project::fileItemTypeNames, vcxproj_reader.cpp): the
# question put to MSBuild, whose answer is the expected value.
ITEM_TYPES = ("ClCompile", "ClInclude", "None", "ResourceCompile", "Text", "CustomBuild", "Image", "Natvis",
              "MASM", "Midl", "FxCompile", "Manifest", "CopyFileToFolders", "Xml")

SEED_TEXT = "int frostJourneySeededError = frost_journey_undeclared_identifier;"


def debuggable(project: Path) -> None:
    """Makes the scratch copy's `project` build like a Visual Studio Debug configuration for the
    debugger steps: /Od without inlining (SLN-0's Debug|x64 compiles /O2, so lldb shows main's locals
    as optimised out and its breakpoint line as an inlined frame), and LinkIncremental=false (ADR 0081
    item 8). Both go just before the Microsoft.Cpp.targets import; the file's line endings are kept."""
    text = project.read_bytes().decode("utf-8-sig")
    anchor = '  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />'
    newline = "\r\n" if "\r\n" in text else "\n"
    if text.count(anchor) != 1:
        raise RuntimeError(f"{project} has no single Microsoft.Cpp.targets import")
    lines = ["  <PropertyGroup>", "    <LinkIncremental>false</LinkIncremental>", "  </PropertyGroup>",
             "  <ItemDefinitionGroup>", "    <ClCompile>", "      <Optimization>Disabled</Optimization>",
             "      <InlineFunctionExpansion>Disabled</InlineFunctionExpansion>", "    </ClCompile>",
             "  </ItemDefinitionGroup>"]
    group = newline.join(lines) + newline
    project.write_bytes(text.replace(anchor, group + anchor).encode("utf-8"))


# The synthetic App's program (written into the scratch copy before FrostIDE starts). It throws a
# std::logic_error, then a std::runtime_error, catches both, and returns from the marked line.
APP_MAIN = """\
// j2_msvc's debuggee (tests/journeys/j2_msvc.py writes it into its scratch copy).
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "app.h"

static int journeyThrow(int kind) {
    if (kind == 0)
        throw std::logic_error("j2 logic");
    throw std::runtime_error("j2 runtime");
}

int main() {
    std::vector<int> numbers{3, 1, 4};
    std::string text = "frost-j2";
    std::map<std::string, int> counts{{"one", 1}, {"two", 2}};
    int caught = 0;
    for (int kind = 0; kind < 2; ++kind) {
        try {
            journeyThrow(kind);
        } catch (const std::exception&) {
            ++caught;
        }
    }
    return caught + app() + static_cast<int>(numbers.size() + text.size() + counts.size()) - 13; // J2_BREAK
}
"""
APP_VALUES = {"numbers": ["3", "1", "4"], "text": "frost-j2", "counts": {"one": "1", "two": "2"}}


# --- Oracles: MSBuild.exe, the solution files ----------------------------------------------------


def machine_msbuild() -> str:
    """This machine's MSBuild.exe (64-bit), as vswhere finds Visual Studio's newest instance."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / \
        "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError(f"no vswhere at {vswhere} (Visual Studio is not installed)")
    out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.Component.MSBuild",
                          "-find", r"MSBuild\**\Bin\amd64\MSBuild.exe"], capture_output=True, text=True,
                         check=False, timeout=120)
    found = [line.strip() for line in out.stdout.splitlines() if line.strip().lower().endswith("msbuild.exe")]
    if not found:
        raise RuntimeError("vswhere found no MSBuild.exe")
    return found[0]


MSBUILD_ENV = {"DOTNET_CLI_UI_LANGUAGE": "en-US", "VSLANG": "1033", "MSBUILDTERMINALLOGGER": "off",
               "MSBUILDDISABLENODEREUSE": "1"}


def solution_properties(solution: Path) -> list[str]:
    """The solution properties Visual Studio and FrostIDE pass to a project build or evaluation."""
    return [f"-p:SolutionDir={solution.parent}\\", f"-p:SolutionPath={solution}",
            f"-p:SolutionName={solution.stem}", f"-p:SolutionFileName={solution.name}",
            f"-p:SolutionExt={solution.suffix}"]


def path_key(path: str) -> str:
    return os.path.normcase(os.path.normpath(path)) if os.path.isabs(path) else path.lower().replace("/", "\\")


def msbuild_items(msbuild: str, project: Path, mapping: tuple[str, str], solution: Path) -> list[str]:
    """The distinct FullPath keys of `project`'s file items in `mapping` (configuration, platform)."""
    command = [msbuild, str(project), "-nologo", "-nodeReuse:false", "-getItem:" + ";".join(ITEM_TYPES),
               f"-p:Configuration={mapping[0]}", f"-p:Platform={mapping[1]}", *solution_properties(solution)]
    out = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         cwd=str(project.parent), env=dict(os.environ, **MSBUILD_ENV), check=False, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"MSBuild -getItem failed for {project}: {(out.stdout + out.stderr)[-2000:]}")
    data = json.loads(out.stdout[out.stdout.index("{"):])
    keys = {path_key(item["FullPath"]) for items in data.get("Items", {}).values() for item in items}
    return sorted(keys)


LOGGED = re.compile(r"^\s*(?:\d+>)?\s*(.*?)(?:\((\d+)(?:[,-](\d+))?[^)]*\))?\s*:\s*(fatal error|error|warning)\s+"
                    r"([A-Za-z]+\d+)?\s*:\s*(.*?)(?:\s+\[[^\]]+\])?\s*$")


def parse_logged(line: str) -> tuple | None:
    """One line of MSBuild's file logger, <origin>(<line>,<col>): error|warning <code>: <text> [<project>],
    as (file key, line, column, severity, code, message); tests/build/msbuild_diagnostic_oracle.hpp's rule."""
    match = LOGGED.match(line.rstrip("\r"))
    if not match:
        return None
    return (path_key(match.group(1)), int(match.group(2) or 0), int(match.group(3) or 0),
            "warning" if match.group(4) == "warning" else "error", match.group(5) or "", match.group(6))


def msbuild_logged(msbuild: str, arguments: list[str], cwd: Path, scratch: Path) -> tuple[int, list[tuple]]:
    """Runs MSBuild with its errors-only and warnings-only file loggers; (exit code, sorted diagnostics)."""
    scratch.mkdir(parents=True, exist_ok=True)
    errors, warnings = scratch / "oracle-errors.log", scratch / "oracle-warnings.log"
    command = [msbuild, *arguments, "-nologo", "-nodeReuse:false", "-noconlog",
               f"-flp1:logfile={errors};errorsonly", f"-flp2:logfile={warnings};warningsonly"]
    out = subprocess.run(command, capture_output=True, cwd=str(cwd), env=dict(os.environ, **MSBUILD_ENV),
                         stdin=subprocess.DEVNULL, check=False, timeout=BUILD_SECONDS)
    found: set[tuple] = set()
    for log in (errors, warnings):
        for line in log.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            parsed = parse_logged(line)
            if parsed:
                found.add(parsed)
    return out.returncode, sorted(found)


def shown_rows(problems: list[dict]) -> list[tuple]:
    """The error list's build rows (errors and warnings), in the oracle's form."""
    rows = {(path_key(p.get("origin") or p.get("file") or ""), int(p.get("line") or 0), int(p.get("column") or 0),
             "warning" if p["severity"] == "warning" else "error", p.get("code") or "", p.get("message") or "")
            for p in problems if p.get("source") == "build" and p["severity"] in ("error", "warning")}
    return sorted(rows)


def solution_relative(relative: str) -> str:
    """A project path as a solution file writes it ("Core\\Core.vcxproj"), with "/" separators, so it
    joins as a path on Linux too (where a backslash is part of a file name)."""
    return relative.replace("\\", "/")


def sln_projects(solution: Path) -> dict[str, dict]:
    """{name: {path, guid}} of the C++ projects a .sln lists."""
    projects = {}
    text = solution.read_text(encoding="utf-8-sig", errors="replace")
    for match in re.finditer(r'^Project\("\{[^}]+\}"\)\s*=\s*"([^"]+)",\s*"([^"]+)",\s*"(\{[^}]+\})"', text, re.M):
        name, relative, guid = match.groups()
        if relative.lower().endswith(".vcxproj"):
            projects[name] = {"path": os.path.normpath(solution.parent / solution_relative(relative)),
                              "guid": guid.upper()}
    return projects


def sln_mapping(solution: Path, guid: str, configuration: str) -> tuple[str, str] | None:
    """The project configuration a .sln's ActiveCfg line maps `configuration` to for `guid`."""
    text = solution.read_text(encoding="utf-8-sig", errors="replace")
    pattern = re.escape(guid) + r"\." + re.escape(configuration) + r"\.ActiveCfg\s*=\s*([^|\r\n]+)\|([^\r\n]+)"
    match = re.search(pattern, text, re.I)
    return (match.group(1).strip(), match.group(2).strip()) if match else None


def slnx_projects(solution: Path, configuration: str) -> dict[str, dict]:
    """{name: {path, mapping}} of the C++ projects a .slnx lists, with the project configuration
    `configuration` maps to: the solution's build type and platform (x86 is Win32 for a C++
    project) unless a BuildType or Platform rule of the project names that solution configuration."""
    build_type, platform = configuration.split("|")
    root = ElementTree.parse(solution).getroot()
    projects = {}
    for element in root.iter("Project"):
        relative = element.get("Path", "")
        if not relative.lower().endswith(".vcxproj"):
            continue
        project_type, project_platform = build_type, "Win32" if platform == "x86" else platform
        for rule in element:
            solution_pattern = rule.get("Solution", "*|*")
            rule_type, _, rule_platform = solution_pattern.partition("|")
            if rule_type not in ("*", build_type) or (rule_platform or "*") not in ("*", platform):
                continue
            if rule.tag == "BuildType":
                project_type = rule.get("Project", project_type)
            elif rule.tag == "Platform":
                project_platform = rule.get("Project", project_platform)
        name = Path(solution_relative(relative)).stem
        projects[name] = {"path": os.path.normpath(solution.parent / solution_relative(relative)),
                          "mapping": (project_type, project_platform)}
    return projects


def solution_oracle(solution: Path, configuration: str) -> dict[str, dict]:
    """{name: {path, mapping}} from the solution file itself."""
    if solution.suffix.lower() == ".slnx":
        return slnx_projects(solution, configuration)
    projects = sln_projects(solution)
    return {name: {"path": info["path"], "mapping": sln_mapping(solution, info["guid"], configuration)}
            for name, info in projects.items()}


# --- The journey ---------------------------------------------------------------------------------


class Journey(j1.Journey):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__(args)
        self.restore: list[tuple[Path, bytes]] = []
        self.created: list[Path] = []
        self.scratch: Path | None = None
        self.native_desktop = None
        self.desktop_api = None

    def _main(self) -> int:
        args = self.args
        if args.config != "Debug":
            self.evidence.step("setup", ok=False, note=f"the automation endpoint is in Debug builds only, not "
                                                       f"{args.config}")
            return 1
        if not WINDOWS:
            self.evidence.step("setup", ok=False, note="j2_msvc needs Visual Studio's MSBuild (Windows)")
            return 1
        frost = Path(args.frost)
        self.evidence.data["frost"] = {"image": str(frost),
                                       "sha256": j1.sha256_file(frost) if frost.is_file() else None}
        try:
            self.msbuild = machine_msbuild()
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            self.evidence.step("setup", ok=False, note=str(error))
            return 1
        self.evidence.data["msbuild"] = {"path": self.msbuild, "sha256": j1.sha256_file(Path(self.msbuild))}
        self.scratch = Path(tempfile.mkdtemp(prefix="frost-j2msvc-"))
        if args.solution == "synthetic":
            work = self.scratch / "msvc_generated"
            shutil.copytree(FIXTURE, work)
            (work / "App" / "main.cpp").write_text(APP_MAIN, encoding="utf-8", newline="\r\n")
            debuggable(work / "App" / "App.vcxproj")
            self.evidence.data["solution"] = {"kind": "synthetic", "fixture": str(FIXTURE), "copy": str(work)}
            self._launch(work)
            self._synthetic(work)
        else:
            corpora_root = os.environ.get("FROST_CORPORA_ROOT", "")
            if not corpora_root:
                self.evidence.step("setup", ok=False, note="FROST_CORPORA_ROOT does not name the corpora root")
                return 1
            root = Path(corpora_root)
            lock = corpora.load_lock(REPO / corpora.LOCK_FILE)
            entry = next(item for item in lock["corpora"] if item["id"] == "r2a")
            problem = corpora.problem_or_none(entry, root)
            if problem:
                self.evidence.step("setup", ok=False, note=f"the pinned R2a clone is not ready: {problem}")
                return 1
            pinned = corpora.corpus_dir(root, entry)
            # A shallow clone of the pin into the scratch directory, as R2aCorpus's edit test makes: the
            # pinned clone is only read (other suites share it), and the journey's files are its own
            # (safeWrite keeps a file's owner, so it refuses to save over a pinned file another account,
            # such as an elevated fetch, created).
            clone = self.scratch / "r2a"
            with corpora.BuildLock(corpora.build_lock_path(root, entry)):
                cloned = subprocess.run(["git", "clone", "-q", "--depth", "1", "--no-tags", pinned.as_uri(), str(clone)],
                                        capture_output=True, text=True, check=False, timeout=1800)
            head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(clone), capture_output=True, text=True,
                                  check=False).stdout.strip() if cloned.returncode == 0 else ""
            if head != entry["sha"]:
                self.evidence.step("setup", ok=False, note=f"the scratch clone of R2a failed or is not the pin "
                                                           f"({head or cloned.stderr[-500:]})")
                return 1
            solution = clone / entry["solutions"][0]
            self.evidence.data["solution"] = {"kind": "r2a", "sha": entry["sha"], "pinned": str(pinned),
                                              "path": str(solution)}
            self.clone, self.clone_status = clone, self._git_status(clone)
            self._launch(clone, startupinfo=self._private_desktop_startup())
            self._r2a(clone, solution)
        self._quit()
        return 0 if all(step["ok"] for step in self.evidence.data["steps"]) else 1

    @staticmethod
    def _git_status(clone: Path) -> str:
        return corpora.Git().run("status", "--porcelain", cwd=clone).stdout

    # -- Waiting ------------------------------------------------------------------------------------

    def poll(self, method: str, done: Callable[[dict], bool], seconds: float, **params: Any) -> dict:
        """Calls `method` until `done(reply)` or the deadline; the last reply."""
        deadline = time.monotonic() + seconds
        reply: dict = {}
        while True:
            reply = self.endpoint.call(method, **params)
            if done(reply) or time.monotonic() >= deadline:
                return reply
            time.sleep(POLL_SECONDS)

    def evaluated(self, path: Path, configuration: str | None = None) -> dict:
        """The solution state once `path` is the explorer's solution and every loaded project shows
        ProjectHost's evaluation (of the solution `configuration`, when one is given)."""
        def ready(state: dict) -> bool:
            projects = state.get("projects") or []
            if configuration is not None and state.get("active") != configuration:
                return False
            return (state.get("available") and same_path(state.get("shownSolution") or "", path) and bool(projects)
                    and all(p["evaluation"] and p["evaluation"]["source"] == "ProjectHost"
                            for p in projects if p["loaded"] and p["path"].lower().endswith(".vcxproj")))
        return self.poll("solution", ready, EVALUATION_SECONDS)

    def build_finished(self, action: str, started_after: int) -> dict:
        """The solution state after the build `action` that started after `started_after` spawn records."""
        def finished(state: dict) -> bool:
            if state.get("state") == "NeedsTrust":
                return True  # The build waits on the trust prompt: the step fails now, not after BUILD_SECONDS.
            last = state.get("lastBuild") or {}
            return not state.get("building") and last.get("action") == action and \
                len(spawn_records(self.spawn_dir)) > started_after
        return self.poll("solution", finished, BUILD_SECONDS)

    # -- Steps shared by both solutions -------------------------------------------------------------

    def open_before_trust(self, path: Path, root: Path) -> bool:
        def body() -> tuple[bool, Any, Any, str]:
            self.endpoint.call("open", path=str(path))
            state = self.poll("solution", lambda s: s.get("available") and same_path(s.get("path", ""), path), 120)
            untrusted = [r for r in state.get("untrusted", []) if same_path(r, root)]
            # Before trust nothing evaluates the projects: no ProjectHost or MSBuild.exe item reached
            # the explorer (it shows the project files as written).
            evaluated = sorted(p["name"] for p in state.get("projects", []) if p.get("evaluation"))
            ok = state.get("available") and same_path(state.get("path", ""), path) and bool(untrusted) and \
                state.get("trusted") is False and not evaluated
            return ok, {"path": str(path), "format": path.suffix[1:], "trusted": False, "untrusted": [str(root)],
                        "evaluated": []}, \
                {"path": state.get("path"), "format": state.get("format"), "trusted": state.get("trusted"),
                 "untrusted": state.get("untrusted"), "evaluated": evaluated}, ""
        return self.run_step(f"open {path.name} before trust", body)

    def explorer_equals_msbuild(self, path: Path, label: str) -> bool:
        def body() -> tuple[bool, Any, Any, str]:
            # The toolbar's configuration combo set to Debug|x64, as the oracle's (a solution's first
            # configuration need not be it: Notepad++'s list starts with Debug|ARM64).
            if self.endpoint.call("solution").get("active") != DEBUG_X64:
                self.endpoint.call("solution.configuration", name=DEBUG_X64)
            state = self.evaluated(path, DEBUG_X64)
            oracle_projects = solution_oracle(path, DEBUG_X64)
            shown = {p["name"]: p for p in state.get("projects", [])}
            oracle, observed, differences = {}, {}, {}
            for name, info in sorted(oracle_projects.items()):
                if info["mapping"] is None:
                    continue  # Not in Debug|x64: the explorer shows it as written.
                expected = msbuild_items(self.msbuild, Path(info["path"]), info["mapping"], path)
                project = shown.get(name) or {}
                files = sorted({path_key(f) for f in project.get("files", [])})
                oracle[name] = {"mapping": "|".join(info["mapping"]), "count": len(expected)}
                evaluation = project.get("evaluation") or {}
                observed[name] = {"mapping": f"{evaluation.get('configuration')}|{evaluation.get('platform')}",
                                  "count": len(files), "source": evaluation.get("source")}
                if files != expected:
                    differences[name] = {"missing": sorted(set(expected) - set(files))[:20],
                                         "extra": sorted(set(files) - set(expected))[:20]}
            ok = bool(oracle) and not differences and all(oracle[n]["mapping"] == observed[n]["mapping"]
                                                          for n in oracle)
            if differences:
                observed["differences"] = differences
            return ok, oracle, observed, "" if ok else "the Solution Explorer differs from MSBuild's evaluation"
        return self.run_step(f"{label}: the Solution Explorer's files per project equal MSBuild's evaluation", body)

    def edit_compile_and_undo(self, solution: Path, source: Path, project: Path) -> None:
        ep = self.endpoint
        original = source.read_bytes()
        self.restore.append((source, original))
        facts: dict[str, Any] = {}

        def edit() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(source), line=1, column=1)
            self.poll("editor", lambda e: e.get("available") and same_path(e.get("path", ""), source), 60)
            ep.call("editor.type", text=SEED_TEXT + "\n")
            ep.call("editor.save")
            on_disk = source.read_bytes()
            lines = on_disk.decode("utf-8", "replace").splitlines()
            facts["line"] = next((i + 1 for i, text in enumerate(lines) if SEED_TEXT in text), 0)
            ok = on_disk != original and facts["line"] > 0
            return ok, {"typedLine": SEED_TEXT, "savedToDisk": True}, {"line": facts["line"],
                                                                       "changed": on_disk != original}, ""

        if not self.run_step("edit: a line typed into the editor and saved is on disk", edit):
            return
        identity = os.path.relpath(source, project.parent)

        def compile_seeded() -> tuple[bool, Any, Any, str]:
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.compile", path=str(source))
            state = self.build_finished("Compile", before)
            shown = shown_rows(ep.call("problems")["problems"])
            mapping = facts.get("mapping") or ("Debug", "x64")
            code, oracle = msbuild_logged(self.msbuild, [str(project), "-t:ClCompile", f"-p:SelectedFiles={identity}",
                                                         f"-p:Configuration={mapping[0]}", f"-p:Platform={mapping[1]}",
                                                         *solution_properties(solution)],
                                          project.parent, self.scratch / "oracle-compile")
            last = state.get("lastBuild") or {}
            seeded = any(row[0] == path_key(str(source)) and row[1] == facts["line"] and row[3] == "error"
                         for row in shown)
            ok = last.get("succeeded") is False and code != 0 and shown == oracle and seeded
            return ok, {"exitCode": code, "diagnostics": oracle}, {"lastBuild": last, "diagnostics": shown}, \
                "" if ok else "the error list differs from MSBuild's record of the compile"

        self.run_step("Ctrl+F7: the error list equals MSBuild's record of the compile", compile_seeded)

        def undo_and_compile() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(source), line=1, column=1)
            restored = False
            for _ in range(4):  # Typing may be several undo steps (the line, then its line break).
                ep.call("editor.undo")
                ep.call("editor.save")
                if source.read_bytes() == original:
                    restored = True
                    break
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.compile", path=str(source))
            state = self.build_finished("Compile", before)
            last = state.get("lastBuild") or {}
            errors = [r for r in shown_rows(ep.call("problems")["problems"]) if r[3] == "error"]
            ok = restored and last.get("succeeded") is True and not errors
            return ok, {"restored": True, "succeeded": True, "errors": []}, \
                {"restored": restored, "lastBuild": last, "errors": errors}, ""

        self.run_step("Undo restores the file byte for byte and Compile succeeds", undo_and_compile)

    # -- The synthetic solution --------------------------------------------------------------------

    def _synthetic(self, work: Path) -> None:
        ep = self.endpoint
        slnx, sln = work / "Generated.slnx", work / "Generated.sln"
        if not self.open_before_trust(slnx, work):
            return

        def trust() -> tuple[bool, Any, Any, str]:
            ep.call("trust", root=str(work))
            state = self.evaluated(slnx)
            sources = {p["name"]: (p["evaluation"] or {}).get("source") for p in state.get("projects", [])}
            ok = bool(sources) and all(s == "ProjectHost" for s in sources.values()) and not state.get("untrusted")
            return ok, {"source": "ProjectHost"}, {"sources": sources, "banner": state.get("banner")}, ""

        if not self.run_step("trust: ProjectHost evaluates every project", trust):
            return
        self.explorer_equals_msbuild(slnx, ".slnx")

        def open_sln() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(sln))
            state = self.evaluated(sln)
            return state.get("format") == "sln" and same_path(state.get("path", ""), sln), \
                {"format": "sln"}, {"format": state.get("format"), "path": state.get("path")}, ""

        if not self.run_step("open Generated.sln (the trusted root)", open_sln):
            return
        self.explorer_equals_msbuild(sln, ".sln")
        self._clangd_configurations(work)
        self.edit_compile_and_undo(sln, work / "App" / "app.cpp", work / "App" / "App.vcxproj")
        self._build_rebuild_clean(work, sln)
        self._debug(work)

    def _clangd_configurations(self, work: Path) -> None:
        ep = self.endpoint

        def database_of(state: dict) -> str:
            return (state.get("clangd") or {}).get("compileCommandsDirectory") or ""

        def on(configuration: str) -> Callable[[dict], bool]:
            folder = configuration.replace("|", "_").lower()
            return lambda s: (s.get("clangd") or {}).get("state") == "running" and \
                Path(database_of(s)).name.lower() == folder and Path(database_of(s)).parent.name.lower() == \
                "clcommandlines"

        def debug_database() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(work / "Core" / "src" / "a.cpp"), line=1, column=1)
            state = self.poll("state", on(DEBUG_X64), CLANGD_SECONDS)
            database = Path(database_of(state)) / "compile_commands.json"
            entries = json.loads(database.read_text(encoding="utf-8")) if database.is_file() else []
            listed = any(same_path(e.get("file", ""), work / "Core" / "src" / "a.cpp") for e in entries)
            ok = on(DEBUG_X64)(state) and listed
            return ok, {"database": "clcommandlines/Debug_x64", "listsAcpp": True}, \
                {"clangd": state.get("clangd"), "listsAcpp": listed}, ""

        if not self.run_step("clangd runs on the GetClCommandLines database of Debug|x64", debug_database):
            return

        def switch(configuration: str) -> Callable[[], tuple[bool, Any, Any, str]]:
            def body() -> tuple[bool, Any, Any, str]:
                before = ep.call("state").get("clangd") or {}
                ep.call("solution.configuration", name=configuration)
                state = self.poll("state", lambda s: on(configuration)(s) and
                                  (s.get("clangd") or {}).get("pid") != before.get("pid"), CLANGD_SECONDS)
                active = ep.call("solution").get("active")
                ok = on(configuration)(state) and state["clangd"].get("pid") != before.get("pid") and \
                    active == configuration
                return ok, {"active": configuration, "restarted": True}, \
                    {"active": active, "pidBefore": before.get("pid"), "clangd": state.get("clangd")}, ""
            return body

        self.run_step("switching to Release|x64 restarts clangd on its database", switch(RELEASE_X64))
        self.run_step("switching back to Debug|x64 restarts clangd on its database", switch(DEBUG_X64))

    def _build_rebuild_clean(self, work: Path, sln: Path) -> None:
        ep = self.endpoint
        program = work / "x64" / "Debug" / "App.exe"

        def rebuild() -> tuple[bool, Any, Any, str]:
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.build", action="rebuild")
            state = self.build_finished("Rebuild", before)
            shown = shown_rows(ep.call("problems")["problems"])
            builds = [r for r in spawn_records(self.spawn_dir)[before:] if r.get("event") == "start"
                      and Path(r.get("executable", "")).name.lower() == "msbuild.exe"]
            code, oracle = msbuild_logged(self.msbuild, [str(sln), "-t:Rebuild", "-p:Configuration=Debug",
                                                         "-p:Platform=x64"], work, self.scratch / "oracle-rebuild")
            last = state.get("lastBuild") or {}
            ok = last.get("succeeded") is True and code == 0 and shown == oracle and bool(builds)
            return ok, {"exitCode": code, "diagnostics": oracle, "msbuildStarted": True}, \
                {"lastBuild": last, "diagnostics": shown, "msbuild": [b.get("executable") for b in builds]}, \
                "" if ok else "the Rebuild's error list differs from MSBuild's record"

        self.run_step("Rebuild runs MSBuild.exe; the error list equals MSBuild's record", rebuild)

        def clean() -> tuple[bool, Any, Any, str]:
            existed = program.is_file()
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.build", action="clean")
            state = self.build_finished("Clean", before)
            last = state.get("lastBuild") or {}
            ok = existed and last.get("succeeded") is True and not program.exists()
            return ok, {"programBefore": True, "programAfter": False}, \
                {"programBefore": existed, "programAfter": program.exists(), "lastBuild": last}, ""

        self.run_step("Clean removes the program", clean)

        def build() -> tuple[bool, Any, Any, str]:
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.build", action="build")
            state = self.build_finished("Build", before)
            last = state.get("lastBuild") or {}
            ok = last.get("succeeded") is True and program.is_file()
            return ok, {"succeeded": True, "program": str(program)}, {"lastBuild": last, "program": program.is_file()}, ""

        self.run_step("Build makes the program again", build)

    def _debug(self, work: Path) -> None:
        ep = self.endpoint
        main = work / "App" / "main.cpp"
        program = work / "x64" / "Debug" / "App.exe"
        marker = next(i + 1 for i, text in enumerate(main.read_text(encoding="utf-8").splitlines()) if "J2_BREAK" in text)
        facts: dict[str, Any] = {}

        entry = next(i + 1 for i, text in enumerate(main.read_text(encoding="utf-8").splitlines())
                     if "std::vector<int> numbers" in text)

        def startup_and_breakpoints() -> tuple[bool, Any, Any, str]:
            ep.call("solution.startup", project="App")
            ep.call("debug.breakpoint", path=str(main), line=entry)
            ep.call("debug.breakpoint", path=str(main), line=marker)
            startup = ep.call("solution").get("startup")
            return startup == "App", {"startup": "App"}, {"startup": startup}, ""

        if not self.run_step("Set as Startup Project App; breakpoints on main's first and last lines",
                             startup_and_breakpoints):
            return

        def signature(state: dict) -> tuple | None:
            """What tells one stop from the next: its reason, filter, type and top frame."""
            if state.get("state") != "Stopped":
                return None
            stop = state.get("stop") or {}
            top = (stop.get("frames") or [{}])[0]
            return (stop.get("reason"), stop.get("exceptionFilter"), stop.get("exceptionType"), top.get("name"),
                    top.get("path"), top.get("line"))

        def next_stop(previous: dict) -> dict:
            """The debugger's state at the next stop after `previous` (a new stop signature), or once
            the session has ended with a new outcome (or never started: no session after 60 s)."""
            before, outcome = signature(previous), previous.get("outcome")
            started = time.monotonic()

            def done(state: dict) -> bool:
                if state.get("state") == "Stopped" and signature(state) != before:
                    return True
                return not state.get("session") and (state.get("outcome") != outcome or
                                                     time.monotonic() - started > 60)
            return self.poll("debug.state", done, DEBUG_SECONDS)

        def stop_of(state: dict) -> dict:
            stop = state.get("stop") or {}
            return {"state": state.get("state"), "reason": stop.get("reason"), "filter": stop.get("exceptionFilter"),
                    "type": stop.get("exceptionType"), "description": stop.get("description"),
                    "frames": [(f.get("name"), Path(f.get("path") or "").name, f.get("line"))
                               for f in stop.get("frames", [])][:8], "status": state.get("status"),
                    "session": state.get("session"), "outcome": state.get("outcome"),
                    "breakpoints": [(Path(b.get("path") or "").name, b.get("line"), b.get("status"), b.get("message"))
                                    for b in state.get("breakpoints", [])],
                    "console": (state.get("console") or "")[-600:]}

        def at_line(state: dict, line: int) -> bool:
            stop = state.get("stop") or {}
            top = (stop.get("frames") or [{}])[0]
            return stop.get("reason") == "breakpoint" and same_path(top.get("path") or "", main) and \
                top.get("line") == line

        def f5_stops_at_entry() -> tuple[bool, Any, Any, str]:
            before = ep.call("debug.state")
            ep.call("run.debug")  # The toolbar's Debug, F5's action: builds the startup project, then debugs.
            state = facts["state"] = next_stop(before)
            return at_line(state, entry), {"reason": "breakpoint", "line": entry}, stop_of(state), ""

        if not self.run_step("F5 builds and debugs App: it stops at the first breakpoint", f5_stops_at_entry):
            self._end_debug()
            return

        def first_stop() -> tuple[bool, Any, Any, str]:
            # The C++ throw filter, restricted to std::runtime_error (the Exceptions list's check box
            # and its Edit dialog's type list, DBG-2b); the session now knows its filters.
            ep.call("debug.exception", filter="cpp_throw", enabled=True, types=["std::runtime_error"])
            ep.call("debug.continue")
            state = facts["state"] = next_stop(facts["state"])
            stop = state.get("stop") or {}
            frames = [f.get("name") or "" for f in stop.get("frames", [])]
            ok = stop.get("reason") == "exception" and stop.get("exceptionFilter") == "cpp_throw" and \
                stop.get("exceptionType") == "std::runtime_error" and any("journeyThrow" in f for f in frames)
            return ok, {"reason": "exception", "filter": "cpp_throw", "type": "std::runtime_error",
                        "inFrame": "journeyThrow"}, stop_of(state), ""

        if not self.run_step("the throw filter stops only for std::runtime_error (not the std::logic_error)",
                             first_stop):
            self._end_debug()
            return

        def breakpoint_stop() -> tuple[bool, Any, Any, str]:
            ep.call("debug.continue")
            state = facts["state"] = next_stop(facts["state"])
            return at_line(state, marker), {"reason": "breakpoint", "line": marker}, stop_of(state), ""

        if not self.run_step("the breakpoint stops on main's last line", breakpoint_stop):
            self._end_debug()
            return

        def locals_show_elements() -> tuple[bool, Any, Any, str]:
            variables = {v["name"]: v for v in ep.call("debug.locals")["variables"]}
            numbers = [c.get("value") for c in variables.get("numbers", {}).get("children", [])]
            text = variables.get("text", {}).get("value", "")
            # A map element shows its pair's type until it is expanded (ADR 0081, the DBG-4a gaps), so each
            # [i] row is expanded as a click on it would: its "first" and "second" children.
            counts: dict[str, str] = {}
            for element in variables.get("counts", {}).get("children", []):
                if (element.get("reference") or 0) <= 0:
                    continue
                pair = {c["name"]: c.get("value") for c in
                        ep.call("debug.variables", reference=element["reference"])["variables"]}
                counts[(pair.get("first") or "").strip('"')] = pair.get("second") or ""
            ok = numbers == APP_VALUES["numbers"] and text.strip('"') == APP_VALUES["text"] and \
                counts == APP_VALUES["counts"]
            facts["locals"] = variables
            shown = {name: {"value": v.get("value"), "type": v.get("type"), "children": len(v.get("children", []))}
                     for name, v in variables.items()}
            return ok, APP_VALUES, {"numbers": numbers, "text": text.strip('"'), "counts": counts, "locals": shown}, ""

        self.run_step("Locals show the std::vector's, std::string's and std::map's elements", locals_show_elements)

        def runs_to_the_end() -> tuple[bool, Any, Any, str]:
            ep.call("debug.continue")
            state = next_stop(facts["state"])
            alone = subprocess.run([str(program)], capture_output=True, cwd=str(program.parent),
                                   stdin=subprocess.DEVNULL, check=False, timeout=120)
            exit_code = (state.get("outcome") or {}).get("exitCode")
            ok = not state.get("session") and exit_code == alone.returncode
            return ok, {"exitCode": alone.returncode}, {"exitCode": exit_code, "state": state}, ""

        self.run_step("continuing ends the program with the exit code it has alone", runs_to_the_end)
        self._end_debug()

    def _end_debug(self) -> None:
        try:
            state = self.endpoint.call("debug.state")
            if state.get("session") and state.get("state") != "Ended":
                self.endpoint.call("debug.stop")
                self.poll("debug.state", lambda s: not s.get("session") or s.get("state") == "Ended", 120)
        except (EndpointError, OSError):
            pass

    # -- R2a -----------------------------------------------------------------------------------------

    def _r2a(self, clone: Path, solution: Path) -> None:
        ep = self.endpoint
        if not self.open_before_trust(solution, solution.parent):
            return

        def trust() -> tuple[bool, Any, Any, str]:
            # The trust dialog's Trust: every root it lists (the solution's folder and the folders of
            # projects outside it, here lexilla/src and scintilla/win32), not only the solution's.
            roots = [str(solution.parent)] + [r for r in ep.call("solution").get("untrusted", [])
                                              if not same_path(r, solution.parent)]
            for root in roots:
                ep.call("trust", root=root)
            state = self.evaluated(solution)
            sources = {p["name"]: (p["evaluation"] or {}).get("source") for p in state.get("projects", [])}
            ok = bool(sources) and all(s == "ProjectHost" for s in sources.values()) and \
                state.get("trusted") is True and not state.get("untrusted")
            observed = {"sources": sources, "banner": state.get("banner"), "roots": roots,
                        "trusted": state.get("trusted"), "untrusted": state.get("untrusted")}
            return ok, {"source": "ProjectHost", "trusted": True, "untrusted": []}, observed, ""

        if not self.run_step("trust: ProjectHost evaluates every project", trust):
            return
        self.explorer_equals_msbuild(solution, ".sln")
        notepad = solution.parent / "notepadPlus.vcxproj"
        self.edit_compile_and_undo(solution, clone / "PowerEditor" / "src" / "MISC" / "Common" / "SortLocale.cpp",
                                   notepad)
        projects = sln_projects(solution)
        lexilla = Path(projects["Lexilla"]["path"])
        mapping = sln_mapping(solution, projects["Lexilla"]["guid"], DEBUG_X64) or ("Debug", "x64")

        def rebuild_project() -> tuple[bool, Any, Any, str]:
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.build", action="rebuild", project="Lexilla")
            state = self.build_finished("Rebuild", before)
            shown = shown_rows(ep.call("problems")["problems"])
            code, oracle = msbuild_logged(self.msbuild, [str(lexilla), "-t:Rebuild", f"-p:Configuration={mapping[0]}",
                                                         f"-p:Platform={mapping[1]}", *solution_properties(solution)],
                                          lexilla.parent, self.scratch / "oracle-lexilla")
            last = state.get("lastBuild") or {}
            ok = last.get("succeeded") is (code == 0) and shown == oracle
            return ok, {"exitCode": code, "diagnostics": oracle}, {"lastBuild": last, "diagnostics": shown}, \
                "" if ok else "the error list differs from MSBuild's record of the Rebuild"

        self.run_step("Rebuild of Lexilla: the error list equals MSBuild's record", rebuild_project)
        if not self._build_and_debug_r2a(clone, solution):
            return

        def clone_unchanged() -> tuple[bool, Any, Any, str]:
            self._restore_files()
            status = self._git_status(clone)
            return status == self.clone_status, {"gitStatus": self.clone_status}, {"gitStatus": status}, ""

        self.run_step("the clone has no change git sees (outputs are ignored)", clone_unchanged)

    # -- Cleanup ---------------------------------------------------------------------------------------

    def _private_desktop_startup(self) -> Any:
        self.desktop_api = ctypes.WinDLL("user32", use_last_error=True)
        self.desktop_api.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p,
                                                   wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
        self.desktop_api.CreateDesktopW.restype = wintypes.HANDLE
        self.desktop_api.CloseDesktop.argtypes = [wintypes.HANDLE]
        self.desktop_api.CloseDesktop.restype = wintypes.BOOL
        name = "frost-j2-r2a-" + uuid.uuid4().hex
        # Read objects, create windows/menus and write objects, as the existing C++ PrivateDesktop.
        self.native_desktop = self.desktop_api.CreateDesktopW(name, None, None, 0, 0x0087, None)
        if not self.native_desktop:
            raise ctypes.WinError(ctypes.get_last_error())
        info = subprocess.STARTUPINFO()
        info.lpDesktop = name
        self.evidence.data["nativeDesktop"] = name
        return info

    def _build_and_debug_r2a(self, clone: Path, solution: Path) -> bool:
        ep = self.endpoint
        program = solution.parent / "x64" / "Debug" / "notepad++.exe"
        pdb = program.with_name("notepadPlus.pdb")
        source = clone / "PowerEditor" / "src" / "winmain.cpp"

        def full_build() -> tuple[bool, Any, Any, str]:
            ep.call("solution.configuration", name=DEBUG_X64)
            selected = self.evaluated(solution, DEBUG_X64)
            before = len(spawn_records(self.spawn_dir))
            ep.call("solution.build", action="build")
            state = self.build_finished("Build", before)
            code, diagnostics = msbuild_logged(self.msbuild, [str(solution), "-t:Build", "-p:Configuration=Debug",
                                                             "-p:Platform=x64"], solution.parent,
                                               self.scratch / "oracle-notepad-full-build")
            ok = (selected.get("active") == DEBUG_X64 and (state.get("lastBuild") or {}).get("succeeded") is True
                  and code == 0 and program.is_file() and pdb.is_file())
            observed = {"lastBuild": state.get("lastBuild"), "oracleExit": code, "diagnostics": diagnostics,
                        "executable": str(program), "pdb": str(pdb),
                        "generatedVersionHeader": (clone / "PowerEditor" / "src" / "NppLibsVersion.h").is_file()}
            if ok:
                observed.update(executableSha256=j1.sha256_file(program), pdbSha256=j1.sha256_file(pdb))
            return ok and observed["generatedVersionHeader"], {"configuration": DEBUG_X64, "fullBuildSucceeded": True}, observed, ""

        if not self.run_step("full untouched pinned Notepad++ Debug|x64 Build produces its real executable and PDB",
                             full_build):
            self.evidence.data["debug"] = {"status": "not started", "reason": "the actual full Build gate failed"}
            return False

        profile = self.scratch / "notepad-profile"
        for directory in ("settings", "appdata", "localappdata", "userprofile"):
            (profile / directory).mkdir(parents=True, exist_ok=False)
        user = solution.parent / "notepadPlus.vcxproj.user"
        if user.exists():
            raise RuntimeError("refusing to overwrite existing Notepad++ debugger user settings")
        ns = "http://schemas.microsoft.com/developer/msbuild/2003"
        document = ElementTree.Element("Project", xmlns=ns)
        properties = ElementTree.SubElement(document, "PropertyGroup", Condition="'$(Configuration)|$(Platform)'=='Debug|x64'")
        arguments = ["-multiInst", "-nosession", "-noPlugin", "-settingsDir=" + str(profile / "settings")]
        ElementTree.SubElement(properties, "LocalDebuggerCommandArguments").text = subprocess.list2cmdline(arguments)
        ElementTree.SubElement(properties, "LocalDebuggerEnvironment").text = "\n".join(
            f"{name}={profile / directory}" for name, directory in
            (("APPDATA", "appdata"), ("LOCALAPPDATA", "localappdata"), ("USERPROFILE", "userprofile")))
        ElementTree.ElementTree(document).write(user, encoding="utf-8", xml_declaration=True)

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                     ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME),
                                           ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
                                           ctypes.POINTER(wintypes.FILETIME)]
        kernel.GetProcessTimes.restype = wintypes.BOOL
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateProcess.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        owned_handles: list[Any] = []

        def handle_identity(handle: Any) -> dict:
            image, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
            if not kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)) or not \
                    kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                           ctypes.byref(system), ctypes.byref(user)):
                raise ctypes.WinError(ctypes.get_last_error())
            return {"image": image.value, "createdFileTime": (created.dwHighDateTime << 32) | created.dwLowDateTime}

        def process_identity(pid: int) -> dict | None:
            handle = kernel.OpenProcess(0x100000 | 0x1000, False, pid)
            if not handle:
                return None
            try:
                identity = handle_identity(handle)
                script = (f"$p=Get-CimInstance Win32_Process -Filter 'ProcessId = {int(pid)}'; if($p){{"
                          "[pscustomobject]@{pid=$p.ProcessId;parent=$p.ParentProcessId} | ConvertTo-Json -Compress}")
                powershell = j1.system_tool(r"WindowsPowerShell\v1.0\powershell.exe")
                result = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-Command", script],
                                        capture_output=True, text=True, errors="replace", check=False, timeout=30)
                if result.returncode:
                    raise RuntimeError("cannot identify the native debugger process: " + result.stderr)
                # The retained process must still be alive after the PID query, preventing reuse
                # between the native identity snapshot and the CIM parent lookup.
                if kernel.WaitForSingleObject(handle, 0) != 0x00000102 or not result.stdout.strip():
                    return None
                identity.update(json.loads(result.stdout))
                return identity
            finally:
                kernel.CloseHandle(handle)

        def adopt_process(identity: dict) -> None:
            handle = kernel.OpenProcess(0x100000 | 0x1000 | 0x0001, False, identity["pid"])
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                current = handle_identity(handle)
                if current["createdFileTime"] != identity["createdFileTime"] or \
                        not same_path(current["image"], identity["image"]):
                    raise RuntimeError("native process identity changed before ownership was retained")
            except BaseException:
                kernel.CloseHandle(handle)
                raise
            owned_handles.append(handle)

        facts: dict[str, Any] = {}

        def stopped_at_entry() -> tuple[bool, Any, Any, str]:
            ep.call("solution.startup", project="Notepad++")
            ep.call("debug.breakpoint", path=str(source), line=641)
            before = len(spawn_records(self.spawn_dir))
            ep.call("run.debug")
            state = self.poll("debug.state", lambda value: value.get("state") == "Stopped" or
                              (value.get("outcome") or {}).get("buildFailed") is True, 240)
            frames = (state.get("stop") or {}).get("frames") or []
            top = frames[0] if frames else {}
            verified = any(same_path(point.get("path", ""), source) and point.get("line") == 641
                           and point.get("status") == "verified" for point in state.get("breakpoints", []))
            pid = state.get("processId")
            child = process_identity(pid) if isinstance(pid, int) and pid > 0 else None
            adapter = process_identity(child["parent"]) if child else None
            starts = [record for record in spawn_records(self.spawn_dir)[before:]
                      if record.get("event") == "start" and adapter and record.get("pid") == adapter["pid"]
                      and same_path(record.get("executable", ""), adapter.get("image", ""))]
            ok = (state.get("state") == "Stopped" and (state.get("stop") or {}).get("reason") == "breakpoint"
                  and verified and same_path(top.get("path", ""), source)
                  and top.get("line") == 641 and "wWinMain" in top.get("name", "") and child is not None
                  and same_path(child.get("image", ""), program) and adapter is not None
                  and Path(adapter.get("image") or "").name.lower() == "lldb-dap.exe" and bool(starts))
            facts.update(state=state, child=child, adapter=adapter)
            if child and same_path(child.get("image", ""), program):
                adopt_process(child)
                if adapter and Path(adapter.get("image") or "").name.lower() == "lldb-dap.exe" and starts:
                    adopt_process(adapter)
            self.evidence.data["debug"] = {"state": state, "child": child, "adapter": adapter,
                                            "arguments": arguments, "settings": str(profile / "settings")}
            return ok, {"verifiedSource": str(source), "line": 641, "ownedProgram": str(program)}, facts.copy(), ""

        try:
            if not self.run_step("F5 stops in actual pinned Notepad++ with its exact owned debuggee and adapter",
                                 stopped_at_entry):
                return False

            def locals_and_continue() -> tuple[bool, Any, Any, str]:
                variables = ep.call("debug.locals")["variables"]
                argument = next((item for item in variables if item.get("name") == "pCmdLine"), None)
                if not argument or "multiInst" not in argument.get("value", ""):
                    return False, {"pCmdLineContains": "multiInst"}, {"variables": variables}, ""
                ep.call("debug.continue")
                state = self.poll("debug.state", lambda value: value.get("state") in ("Running", "Ended"), 120)
                child = process_identity(facts["child"]["pid"])
                ok = (state.get("session") is True and state.get("state") == "Running" and child is not None
                      and child.get("createdFileTime") == facts["child"]["createdFileTime"]
                      and same_path(child.get("image", ""), program))
                return ok, {"pCmdLineContains": "multiInst", "sameNativeProcessRunning": True}, {
                    "argument": argument, "state": state, "child": child}, ""

            if not self.run_step("Locals shows real Notepad++ arguments and Continue runs the same owned instance",
                                 locals_and_continue):
                return False

            def stopped_owned_processes() -> tuple[bool, Any, Any, str]:
                ep.call("debug.stop")
                state = self.poll("debug.state", lambda value: not value.get("session"), 120)
                waits = [kernel.WaitForSingleObject(handle, 10000) for handle in owned_handles]
                ok = not state.get("session") and len(waits) == 2 and all(status == 0 for status in waits)
                return ok, {"ownedProcessesExited": True}, {"state": state, "ownedHandleWaits": waits}, ""

            return self.run_step("Stop ends the exact Notepad++ debuggee and its owned adapter", stopped_owned_processes)
        finally:
            try:
                self._end_debug()
            finally:
                # Handles are adopted only after image and creation identity checks. They cannot
                # become unrelated processes through PID reuse, including if DAP Stop failed.
                cleanup_failures = []
                for handle in owned_handles:
                    try:
                        status = kernel.WaitForSingleObject(handle, 10000)
                        if status == 0x00000102:
                            terminated = bool(kernel.TerminateProcess(handle, 99))
                            status = kernel.WaitForSingleObject(handle, 10000)
                            if not terminated:
                                cleanup_failures.append("TerminateProcess failed for an owned native handle")
                        if status != 0:
                            cleanup_failures.append(f"owned native process cleanup wait returned {status}")
                    finally:
                        if not kernel.CloseHandle(handle):
                            cleanup_failures.append("CloseHandle failed for an owned native process")
                if cleanup_failures:
                    self.evidence.step("owned native cleanup", ok=False, observed=cleanup_failures)
                    raise RuntimeError("; ".join(cleanup_failures))

    def _restore_files(self) -> None:
        for path, original in self.restore:
            if path.is_file() and path.read_bytes() != original:
                path.write_bytes(original)

    def _cleanup(self) -> None:
        super()._cleanup()  # frost and its tree end first, so nothing holds the files below.
        desktop_error = None
        if self.native_desktop:
            if not self.desktop_api.CloseDesktop(self.native_desktop):
                desktop_error = str(ctypes.WinError(ctypes.get_last_error()))
                self.evidence.step("native desktop cleanup", ok=False, note=desktop_error)
            self.native_desktop = None
        self._restore_files()
        for created in self.created:
            shutil.rmtree(created, ignore_errors=True)
        self.created = []
        if self.scratch is not None:
            shutil.rmtree(self.scratch, ignore_errors=True)
            self.scratch = None
        if desktop_error:
            # j1.main has already computed its return value before running cleanup. Persist and
            # raise here so cleanup failure cannot leave a successful journey exit code.
            self.evidence.write()
            raise RuntimeError(desktop_error)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frost", required=True, help="the frost image (Debug)")
    parser.add_argument("--config", required=True, help="the ctest configuration")
    parser.add_argument("--evidence", required=True, help="the directory for the evidence JSON")
    parser.add_argument("--solution", required=True, choices=("synthetic", "r2a"), help="which solution")
    args = parser.parse_args()
    j1.JOURNEY = JOURNEY  # The evidence file's name and its "journey" field (j1_cmake.Evidence).
    try:
        return Journey(args).main()
    except SystemExit as stop:
        return int(stop.code or 1)


if __name__ == "__main__":
    sys.exit(main())
