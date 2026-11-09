# SPDX-License-Identifier: Apache-2.0
#
# Journey j2_run (RUN-2/RUN-3; PLAN.md Appendix C R2): the real FrostIDE (Debug, offscreen, on a
# fresh profile) driven through its automation endpoint (src/app/src/automation_endpoint.hpp, ADR
# 0080) against the pinned FrostEngine corpus with the pinned cmake and Ninja, the corpus tier. It
# reuses j1_cmake.py's launch, endpoint client, evidence and spawn-log reading. Every expected value
# comes from an oracle that ran here, never from a constant:
#   1. open the corpus folder, select the platform's preset and trust it; the configure finishes
#      (cmake's exit code 0 and the File API model loaded);
#   2. the run configurations FrostIDE generates are the EXECUTABLE targets of the File API reply
#      cmake wrote (the oracle reads the reply's JSON itself: index -> codemodel -> target files);
#   3. FrostEditor's configuration is selected and edited as the Run/Debug Configurations dialog's
#      OK does (arguments --offscreen --no-automation --config-dir <a scratch folder>), and the edit
#      is in the workspace store;
#   3b. FrostIDE restarts (UAT-1 step 2): frost quits through the endpoint, exits with 0 and leaves
#      no process of its tree; the real frost starts again on the same profile, opens the corpus as
#      the first start did, and lists FrostEditor's configuration with the edited arguments (the
#      oracle: the arguments the journey wrote, read back from the store file on disk);
#   4. Run on the toolbar builds FrostEditor (once, under the corpus build lock) and starts it: the
#      spawn log has a start record of the artifact the reply names, the RunManager reports its
#      process, and the OS process table shows that process running that image;
#   5. its output streams into its tab of the Run tool window while the process is still running
#      (the OS table says so after the text arrived);
#   6. Stop on the toolbar ends the whole tree: every process the OS table showed below FrostEditor
#      (and FrostEditor itself), identified by pid and creation time, is gone from a fresh table.
#
# The evidence (one JSON file per run, under --evidence), as j1_cmake's. Run by ctest
# (tests/journeys/CMakeLists.txt, label corpus: verify -Corpus only):
#   j2_run.py --frost <frost image> --config <ctest config> --evidence <directory>
# FROST_CORPORA_ROOT names the corpora root. A missing corpus, a missing pinned engine configuration,
# a non-Debug tree or a missing tool fails the journey (exit 1); it never skips.

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import j1_cmake as j1  # noqa: E402  (the shared journey machinery, beside this file)
from j1_cmake import REPO, WINDOWS, corpora, same_path, spawn_records  # noqa: E402

JOURNEY = "j2_run"
# j1_cmake's Evidence and step printing name the journey through its module global.
j1.JOURNEY = JOURNEY

TARGET = "FrostEditor"
CONFIGURE_SECONDS = 3600
BUILD_AND_START_SECONDS = 4 * 3600  # The first run builds FrostEditor and the engine it links.
OUTPUT_SECONDS = 300
STOP_SECONDS = 120
QUIT_SECONDS = 120  # The restart step: frost's own exit after the endpoint's quit.
POLL_SECONDS = 0.25


# --- The File API reply oracle --------------------------------------------------------------------


def reply_executables(build_dir: Path) -> dict[str, list[str]]:
    """{name: [artifact paths, absolute]} of the EXECUTABLE targets in the newest reply of `build_dir`."""
    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise RuntimeError(f"no File API reply index in {reply}")
    index = json.loads(indexes[-1].read_text(encoding="utf-8"))
    codemodel = next((o["jsonFile"] for o in index.get("objects", []) if o.get("kind") == "codemodel"), None)
    if not codemodel:
        raise RuntimeError(f"{indexes[-1]} lists no codemodel")
    model = json.loads((reply / codemodel).read_text(encoding="utf-8"))
    executables: dict[str, list[str]] = {}
    for target in model["configurations"][0].get("targets", []):
        data = json.loads((reply / target["jsonFile"]).read_text(encoding="utf-8"))
        if data.get("type") == "EXECUTABLE":
            executables[data["name"]] = [str((build_dir / a["path"]).resolve()) if not Path(a["path"]).is_absolute()
                                         else a["path"] for a in data.get("artifacts", [])]
    return executables


def program_of(artifacts: list[str]) -> str:
    """The artifact a run starts: on Windows the first .exe, else the first that is not a .pdb."""
    if WINDOWS:
        for artifact in artifacts:
            if artifact.lower().endswith(".exe"):
                return artifact
        for artifact in artifacts:
            if not artifact.lower().endswith(".pdb"):
                return artifact
        return ""
    return artifacts[0] if artifacts else ""


