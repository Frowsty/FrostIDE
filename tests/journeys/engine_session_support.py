# SPDX-License-Identifier: Apache-2.0
# Real R6 TCP oracle and negative authentication probes; no mocked engine or optional skip path.
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import hmac
import json
import os
import queue
import secrets
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

CAPABILITIES = ["session.v2", "auth.hmac-sha256", "events.v1", "heartbeat.v1"]


def run_cli(editor: Path, project: Path, command: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
    """Obtain expected content/errors from a separate real headless process on the scratch project."""
    reply = subprocess.run([str(editor), command, "--project", str(project), "--params", json.dumps(params or {}),
                            "--log-level", "error"], cwd=project, capture_output=True, text=True,
                           errors="replace", timeout=300, check=False)
    document = json.loads(reply.stdout)
    if reply.returncode not in (0, 3) or document.get("ok") is not True or not isinstance(document.get("result"), dict):
        raise RuntimeError(f"real CLI {command} failed with exit {reply.returncode}")
    return document["result"]


def install_acceptance_script(project: Path, source: str) -> None:
    """Use the pinned scene's existing script handle and strip unrelated scripts from the copy."""
    scene_path = project / "Assets/Tests/FeatureTest.fscene"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    retained = False
    for entity in scene["Entities"]:
        components = entity["Components"]
        script = components.get("Script")
        if script:
            if not retained and script["Script"].get("Path") == "Assets/Scripts/FeatureTest/Hero.lua":
                retained = True
            else:
                del components["Script"]
    if not retained:
        raise RuntimeError("pinned scene no longer references the acceptance script")
    scene_path.write_text(json.dumps(scene), encoding="utf-8")
    (project / "Assets/Scripts/FeatureTest/Hero.lua").write_text(source, encoding="utf-8")


class EditorProcess:
    """Offscreen real editor with bounded startup; output is drained and credentials stay private."""
    def __init__(self, image: Path, project: Path, config: Path) -> None:
        self.closed = False
        environment = dict(os.environ)
        environment.pop("FROST_AUTOMATION_TOKEN", None)
        self.process = subprocess.Popen([str(image), "--project", str(project), "--automation-port", "0",
                                         "--config-dir", str(config), "--offscreen", "--size", "320x240"],
                                        cwd=project, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, env=environment, start_new_session=os.name != "nt")
        self.startup: queue.Queue[dict[str, Any] | None] = queue.Queue()
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()
        try:
            startup = self.startup.get(timeout=120)
            if not startup:
                raise RuntimeError("real editor exited before publishing its startup record")
            self.endpoint = {key: startup[key] for key in ("port", "projectPath", "instanceId", "generation")}
            self.endpoint["secret"] = startup["token"]
        except BaseException:
            self.close()
            raise

    def _drain(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            if len(line) > 1 << 20:
                continue
            try:
                value = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if isinstance(value, dict):
                payload = value.get("result", value)
                if isinstance(payload, dict) and payload.get("port"):
                    self.startup.put(payload)
        self.startup.put(None)

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        import j1_cmake as j1
        j1.kill_tree(self.process)
        self.reader.join(timeout=10)
        if self.process.stdout:
            self.process.stdout.close()


def transcript(challenge: dict[str, Any], role: str) -> bytes:
    """Independent contract-v2 transcript oracle; keep roles and trailing newline exact."""
    values = [("role", role), ("instanceId", challenge["instanceId"]),
              ("generation", challenge["generation"]), ("projectPath", challenge["projectPath"]),
              ("clientNonce", challenge["clientNonce"]), ("serverNonce", challenge["serverNonce"]),
              ("capabilities", ",".join(challenge["capabilities"])),
              ("heartbeatIntervalMs", challenge["heartbeatIntervalMs"]),
              ("heartbeatTimeoutMs", challenge["heartbeatTimeoutMs"])]
    return ("FrostEngine.Session.v2\n" + "".join(f"{key}={value}\n" for key, value in values)).encode()


class EngineOracle:
    """A bounded, independent newline JSON-RPC client. Notifications are retained, never polled."""
    def __init__(self, endpoint: dict[str, Any]) -> None:
        self.endpoint = endpoint
        self.socket = socket.create_connection(("127.0.0.1", endpoint["port"]), timeout=10)
        self.socket.settimeout(15)
        self.buffer = b""
        self.next_id = 0
        self.notifications: deque[dict[str, Any]] = deque()
        self.notification_sizes: deque[int] = deque()
        self.notification_bytes = 0
        self.identity: dict[str, Any] | None = None

    def __enter__(self) -> EngineOracle:
        return self

    def __exit__(self, *_: Any) -> None:
        self.socket.close()

    def request(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        self.next_id += 1
        packet: dict[str, Any] = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}
        if self.identity:
            packet["session"] = self.identity
        deadline = time.monotonic() + 15
        self.socket.settimeout(15)
        self.socket.sendall(json.dumps(packet, separators=(",", ":")).encode() + b"\n")
        while True:
            while b"\n" not in self.buffer:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("real engine RPC exceeded its overall deadline")
                self.socket.settimeout(remaining)
                chunk = self.socket.recv(65536)
                if not chunk:
                    raise RuntimeError("engine closed RPC stream")
                self.buffer += chunk
                if len(self.buffer) > 16 << 20 and b"\n" not in self.buffer:
                    raise RuntimeError("engine exceeded the contract frame bound")
            line, _, self.buffer = self.buffer.partition(b"\n")
            if len(line) > 16 << 20:
                raise RuntimeError("engine exceeded the contract frame bound")
            if time.monotonic() >= deadline:
                raise TimeoutError("real engine RPC exceeded its overall deadline")
            reply = json.loads(line)
            if time.monotonic() >= deadline:
                raise TimeoutError("real engine RPC exceeded its overall deadline")
            if not isinstance(reply, dict) or reply.get("jsonrpc") != "2.0":
                raise RuntimeError("real engine emitted a malformed JSON-RPC packet")
            if reply.get("id") == self.next_id:
                return reply
            if "id" in reply:
                raise RuntimeError("real engine emitted an unexpected reply identity")
            if not isinstance(reply.get("method"), str):
                raise RuntimeError("real engine emitted an invalid notification")
            if len(line) <= 1 << 20:
                while self.notifications and (len(self.notifications) >= 256 or
                                               self.notification_bytes + len(line) > 1 << 20):
                    self.notifications.popleft()
                    self.notification_bytes -= self.notification_sizes.popleft()
                self.notifications.append(reply)
                self.notification_sizes.append(len(line))
                self.notification_bytes += len(line)

    def challenge(self) -> dict[str, Any]:
        nonce = secrets.token_hex(32)
        reply = self.request("auth.challenge", {"contractVersion": 2, "clientNonce": nonce,
                             "projectPath": self.endpoint["projectPath"], "requiredCapabilities": CAPABILITIES})
        result = reply.get("result")
        if not isinstance(result, dict):
            raise RuntimeError(f"real engine rejected v2 challenge: {reply}")
        if result.get("contractVersion") != 2 or result.get("algorithm") != "hmac-sha256":
            raise RuntimeError("real engine did not negotiate contract-v2 HMAC-SHA256")
        for key in ("projectPath", "instanceId", "generation"):
            if result.get(key) != self.endpoint[key]:
                raise RuntimeError(f"challenge changed selected {key}")
        if result.get("clientNonce") != nonce or result.get("capabilities") != CAPABILITIES:
            raise RuntimeError("challenge nonce/capabilities do not match the request")
        proof = hmac.new(self.endpoint["secret"].encode(), transcript(result, "server"), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(proof, result.get("serverProof", "")):
            raise RuntimeError("real engine server proof failed independent HMAC oracle")
        if not 0 < result["heartbeatIntervalMs"] < result["heartbeatTimeoutMs"]:
            raise RuntimeError("invalid heartbeat negotiation")
        return result

    def authenticate(self) -> dict[str, Any]:
        challenge = self.challenge()
        proof = hmac.new(self.endpoint["secret"].encode(), transcript(challenge, "client"), hashlib.sha256).hexdigest()
        reply = self.request("auth.prove", {"proof": proof})
        if not authenticated_result(reply, challenge):
            raise RuntimeError("real engine rejected correct proof or changed the authentication identity")
        self.identity = {key: challenge[key] for key in ("instanceId", "generation", "projectPath")}
        return challenge


def authenticated_result(reply: dict[str, Any], challenge: dict[str, Any]) -> bool:
    result = reply.get("result")
    return ("error" not in reply and isinstance(result, dict) and result.get("authenticated") is True and
            result.get("contractVersion") == 2 and all(result.get(key) == challenge[key]
                for key in ("instanceId", "generation", "projectPath")))


def authentication_probes(endpoint: dict[str, Any]) -> dict[str, Any]:
    """Every verdict requires an actual real-engine RPC error, plus a positive control."""
    result: dict[str, Any] = {}
    with EngineOracle(endpoint) as client:
        result["unpairedRejected"] = "error" in client.request("play.state", {})
        result["legacyTokenRejected"] = "error" in client.request("play.state", {"token": endpoint["secret"]})
        result["legacyAuthRejected"] = "error" in client.request("auth", {"token": endpoint["secret"]})
    with EngineOracle(endpoint) as client:
        challenge = client.challenge()
        wrong = hmac.new(b"deliberately unpaired R6 acceptance key", transcript(challenge, "client"),
                         hashlib.sha256).hexdigest()
        result["wrongHmacRejected"] = "error" in client.request("auth.prove", {"proof": wrong})
        correct = hmac.new(endpoint["secret"].encode(), transcript(challenge, "client"), hashlib.sha256).hexdigest()
        result["consumedChallengeRejected"] = "error" in client.request("auth.prove", {"proof": correct})
    with EngineOracle(endpoint) as client:
        challenge = client.challenge()
        result["roleReflectionRejected"] = "error" in client.request("auth.prove", {"proof": challenge["serverProof"]})
    with EngineOracle(endpoint) as client:
        challenge = client.challenge()
        proof = hmac.new(endpoint["secret"].encode(), transcript(challenge, "client"), hashlib.sha256).hexdigest()
        reply = client.request("auth.prove", {"proof": proof})
        result["pairedAccepted"] = authenticated_result(reply, challenge)
    with EngineOracle(endpoint) as client:
        client.challenge()
        result["replayedProofRejected"] = "error" in client.request("auth.prove", {"proof": proof})
    with EngineOracle(endpoint) as client:
        challenge = client.authenticate()
        result["heartbeatIntervalMs"] = challenge["heartbeatIntervalMs"]
        result["heartbeatTimeoutMs"] = challenge["heartbeatTimeoutMs"]
        result["authenticatedCommandAccepted"] = "result" in client.request("schema.export", {})
    result["ok"] = all(value is True for key, value in result.items() if key.endswith(("Rejected", "Accepted")))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-auth", action="store_true", required=True)
    parser.parse_args()
    try:
        result = authentication_probes(json.load(sys.stdin))
    except Exception as error:
        # Do not print credentials or arbitrary exception payloads containing request params.
        print(json.dumps({"ok": False, "errorType": type(error).__name__}))
        return 1
    print(json.dumps(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
