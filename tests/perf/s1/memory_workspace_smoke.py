# SPDX-License-Identifier: Apache-2.0
# Verify native memory counters for an owned Debug Workbench before and after opening a workspace.
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "journeys"))
from j1_cmake import Endpoint, PROTOCOL  # noqa: E402


def receive(probe: subprocess.Popen[str], label: str) -> list[int]:
    fields = probe.stdout.readline().split()
    if not fields or fields[0] != label:
        raise RuntimeError(f"probe did not return {label}")
    return [int(field) for field in fields[1:]]


def request(probe: subprocess.Popen[str], command: str) -> None:
    probe.stdin.write(command + "\n")
    probe.stdin.flush()


def run(probe_path: Path, frost: Path, scratch: Path) -> None:
    connection = scratch / "automation.json"
    workspace = scratch / "workspace"
    workspace.mkdir()
    source = workspace / "live.cpp"
    source.write_text("int frost_smoke() { return 42; }\n", encoding="utf-8")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", FROST_AUTOMATION_CONNECTION=str(connection))
    for key in ("TEMP", "TMP", "LOCALAPPDATA", "APPDATA", "USERPROFILE"):
        directory = scratch / key
        directory.mkdir()
        env[key] = str(directory)
    with subprocess.Popen([probe_path, "--sample-workbench", frost], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, cwd=scratch, env=env) as probe:
        watchdog = threading.Timer(65, probe.kill)
        watchdog.start()
        endpoint = None
        try:
            root, = receive(probe, "root")
            deadline = time.monotonic() + 20
            while not connection.is_file() and probe.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            info = json.loads(connection.read_text(encoding="utf-8"))
            if info["protocol"] != PROTOCOL or info["pid"] != root:
                raise RuntimeError("the Workbench does not own the measured root PID")
            endpoint = Endpoint(info["server"], info["token"])

            def sample(phase: str, project: Path | None, clangd: int | None = None) -> None:
                def verify_state() -> None:
                    state = endpoint.call("state")
                    if project is None:
                        if state["project"]:
                            raise RuntimeError("the initial Workbench already has a workspace")
                    elif Path(state["project"]).resolve() != project.resolve():
                        raise RuntimeError("the Workbench switched away from the disposable workspace")
                    if clangd and (str(state["clangd"]["state"]).lower() != "running"
                                   or state["clangd"]["pid"] != clangd):
                        raise RuntimeError("the Workbench changed its active clangd")
                verify_state()
                request(probe, "sample")
                private, working, tree_private, tree_working, job_peak, *members = receive(probe, "sample")
                verify_state()
                required = {root, clangd} if clangd else {root}
                if not required <= set(members) or min(private, working, tree_private, tree_working, job_peak) <= 0:
                    raise RuntimeError("owned-job sample omits verified processes or memory data")
                print(json.dumps(dict(phase=phase, root=root, clangd=clangd, members=members, privateBytes=private,
                                      workingSetBytes=working, treePrivateBytes=tree_private,
                                      summedWorkingSetBytes=tree_working, kernelJobPeakCommitBytes=job_peak)))

            sample("no-project", None)
            endpoint.call("open", path=str(workspace))
            endpoint.call("trust", root=str(workspace))
            endpoint.call("open", path=str(source))
            state = endpoint.call("wait", **{"for": "clangd", "timeoutMs": 25_000})
            lang = state.get("clangd", {})
            if state.get("timedOut") or str(lang.get("state", "")).lower() != "running" or not lang.get("pid"):
                raise RuntimeError("the actual Workbench did not reach a running clangd phase")
            sample("disposable-workspace-clangd-running", workspace, lang["pid"])
            endpoint.call("quit")
            request(probe, "finish")
            if probe.wait(timeout=12) != 0:
                raise RuntimeError("the owned Workbench did not exit cleanly")
        finally:
            if endpoint is not None:
                endpoint.close()
            if probe.poll() is None:
                probe.kill()
            watchdog.cancel()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--frost", type=Path, required=True, help="Debug FrostIDE with the automation endpoint")
    parser.add_argument("--scratch", type=Path, required=True, help="Directory for a new disposable profile")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="frost-memory-", dir=args.scratch.resolve()) as path:
        run(args.probe.resolve(), args.frost.resolve(), Path(path))