# --- The OS process table -------------------------------------------------------------------------


def process_table() -> dict[int, dict]:
    """{pid: {parent, name, created}} from the OS (Win32_Process through CIM; /proc on Linux)."""
    table: dict[int, dict] = {}
    if WINDOWS:
        script = ("Get-CimInstance Win32_Process | ForEach-Object { [pscustomobject]@{ pid = $_.ProcessId; "
                  "parent = $_.ParentProcessId; name = $_.Name; "
                  "created = $_.CreationDate.ToUniversalTime().ToString('o') } } | ConvertTo-Json -Compress")
        powershell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "WindowsPowerShell",
                                  "v1.0", "powershell.exe")
        out = subprocess.run([powershell, "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True,
                             text=True, errors="replace", check=False, timeout=120)
        rows = json.loads(out.stdout or "[]")
        for row in rows if isinstance(rows, list) else [rows]:
            table[int(row["pid"])] = {"parent": int(row["parent"]), "name": row["name"], "created": row["created"]}
        return table
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        name = stat[stat.index("(") + 1:stat.rindex(")")]
        fields = stat[stat.rindex(")") + 2:].split()
        if fields[0] == "Z":
            continue  # A zombie has ended.
        table[int(entry.name)] = {"parent": int(fields[1]), "name": name, "created": fields[19]}
    return table


def tree_below(root: int, table: dict[int, dict]) -> dict[int, dict]:
    """`root` and its descendants. On Windows a parent link counts only when the child is younger than
    the parent (Windows keeps a stale parent id after the parent exits and its pid is reused)."""
    tree = {root: table[root]} if root in table else {}
    changed = True
    while changed:
        changed = False
        for pid, row in table.items():
            if pid in tree or row["parent"] not in tree:
                continue
            if WINDOWS and row["created"] < tree[row["parent"]]["created"]:
                continue
            tree[pid] = row
            changed = True
    return tree


# --- The journey ---------------------------------------------------------------------------------


class Journey(j1.Journey):
    def _main(self) -> int:
        args = self.args
        if args.config != "Debug":
            self.evidence.step("setup", ok=False, note=f"the automation endpoint is in Debug builds only, not "
                                                       f"{args.config}")
            return 1
        corpora_root = os.environ.get("FROST_CORPORA_ROOT", "")
        if not corpora_root:
            self.evidence.step("setup", ok=False, note="FROST_CORPORA_ROOT does not name the corpora root")
            return 1
        root = Path(corpora_root)
        lock = corpora.load_lock(REPO / corpora.LOCK_FILE)
        entry = next(item for item in lock["corpora"] if item["id"] == "frostengine")
        corpus = corpora.corpus_dir(root, entry)
        preset = corpora.engine_preset(entry, None)
        self.evidence.data["corpus"] = {"id": entry["id"], "sha": entry["sha"], "path": str(corpus),
                                        "preset": preset}
        frost = Path(args.frost)
        self.evidence.data["frost"] = {"image": str(frost),
                                       "sha256": j1.sha256_file(frost) if frost.is_file() else None}
        try:
            cmake = corpora.pinned_tool("cmake", REPO)
            ninja = corpora.pinned_tool("ninja", REPO)
        except RuntimeError as error:
            self.evidence.step("setup", ok=False, note=str(error))
            return 1
        problem = corpora.problem_or_none(entry, root) or corpora.engine_problem(
            corpus, entry, preset, {"cmake": cmake, "ninja": ninja})
        if problem:
            self.evidence.step("setup", ok=False, note=f"the pinned engine is not ready: {problem} "
                                                       f"(tools/corpora.py fetch; engine --configure)")
            return 1
        # The run builds FrostEditor in the shared clone: under its build lock, like every corpus build.
        with corpora.BuildLock(corpora.build_lock_path(root, entry)):
            # The edit lands in the clone's workspace store; the cleanup puts the store back as it was.
            self.store = corpus / ".frost" / "runConfigurations" / "configurations.json"
            self.store_before = self.store.read_bytes() if self.store.is_file() else None
            self.frost_dir_before = (corpus / ".frost").exists()
            self._launch(corpus)
            try:
                self._run_journey(corpus, preset)
            finally:
                self._stop_runs()
                self._quit()
        return 0 if all(step["ok"] for step in self.evidence.data["steps"]) else 1

    def _stop_runs(self) -> None:
        """Whatever happened, no run outlives the journey (Stop, then the cleanup's kill_tree)."""
        if self.endpoint is None:
            return
        try:
            if self.endpoint.call("run.state").get("running"):
                self.endpoint.call("run.stop")
        except (j1.EndpointError, OSError):
            pass

    def _run_journey(self, corpus: Path, preset: str) -> None:
        build_dir = corpus / "build" / preset
        editor_config = Path(tempfile.mkdtemp(prefix="frost-j2-editor-"))
        self.editor_config = editor_config
        facts: dict[str, Any] = {}

        def open_and_configure() -> tuple[bool, Any, Any, str]:
            self.endpoint.call("open", path=str(corpus))
            opened = self.endpoint.call("state")["cmake"]
            if not opened["available"]:
                return False, None, None, "the folder did not open as a CMake project"
            # After a restart the profile already has the preset and may be configuring with it.
            if opened.get("activeProfile") != preset:
                self.endpoint.call("cmake.select", name=preset)
            self.endpoint.call("trust", root=str(corpus))

            def configured(state: dict) -> bool:
                last = state["cmake"].get("lastConfigure")
                return (bool(last) and last.get("profile") == state["cmake"].get("activeProfile")
                        and state["cmake"].get("modelLoaded") is True)

            state = self.wait_cmake(120, configured)
            if not configured(state):
                self.endpoint.call("cmake.configure")
                state = self.wait_cmake(CONFIGURE_SECONDS, configured)
            # The CMake Targets window shows the model Run builds (it follows on the UI queue).
            deadline = time.monotonic() + 120
            while configured(state) and not state["cmake"].get("targetsCurrent") and time.monotonic() < deadline:
                time.sleep(POLL_SECONDS)
                state = self.endpoint.call("state")
            last = state.get("cmake", {}).get("lastConfigure") or {}
            ok = configured(state) and state["cmake"].get("targetsCurrent") is True and last.get("exitCode") == 0 and same_path(
                state["cmake"].get("buildDirectory", ""), build_dir)
            return ok, {"exitCode": 0, "buildDirectory": str(build_dir)}, state.get("cmake"), ""

        if not self.run_step("open, trust and configure the corpus", open_and_configure):
            return

        def configurations() -> tuple[bool, Any, Any, str]:
            oracle = reply_executables(build_dir)
            facts["executables"] = oracle
            deadline = time.monotonic() + 120
            observed: list[str] = []
            while time.monotonic() < deadline:
                listed = self.endpoint.call("run.configurations")["configurations"]
                observed = sorted(c["target"] for c in listed if c["type"] == "cmake-target" and c["generated"])
                if len(observed) == len(oracle):
                    break
                time.sleep(POLL_SECONDS)  # The configuration list follows the published model.
            expected = sorted(oracle)
            return (observed == expected, {"count": len(expected), "targets": expected},
                    {"count": len(observed), "targets": observed},
                    "" if observed == expected else "the configurations differ from the reply's EXECUTABLE targets")

        if not self.run_step("one run configuration per EXECUTABLE target of the File API reply", configurations):
            return
        if TARGET not in facts["executables"]:
            self.evidence.step("setup", ok=False, note=f"the reply has no {TARGET} executable target")
            return
        program = program_of(facts["executables"][TARGET])
        arguments = ["--offscreen", "--no-automation", "--config-dir", str(editor_config)]

        def select_and_edit() -> tuple[bool, Any, Any, str]:
            listed = self.endpoint.call("run.configurations")["configurations"]
            config = next(c for c in listed if c["type"] == "cmake-target" and c["target"] == TARGET)
            facts["id"] = config["id"]
            self.endpoint.call("run.select", id=config["id"])
            self.endpoint.call("run.edit", id=config["id"], arguments=arguments)
            after = self.endpoint.call("run.configurations")
            edited = next(c for c in after["configurations"] if c["id"] == config["id"])
            store = corpus / ".frost" / "runConfigurations" / "configurations.json"
            stored = json.loads(store.read_text(encoding="utf-8")) if store.is_file() else {}
            stored_args = next((c.get("arguments") for c in stored.get("configurations", [])
                                if c.get("id") == config["id"]), None)
            ok = after["selected"] == config["id"] and edited["arguments"] == arguments and stored_args == arguments
            return ok, {"selected": config["id"], "arguments": arguments, "stored": arguments}, \
                {"selected": after["selected"], "arguments": edited["arguments"], "stored": stored_args}, ""

        if not self.run_step(f"select {TARGET} and edit its arguments (persisted)", select_and_edit):
            return

        def stored_arguments() -> list[str] | None:
            # The workspace store on disk: what a new FrostIDE reads back.
            stored = json.loads(self.store.read_text(encoding="utf-8")) if self.store.is_file() else {}
            return next((c.get("arguments") for c in stored.get("configurations", []) if c.get("id") == facts["id"]),
                        None)

        def restart_keeps_the_edit() -> tuple[bool, Any, Any, str]:
            """UAT-1 step 2's restart: frost quits as File > Exit would (the endpoint's quit), every
            process of its tree ends, the real frost starts again on the same profile, opens the corpus
            as the first start did, and lists FrostEditor's configuration with the edited arguments.
            The oracle is the arguments this journey wrote, as the store file on disk has them."""
            on_disk = stored_arguments()
            tree = tree_below(self.frost_process.pid, process_table())
            identity = {pid: row["created"] for pid, row in tree.items()}
            self.endpoint.call("quit")
            try:
                exit_code = self.frost_process.wait(timeout=QUIT_SECONDS)
            except subprocess.TimeoutExpired:
                return False, {"exited": True}, {"exited": False}, f"frost did not exit {QUIT_SECONDS} s after quit"
            self.endpoint.close()
            self.endpoint = None
            deadline = time.monotonic() + STOP_SECONDS
            survivors: list[dict] = []
            while True:  # Its children (clangd, cmake) end after it: wait for them, never kill them.
                after = process_table()
                survivors = [{"pid": pid, "name": tree[pid]["name"]} for pid, created in identity.items()
                             if pid in after and after[pid]["created"] == created]
                if not survivors or time.monotonic() > deadline:
                    break
                time.sleep(POLL_SECONDS)
            for survivor in survivors:  # Recorded as a failure above; the journey leaves no process behind.
                if WINDOWS:
                    subprocess.run([j1.system_tool("taskkill.exe"), "/T", "/F", "/PID", str(survivor["pid"])],
                                   capture_output=True, check=False)
                else:
                    try:
                        os.kill(survivor["pid"], 9)
                    except OSError:
                        pass
            self._launch(corpus, again=True)  # Records "connect again"; a failure ends the journey there.
            reopened, _, _, note = open_and_configure()
            listed: dict | None = None
            deadline = time.monotonic() + 120
            while reopened and time.monotonic() < deadline:
                after_restart = self.endpoint.call("run.configurations")
                listed = next((c for c in after_restart["configurations"] if c["id"] == facts["id"]), None)
                if listed is not None:
                    break
                time.sleep(POLL_SECONDS)  # The configuration list follows the published model.
            selected = self.endpoint.call("run.configurations")["selected"]
            if listed is not None and selected != facts["id"]:
                self.endpoint.call("run.select", id=facts["id"])  # The Run below starts FrostEditor again.
            observed = {"exitCode": exit_code, "survivors": survivors, "treeSize": len(identity),
                        "reopened": reopened, "arguments": listed["arguments"] if listed else None,
                        "stored": stored_arguments(), "selectedAfterRestart": selected}
            ok = (exit_code == 0 and not survivors and reopened and listed is not None
                  and listed["arguments"] == arguments and on_disk == arguments and observed["stored"] == arguments)
            return ok, {"exitCode": 0, "survivors": [], "reopened": True, "arguments": arguments,
                        "stored": arguments}, observed, note or ("" if ok else "the edit did not survive the restart")

        if not self.run_step(f"{TARGET}'s edited arguments survive a FrostIDE restart (same profile)",
                             restart_keeps_the_edit):
            return
        key = "cmake:" + TARGET

        def tab_of(state: dict) -> dict | None:
            return next((t for t in state.get("tabs", []) if t["key"] == key), None)

        def run_from_toolbar() -> tuple[bool, Any, Any, str]:
            self.endpoint.call("run.start")
            deadline = time.monotonic() + BUILD_AND_START_SECONDS
            state: dict = {}
            run: dict | None = None
            while time.monotonic() < deadline:
                state = self.endpoint.call("run.state")
                run = next((r for r in state.get("runs", []) if r["configuration"] == facts["id"]
                            and r["processId"]), None)
                tab = tab_of(state)
                if run or (tab and not tab["active"] and not state.get("running")):
                    break
                time.sleep(1.0)  # A build of the engine: poll the endpoint, no fixed wait.
            if not run:
                tab = tab_of(state)
                # The Run tab only says the build failed; the build's own result and the errors MSBuild or
                # the compiler printed (Problems, source "build") say why.
                last = (self.endpoint.call("state").get("cmake") or {}).get("lastBuild")
                errors = [p for p in self.endpoint.call("problems")["problems"]
                          if p.get("source") == "build" and p["severity"] == "error"][:20]
                return False, {"program": program}, {"tab": tab["text"][-4000:] if tab else None,
                                                     "lastBuild": last, "buildErrors": errors}, \
                    f"{TARGET} did not start (the build failed or the run ended)"
            facts["pid"] = int(run["processId"])
            table = process_table()
            row = table.get(facts["pid"], {})
            starts = [r for r in spawn_records(self.spawn_dir) if r.get("event") == "start"
                      and same_path(r.get("executable", ""), program)]
            observed = {"processId": facts["pid"], "image": row.get("name"), "commandLine": run["commandLine"],
                        "spawnStarts": len(starts)}
            ok = bool(row) and row["name"].lower() == Path(program).name.lower() and len(starts) == 1
            return ok, {"program": program, "image": Path(program).name, "spawnStarts": 1}, observed, ""

        if not self.run_step(f"Run on the toolbar builds and starts {TARGET} through the launcher",
                             run_from_toolbar):
            return

        def program_lines(text: str) -> list[str]:
            # The console starts empty for a run; only its closing line is FrostIDE's own.
            return [line for line in text.splitlines()
                    if line.strip() and not line.startswith("Process finished with exit code")]

        def output_streams() -> tuple[bool, Any, Any, str]:
            deadline = time.monotonic() + OUTPUT_SECONDS
            text = ""
            while time.monotonic() < deadline:
                tab = tab_of(self.endpoint.call("run.state")) or {}
                text = tab.get("text", "")
                if program_lines(text) or not tab.get("active"):
                    break
                time.sleep(POLL_SECONDS)
            # Streamed: the text is there and the OS still runs the very process that wrote it.
            row = process_table().get(facts["pid"])
            alive = row is not None and row["name"].lower() == Path(program).name.lower()
            lines = program_lines(text)
            ok = bool(lines) and alive
            return ok, {"outputWhileRunning": True}, {"lines": lines[:20], "stillRunning": alive}, \
                "" if ok else "no output arrived while the program ran"

        self.run_step(f"{TARGET}'s output streams into its Run tab", output_streams)

        def stop_ends_the_tree() -> tuple[bool, Any, Any, str]:
            before = process_table()
            tree = tree_below(facts["pid"], before)
            identity = {pid: row["created"] for pid, row in tree.items()}
            self.endpoint.call("run.stop")
            deadline = time.monotonic() + STOP_SECONDS
            state: dict = {}
            while time.monotonic() < deadline:
                state = self.endpoint.call("run.state")
                tab = tab_of(state) or {}
                if not tab.get("active") and not state.get("runs"):
                    break
                time.sleep(POLL_SECONDS)
            after = process_table()
            survivors = [{"pid": pid, "name": tree[pid]["name"]} for pid, created in identity.items()
                         if pid in after and after[pid]["created"] == created]
            tab = tab_of(state) or {}
            ok = not survivors and not tab.get("active") and not state.get("runs")
            return ok, {"survivors": [], "treeSize": len(identity)}, \
                {"survivors": survivors, "tree": [{"pid": p, "name": r["name"]} for p, r in tree.items()],
                 "closing": tab.get("text", "").splitlines()[-1:] if tab else None}, ""

        self.run_step("Stop on the toolbar ends the whole process tree", stop_ends_the_tree)

    def _cleanup(self) -> None:
        super()._cleanup()
        store: Path | None = getattr(self, "store", None)
        if store is not None:
            if not self.frost_dir_before:
                shutil.rmtree(store.parent.parent, ignore_errors=True)  # .frost/ is the journey's own.
            elif self.store_before is not None:
                store.write_bytes(self.store_before)
            elif store.is_file():
                store.unlink()
                try:
                    store.parent.rmdir()  # Only if the journey's store was all it held.
                except OSError:
                    pass
            self.store = None
        if getattr(self, "editor_config", None) is not None:
            shutil.rmtree(self.editor_config, ignore_errors=True)
            self.editor_config = None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frost", required=True, help="the frost image (Debug)")
    parser.add_argument("--config", required=True, help="the ctest configuration")
    parser.add_argument("--evidence", required=True, help="the directory for the evidence JSON")
    args = parser.parse_args()
    try:
        return Journey(args).main()
    except SystemExit as stop:
        return int(stop.code or 1)


if __name__ == "__main__":
    sys.exit(main())
