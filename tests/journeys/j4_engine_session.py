# SPDX-License-Identifier: Apache-2.0
# R6 real IDE/engine journey. Expected content and errors come from the pinned engine CLI;
# session state comes exclusively from retained engine push events in the real IDE snapshot.
# Every missing tool, engine build, contract field or event fails. There is no optional fixture skip.
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
import j1_cmake as j1  # noqa: E402
from engine_session_support import EditorProcess, authentication_probes, install_acceptance_script, run_cli  # noqa: E402
from j3_frostengine_lua import find_frost_editor_binary  # noqa: E402

JOURNEY = "j4_engine_session"
j1.JOURNEY = JOURNEY
LOG_MARKER = "R6 journey live log"
ERROR_MARKER = "R6 journey real script failure"


class Journey(j1.Journey):
    def __init__(self, args: argparse.Namespace) -> None:
        super().__init__(args)
        self.editors: list[EditorProcess] = []

    def _cleanup(self) -> None:
        for editor in reversed(self.editors):
            editor.close()
        self.editors.clear()
        super()._cleanup()

    def _main(self) -> int:
        if self.args.config != "Debug":
            self.evidence.step("setup", ok=False, note="the mandatory journey requires the Debug automation endpoint")
            return 1
        try:
            root_text = os.environ.get("FROST_CORPORA_ROOT")
            if not root_text:
                raise RuntimeError("FROST_CORPORA_ROOT is missing; run verify -Corpus/--corpus")
            root = Path(root_text)
            lock = j1.corpora.load_lock(j1.REPO / j1.corpora.LOCK_FILE)
            entry = next(item for item in lock["corpora"] if item["id"] == "frostengine")
            corpus = j1.corpora.corpus_dir(root, entry)
            preset = j1.corpora.engine_preset(entry, None)
            cmake = j1.corpora.pinned_tool("cmake", j1.REPO)
            ninja = j1.corpora.pinned_tool("ninja", j1.REPO)
            problem = j1.corpora.problem_or_none(entry, root) or j1.corpora.engine_problem(
                corpus, entry, preset, {"cmake": cmake, "ninja": ninja})
            if problem:
                raise RuntimeError(f"pinned engine unavailable: {problem}")
            self.evidence.data["corpus"] = {"id": entry["id"], "sha": entry["sha"], "path": str(corpus),
                                            "preset": preset}
            self.evidence.data["frost"] = {"image": self.args.frost,
                                           "sha256": j1.sha256_file(Path(self.args.frost))}
            with j1.corpora.BuildLock(j1.corpora.build_lock_path(root, entry)):
                env = j1.corpora.msvc_environment(j1.REPO) if j1.WINDOWS else dict(os.environ)
                started = time.monotonic()
                built = subprocess.run([cmake, "--build", str(corpus / "build" / preset), "--target", "FrostEditor"],
                                       cwd=corpus, env=env, capture_output=True, text=True, errors="replace",
                                       timeout=3 * 3600, check=False)
                image = find_frost_editor_binary(corpus, preset)
                if not self.evidence.step("build pinned real FrostEditor", ok=built.returncode == 0 and image is not None,
                                          oracle={"exitCode": 0}, observed={"exitCode": built.returncode},
                                          seconds=time.monotonic() - started,
                                          note="" if built.returncode == 0 else (built.stdout + built.stderr)[-4000:]):
                    return 1
                assert image is not None
                self.evidence.data["engine"] = {"image": str(image), "sha256": j1.sha256_file(image)}
                try:
                    self._launch(corpus)
                    self._journey_session(corpus, image)
                    self._quit()
                finally:
                    # End every editor before releasing the build lock or removing its project.
                    for editor in reversed(self.editors):
                        editor.close()
                    self.editors.clear()
                    # A failed Start/Stop step can leave an IDE-owned child alive. End the IDE's
                    # whole process tree and retain evidence before the shared build lock opens.
                    super()._cleanup()
        except Exception as error:
            self.evidence.step("setup/session failure", ok=False, note=f"{type(error).__name__}: {error}")
        return 0 if self.evidence.data["steps"] and all(step["ok"] for step in self.evidence.data["steps"]) else 1

    def wait_snapshot(self, predicate: Callable[[dict[str, Any]], bool], seconds: float = 30) -> dict[str, Any]:
        """Wait on the UI projection only; engine.state does not send an engine state RPC."""
        assert self.endpoint is not None
        deadline = time.monotonic() + seconds
        snapshot: dict[str, Any] = {}
        while time.monotonic() < deadline:
            snapshot = self.endpoint.call("engine.state")
            if predicate(snapshot):
                return snapshot
            time.sleep(0.02)
        raise RuntimeError(f"real IDE did not reach required pushed state within {seconds}s")

    def command(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        assert self.endpoint is not None
        request = self.endpoint.call("engine.command", command=method, params=params or {})
        request_id = request["requestId"]
        state = self.wait_snapshot(lambda value: value.get("lastCompletedRequestId") == request_id and
                                   (value.get("lastReply") is not None or bool(value.get("lastError"))))
        if state.get("lastError"):
            raise RuntimeError(f"real engine refused {method}: {state['lastError']}")
        return state["lastReply"]

    def start_play(self) -> dict[str, Any]:
        assert self.endpoint is not None
        sequence = self.endpoint.call("engine.state")["eventSequence"]
        receipt = self.command("play.start")
        operation = receipt.get("operationId")
        if not (receipt.get("accepted") is True and receipt.get("pending") is True and
                receipt.get("startedNow") is False and receipt.get("state") == "Edit" and
                receipt.get("mode") == "Edit" and receipt.get("phase") == "pending" and
                receipt.get("playGeneration") == "" and isinstance(operation, str) and
                re.fullmatch(r"[0-9a-f]{32}", operation)):
            raise RuntimeError("real Play receipt did not identify its pending operation")

        def matching(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
            return [event for event in snapshot.get("events", []) if event.get("topic") == "play.state" and
                    event.get("sequence", 0) > sequence and
                    (event.get("data") or {}).get("operationId") == operation and
                    (event.get("data") or {}).get("phase") == "playing" and
                    (event.get("data") or {}).get("state") == "Play" and
                    bool((event.get("data") or {}).get("playGeneration"))]

        state = self.wait_snapshot(lambda value: value.get("operationId") == operation and
            value.get("phase") == "playing" and value.get("playing") is True and bool(matching(value)))
        pushed = matching(state)[-1]
        if state.get("playGeneration") != pushed["data"]["playGeneration"]:
            raise RuntimeError("IDE Play generation differs from the actual committed operation push")
        return {"receipt": receipt, "commit": pushed["data"], "sequence": pushed["sequence"]}

    def stop_play(self, started: dict[str, Any]) -> dict[str, Any]:
        assert self.endpoint is not None
        operation, generation = started["commit"]["operationId"], started["commit"]["playGeneration"]
        sequence = self.endpoint.call("engine.state")["eventSequence"]
        reply = self.command("play.stop", {"expectedOperationId": operation, "expectedPlayGeneration": generation})
        if not (reply.get("operationId") == operation and reply.get("phase") == "completed" and
                reply.get("state") == "Edit" and reply.get("playGeneration") == ""):
            raise RuntimeError("real guarded Stop did not complete its exact committed operation")

        def matching(snapshot: dict[str, Any]) -> list[dict[str, Any]]:
            return [event for event in snapshot.get("events", []) if event.get("topic") == "play.state" and
                    event.get("sequence", 0) > sequence and
                    (event.get("data") or {}).get("operationId") == operation and
                    (event.get("data") or {}).get("phase") == "completed" and
                    (event.get("data") or {}).get("state") == "Edit" and
                    not (event.get("data") or {}).get("playGeneration")]

        state = self.wait_snapshot(lambda value: value.get("state") == "Edit" and value.get("operationId") == operation
                                  and value.get("phase") == "completed" and bool(matching(value)))
        return {"reply": reply, "terminal": matching(state)[-1]}

    def _journey_session(self, corpus: Path, image: Path) -> None:
        assert self.endpoint is not None
        ep = self.endpoint
        project = self.tmp / "project"
        shutil.copytree(corpus / "TestProject", project)
        install_acceptance_script(project,
            "local Probe = {}\nfunction Probe:OnCreate()\n  print('R6 journey live log')\n"
            "  error('R6 journey real script failure')\nend\nreturn Probe\n")
        config = self.tmp / "engine-config"
        assets = run_cli(image, project, "assets.list")
        scenes = run_cli(image, project, "assets.list", {"type": "Scene"})
        schema = run_cli(image, project, "schema.export")
        excluded = next((command["name"] for command in schema["commands"]
                         if not (command.get("idePolicy") or {}).get("allowed", False)), None)
        if not excluded:
            raise RuntimeError("real schema contains no command outside the IDE policy")
        error_oracle = run_cli(image, project, "test.run", {"scenes": ["Tests/FeatureTest.fscene"],
                                                         "frames": 1, "headless": True})
        errors = [error for scene in error_oracle["scenes"] for error in scene.get("scriptErrors", [])
                  if ERROR_MARKER in error.get("message", "")]
        if not errors or not errors[0].get("path") or not errors[0].get("line"):
            raise RuntimeError("real test.run did not provide the seeded script diagnostic oracle")
        diagnostic = errors[0]
        first = EditorProcess(image, project, config)
        self.editors.append(first)
        second = EditorProcess(image, project, config)
        self.editors.append(second)

        def auth() -> tuple[bool, Any, Any, str]:
            verdict = authentication_probes(first.endpoint)
            return verdict["ok"], {"allRealAuthenticationProbesPass": True}, verdict, ""
        if not self.run_step("real engine rejects unpaired, legacy, wrong HMAC, reflection and replay", auth):
            return

        def pair() -> tuple[bool, Any, Any, str]:
            ep.call("trust", root=str(project))
            ep.call("open", path=str(project))
            ep.call("engine.configure", editorPath=str(image), configDirectory=str(config),
                    automationDirectory=str(config / "Automation"), editorArguments=["--offscreen", "--size", "320x240"])
            self.wait_snapshot(lambda value: not value.get("discoveryBusy", True))
            ep.call("engine.refresh")
            snapshot = self.wait_snapshot(lambda value: not value.get("discoveryBusy", True) and
                {first.endpoint["instanceId"], second.endpoint["instanceId"]}.issubset(
                    {instance["instanceId"] for instance in value.get("instances", [])}))
            ep.call("engine.pair", instanceId=second.endpoint["instanceId"])
            snapshot = self.wait_snapshot(lambda value: value.get("connected") is True and isinstance(value.get("state"), str))
            expected = {"selectedInstanceId": second.endpoint["instanceId"], "connected": True}
            observed = {key: snapshot.get(key) for key in expected}
            return observed == expected, expected, observed, ""
        if not self.run_step("discover two real instances and pair the explicitly selected instance", pair):
            return

        def content() -> tuple[bool, Any, Any, str]:
            state = self.wait_snapshot(lambda value: (value.get("engineProject") or {}).get("available") is True)
            expected = {"assetCount": len(assets["assets"]), "sceneCount": len(scenes["assets"])}
            observed = {key: state["engineProject"].get(key) for key in expected}
            def entries(listing: list[dict[str, Any]]) -> list[tuple[str, str, str]]:
                return sorted((item["path"], item["type"], str(item["handle"])) for item in listing)
            expected["assets"] = entries(assets["assets"])
            observed["assets"] = entries(state["engineProject"]["assets"])
            return expected == observed, expected, observed, "scenes use assets.list --type Scene, not the open scene.list"
        if not self.run_step("Engine view content counts equal the real asset and Scene CLI listings", content):
            return

        def policy() -> tuple[bool, Any, Any, str]:
            before = ep.call("engine.state")["sentCommandCount"]
            rejected = False
            try:
                ep.call("engine.command", command=excluded, params={})
            except j1.EndpointError:
                rejected = True
            after_deny = ep.call("engine.state")["sentCommandCount"]
            reply = self.command("editor.focus")
            after_allow = ep.call("engine.state")["sentCommandCount"]
            ok = rejected and after_deny == before and after_allow > after_deny and isinstance(reply, dict)
            return ok, {"deniedBeforeSend": True, "allowedCommandExecuted": True}, {
                "denied": rejected, "before": before, "afterDeny": after_deny, "afterAllow": after_allow}, ""
        if not self.run_step("authenticated schema policy permits a real console command and denied command sends zero frames", policy):
            return

        def transitions() -> tuple[bool, Any, Any, str]:
            started = self.start_play()
            operation, generation = started["commit"]["operationId"], started["commit"]["playGeneration"]
            observations: list[dict[str, Any]] = []
            for cycle in range(20):
                for paused in (True, False):
                    sequence = ep.call("engine.state")["eventSequence"]
                    reply = self.command("play.pause", {"paused": paused})
                    state = self.wait_snapshot(lambda value: value.get("eventSequence", 0) > sequence and
                                               value.get("paused") is paused and value.get("operationId") == operation
                                               and value.get("playGeneration") == generation)
                    matching = [event for event in state.get("events", []) if event.get("topic") == "play.state"
                                and event.get("sequence", 0) > sequence and
                                (event.get("data") or {}).get("paused") is paused and
                                (event.get("data") or {}).get("operationId") == operation and
                                (event.get("data") or {}).get("playGeneration") == generation]
                    if not matching or reply.get("paused") is not paused:
                        return False, {"cycles": 20, "pushOnly": True}, observations, "missing actual play.state push"
                    observations.append({"cycle": cycle, "paused": paused, "sequence": matching[-1]["sequence"]})
            self.command("play.pause", {"paused": True})
            paused_state = self.wait_snapshot(lambda value: value.get("paused") is True)
            frame, sequence = paused_state["frame"], paused_state["eventSequence"]
            self.command("play.step", {"frames": 1})
            stepped = self.wait_snapshot(lambda value: value.get("eventSequence", 0) > sequence and
                                        value.get("frame", 0) > frame and value.get("paused") is True and
                                        value.get("operationId") == operation and value.get("playGeneration") == generation)
            stopped = self.stop_play(started)
            return True, {"cycles": 20, "stepFrameIncreases": True}, {"transitions": observations,
                "frameBeforeStep": frame, "frameAfterStep": stepped["frame"], "start": started, "stop": stopped}, ""
        def live_diagnostic() -> tuple[bool, Any, Any, str]:
            expected_path = project / diagnostic["path"]
            deadline = time.monotonic() + 30
            problems: list[dict[str, Any]] = []
            while time.monotonic() < deadline:
                state = ep.call("engine.state")
                logs = [event for event in state.get("events", []) if event.get("topic") == "log.entry" and
                        LOG_MARKER in (event.get("data") or {}).get("message", "")]
                problems = ep.call("problems")["problems"]
                found = next((i for i, problem in enumerate(problems) if problem.get("source") == "engine"
                              and ERROR_MARKER in problem.get("message", "")
                              and j1.same_path(problem.get("file", ""), expected_path)
                              and problem.get("line") == diagnostic["line"]), None)
                if found is not None and logs:
                    ep.call("problems.activate", index=found)
                    opened = ep.call("editor")
                    ok = bool(logs) and j1.same_path(opened.get("path", ""), expected_path) and opened.get("line") == diagnostic["line"]
                    return ok, {"logStreamed": True, "path": str(expected_path), "line": diagnostic["line"]}, {
                        "logStreamed": bool(logs), "editor": opened, "problem": problems[found]}, ""
                time.sleep(0.02)
            return False, diagnostic, problems, "real engine diagnostic did not appear in Problems"
        diagnostic_play = self.start_play()
        if not self.run_step("live engine log arrives and script error Problems activates the CLI oracle file and line", live_diagnostic):
            return
        self.stop_play(diagnostic_play)
        if not self.run_step("Play, twenty pause/resume cycles, Step and Stop follow real push events", transitions):
            return

        def long_main_thread_command() -> tuple[bool, Any, Any, str]:
            before = ep.call("engine.state")
            timeout_ms = before["heartbeatTimeoutMs"]
            if not isinstance(timeout_ms, int) or timeout_ms <= 0:
                raise RuntimeError("missing negotiated heartbeat timeout")
            # The existing scene reference/handle remains real. Each protected Lua callback burns
            # 50 ms of CPU, below its watchdog, while the whole main-thread command spans two
            # negotiated heartbeat windows. os.clock is part of the engine's actual Lua sandbox.
            install_acceptance_script(project,
                "local Probe = {}\nfunction Probe:OnUpdate()\n local stopAt = os.clock() + 0.05\n"
                " local n = 0\n repeat n = (n + 1) % 104729 until os.clock() >= stopAt\n"
                " self.n = n\nend\nreturn Probe\n")
            frames = (timeout_ms * 2 + 49) // 50
            started = time.monotonic()
            reply = self.command("test.run", {"scenes": ["Tests/FeatureTest.fscene"], "frames": frames,
                                               "headless": True})
            elapsed_ms = (time.monotonic() - started) * 1000
            after = ep.call("engine.state")
            ok = (elapsed_ms > timeout_ms and after.get("connected") is True and
                  after.get("connectionGeneration") == before["connectionGeneration"] and
                  isinstance(reply.get("scenes"), list))
            return ok, {"minimumDurationMs": timeout_ms, "sameAuthenticatedConnection": True}, {
                "durationMs": elapsed_ms, "framesRequested": frames, "connected": after.get("connected"),
                "generationBefore": before.get("connectionGeneration"),
                "generationAfter": after.get("connectionGeneration")}, ""
        if not self.run_step("network-thread heartbeat survives an actual long main-thread Lua command", long_main_thread_command):
            return

        def killed() -> tuple[bool, Any, Any, str]:
            state = ep.call("engine.state")
            timeout_ms = state["heartbeatTimeoutMs"]
            if not isinstance(timeout_ms, int) or timeout_ms <= 0:
                raise RuntimeError("missing real negotiated heartbeat timeout")
            started = time.monotonic()
            # Only the paired second process dies; the first remains a distinct live instance.
            second.close()
            snapshot = self.wait_snapshot(lambda value: value.get("connected") is False, timeout_ms / 1000)
            elapsed_ms = (time.monotonic() - started) * 1000
            return elapsed_ms <= timeout_ms, {"disconnectWithinMs": timeout_ms}, {
                "elapsedMs": elapsed_ms, "connected": snapshot.get("connected")}, ""
        if not self.run_step("killed real engine disconnects within its negotiated heartbeat bound", killed):
            return

        def start_editor() -> tuple[bool, Any, Any, str]:
            ep.call("engine.disconnect")
            first.close()
            ep.call("engine.start")
            snapshot = self.wait_snapshot(lambda value: value.get("connected") is True, 120)
            configs = ep.call("run.configurations")["configurations"]
            records = j1.spawn_records(self.spawn_dir)
            starts = [record for record in records if record.get("event") == "start" and
                      j1.same_path(record.get("executable", ""), image)]
            owned = [item for item in configs if item.get("type") == "frost-editor"]
            ok = bool(starts) and bool(owned) and snapshot.get("selectedInstanceId") not in (
                first.endpoint["instanceId"], second.endpoint["instanceId"])
            # Startup records carry pairing material; the Run UI must redact them.
            run_state = ep.call("run.state")
            redacted = "\"token\"" not in json.dumps(run_state) and "serverProof" not in json.dumps(run_state)
            ep.call("run.stop")
            self.wait_snapshot(lambda value: value.get("connected") is False, 30)
            return ok and redacted, {"ownedTypedRun": True, "authenticated": True, "credentialsRedacted": True}, {
                "typedRunIds": [item["id"] for item in owned], "spawned": bool(starts),
                "connected": snapshot.get("connected"), "credentialsRedacted": redacted}, ""
        self.run_step("Start Editor creates a real RUN-11 configuration, authenticates and Stop ends its owned session", start_editor)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frost", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--evidence", required=True)
    return Journey(parser.parse_args()).main()


if __name__ == "__main__":
    sys.exit(main())
