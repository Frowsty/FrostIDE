# SPDX-License-Identifier: Apache-2.0
# Independent bounded DAP/HMAC client for real pinned FrostEditor acceptance, never a DAP server.
from __future__ import annotations

from collections import deque
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import socket
import sys
import time
from typing import Any, Callable

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/journeys"))
from engine_session_support import EngineOracle  # noqa: E402

MAX_BODY = 1 << 20
MAX_HEADER = 8 << 10
MAX_EVENTS = 4 << 20
IDENTITY_KEYS = ("instanceId", "generation", "projectPath", "processId", "debugSessionId")


def require(condition: Any, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def source_line(source: str, marker: str) -> int:
    found = [number for number, line in enumerate(source.splitlines(), 1) if f"-- {marker}" in line]
    require(len(found) == 1, f"fixture marker {marker} is missing or ambiguous")
    return found[0]


def physical_source(project: Path, source: dict[str, Any]) -> Path:
    path = Path(source.get("path", ""))
    require(bool(source.get("path")), "real stack frame omitted its physical/virtual source path")
    path = path if path.is_absolute() else project / path
    actual = path.resolve(strict=True)
    require(actual.is_relative_to(project.resolve()), "engine source escaped the isolated physical project")
    return actual


def transcript(value: dict[str, Any], role: str) -> bytes:
    ordered = [("role", role)] + [(key, value[key]) for key in IDENTITY_KEYS] + [
        ("clientNonce", value["clientNonce"]), ("serverNonce", value["serverNonce"])]
    return ("FrostEngine.ScriptDebug.v1\n" + "".join(f"{key}={item}\n" for key, item in ordered)).encode()


class RealDap:
    """Single-request client with absolute deadlines, bounded framing and retained actual events."""
    def __init__(self, bootstrap: dict[str, Any], secret_sink: list[str]) -> None:
        require(bootstrap.get("protocolVersion") == 1, "missing real script-debug bootstrap protocol v1")
        require(bootstrap.get("host") == "127.0.0.1", "script debugger is not loopback-only")
        require(type(bootstrap.get("port")) is int and 0 < bootstrap["port"] <= 65535,
                "invalid real script-debug endpoint port")
        require(type(bootstrap.get("processId")) is int and bootstrap["processId"] > 0,
                "script-debug bootstrap omitted the actual engine OS pid")
        require(all(isinstance(bootstrap.get(key), str) and
                    re.fullmatch(r"[0-9a-f]{32}", bootstrap[key]) for key in
                    ("instanceId", "generation", "debugSessionId")) and
                isinstance(bootstrap.get("credential"), str) and
                re.fullmatch(r"[0-9a-f]{64}", bootstrap["credential"]), "invalid private script-debug lease")
        require(bootstrap.get("expiresMs") == 30000, "unexpected script-debug lease expiry contract")
        self.bootstrap = bootstrap
        self.secrets = secret_sink
        self.secrets.append(bootstrap["credential"])
        self.socket = socket.create_connection((bootstrap["host"], bootstrap["port"]), timeout=5)
        self.buffer = b""
        self.sequence = 0
        self.event_cursor = 0
        self.events: deque[tuple[int, dict[str, Any], int]] = deque()
        self.event_bytes = 0
        self.closed = False

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.socket.close()

    def _read(self, deadline: float) -> dict[str, Any]:
        def receive() -> None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("real DAP operation exceeded its absolute deadline")
            self.socket.settimeout(remaining)
            chunk = self.socket.recv(65536)
            if not chunk:
                raise EOFError("real DAP endpoint closed")
            self.buffer += chunk

        while b"\r\n\r\n" not in self.buffer:
            require(len(self.buffer) <= MAX_HEADER, "real DAP header exceeded 8 KiB")
            receive()
        header, _, tail = self.buffer.partition(b"\r\n\r\n")
        require(len(header) <= MAX_HEADER, "real DAP header exceeded 8 KiB")
        lengths = []
        for line in header.split(b"\r\n"):
            key, separator, value = line.partition(b":")
            require(bool(separator), "real DAP header is malformed")
            if key.lower() == b"content-length":
                require(value.strip().isdigit(), "real DAP length is not an unsigned decimal")
                lengths.append(int(value))
        require(len(lengths) == 1 and 0 < lengths[0] <= MAX_BODY, "real DAP body length is invalid")
        self.buffer = tail
        while len(self.buffer) < lengths[0]:
            receive()
        body, self.buffer = self.buffer[:lengths[0]], self.buffer[lengths[0]:]
        require(time.monotonic() < deadline, "real DAP read exceeded its absolute deadline")
        packet = json.loads(body)
        require(isinstance(packet, dict) and packet.get("type") in ("response", "event"),
                "real DAP endpoint emitted an invalid packet")
        return packet

    def _retain(self, packet: dict[str, Any]) -> None:
        require(packet.get("type") == "event", "real DAP endpoint emitted an unexpected response")
        size = len(json.dumps(packet).encode())
        require(self.event_bytes + size <= MAX_EVENTS and len(self.events) < 4096,
                "real DAP event queue exceeded its acceptance bound")
        self.event_cursor += 1
        self.events.append((self.event_cursor, packet, size))
        self.event_bytes += size

    def request_packet(self, command: str, arguments: dict[str, Any] | None = None,
                       timeout: float = 15) -> dict[str, Any]:
        self.sequence += 1
        packet = {"seq": self.sequence, "type": "request", "command": command, "arguments": arguments or {}}
        body = json.dumps(packet, separators=(",", ":")).encode()
        require(len(body) <= MAX_BODY, "acceptance request exceeded the actual DAP frame bound")
        deadline = time.monotonic() + timeout
        self.socket.settimeout(timeout)
        self.socket.sendall(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        while True:
            reply = self._read(deadline)
            if reply.get("type") == "event":
                self._retain(reply)
                continue
            require(reply.get("request_seq") == self.sequence and reply.get("command") == command,
                    "real DAP response did not match the actual outstanding request")
            return reply

    def request(self, command: str, arguments: dict[str, Any] | None = None, timeout: float = 15) -> dict[str, Any]:
        packet = self.request_packet(command, arguments, timeout)
        require(packet.get("success") is True, f"real DAP {command} failed")
        body = packet.get("body", {})
        require(isinstance(body, dict), f"real DAP {command} omitted its response body object")
        return body

    def wait_event(self, event: str, after: int = 0,
                   predicate: Callable[[dict[str, Any]], bool] | None = None, timeout: float = 15) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        while True:
            for cursor, packet, _ in self.events:
                body = packet.get("body", {})
                if cursor > after and packet.get("event") == event and (predicate is None or predicate(body)):
                    return body
            self._retain(self._read(deadline))

    def authenticate(self) -> None:
        nonce = secrets.token_hex(32)
        challenge = self.request("frost/authChallenge", {"debugSessionId": self.bootstrap["debugSessionId"],
                                                        "clientNonce": nonce})
        require(challenge.get("algorithm") == "hmac-sha256" and challenge.get("clientNonce") == nonce,
                "real DAP challenge changed the negotiation or client nonce")
        require(all(challenge.get(key) == self.bootstrap[key] for key in IDENTITY_KEYS),
                "real DAP challenge changed the trusted authenticated R6 identity")
        require(isinstance(challenge.get("serverNonce"), str) and
                re.fullmatch(r"[0-9a-f]{64}", challenge["serverNonce"]), "real DAP server nonce is invalid")
        server = hmac.new(self.bootstrap["credential"].encode(), transcript(challenge, "server"),
                          hashlib.sha256).hexdigest()
        require(hmac.compare_digest(server, challenge.get("serverProof", "")), "real DAP server proof failed")
        proof = hmac.new(self.bootstrap["credential"].encode(), transcript(challenge, "client"),
                         hashlib.sha256).hexdigest()
        self.secrets.extend((server, proof))
        proved = self.request("frost/authProve", {"proof": proof})
        require(proved.get("authenticated") is True and proved.get("protocolVersion") == 1 and
                all(proved.get(key) == self.bootstrap[key] for key in IDENTITY_KEYS),
                "real DAP proof did not authenticate the exact selected engine/VM lease")


class RealLuaSession:
    """One actual editor/player Lua session; every stop/frame/value originates at engine DAP."""
    def __init__(self, endpoint: dict[str, Any], secret_sink: list[str], role: str = "editor") -> None:
        self.engine = EngineOracle(endpoint)
        self.dap: RealDap | None = None
        try:
            self.negotiation = self.engine.authenticate()
            info = self.engine.request("server.info", {}).get("result")
            require(isinstance(info, dict) and info.get("role") == role, "real acceptance target has the wrong host role")
            self.role = role
            subscribed = self.engine.request("server.subscribe", {"events": ["play.state", "log.entry", "diagnostic"]})
            require("error" not in subscribed, "real acceptance could not subscribe to engine push oracles")
            bootstrap = self.engine.request("script.debug.bootstrap", {}).get("result")
            require(isinstance(bootstrap, dict), "real engine did not expose authenticated script-debug bootstrap")
            require(all(bootstrap.get(key) == endpoint[key] for key in ("instanceId", "generation", "projectPath")) and
                    bootstrap.get("processId") == info.get("processId"), "real debugger bootstrap changed engine identity")
            self.dap = RealDap(bootstrap, secret_sink)
            self.dap.authenticate()
            self.project = Path(endpoint["projectPath"])
            self.script = self.project / "Assets/Scripts/FeatureTest/Hero.lua"
            self.source = self.script.read_bytes().decode("utf-8")
            self.playing = False
            self.play_generation: str | None = None
            self.operation_id: str | None = None
            self.start_receipt: dict[str, Any] | None = None
            self.receipt_elapsed_seconds = 0.0
            self.start_event_cursor = engine_event_cursor(self.engine)
            self.active_breakpoints: dict[int, dict[str, Any]] = {}
        except BaseException:
            if self.dap:
                self.dap.close()
            self.engine.__exit__()
            raise

    def close(self) -> None:
        assert self.dap is not None
        self.dap.close()  # Socket detach must release a real VM pause before owner commands can run.
        try:
            if self.operation_id:
                self.stop_owned()
        finally:
            self.engine.__exit__()

    def initialize(self) -> dict[str, Any]:
        capabilities = self.dap.request("initialize", {"adapterID": "frost-real-lua-acceptance", "linesStartAt1": True,
                                                      "columnsStartAt1": True, "pathFormat": "path",
                                                      "supportsVariableType": True, "supportsRunInTerminalRequest": False})
        self.dap.wait_event("initialized")
        return capabilities

    def attach(self) -> None:
        self.dap.request("attach", {"projectPath": str(self.project), "stopOnEntry": False})

    def breakpoints(self, markers: list[str], extra: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
        entries = [{"line": source_line(self.source, marker), **(extra[index] if extra else {})}
                   for index, marker in enumerate(markers)]
        result = self.dap.request("setBreakpoints", {"source": {"path": str(self.script)},
                                                     "breakpoints": entries, "sourceModified": False})
        actual = result.get("breakpoints")
        require(isinstance(actual, list) and len(actual) == len(entries), "real DAP lost submitted breakpoint entries")
        self.active_breakpoints = {}
        for requested, observed in zip(entries, actual, strict=True):
            require(observed.get("verified") is False or
                    (type(observed.get("id")) is int and observed["id"] > 0),
                    "verified real breakpoint omitted its identity")
            self.active_breakpoints[requested["line"]] = observed
        return actual

    def start(self) -> None:
        self.dap.request("configurationDone")
        if self.role == "player":
            # Its opt-in private pre-scene wait releases at this genuine configuration ACK.
            # A player starts its initial scene exactly once; there is no synthetic play.start.
            return
        self.start_event_cursor = engine_event_cursor(self.engine)
        started = time.monotonic()
        reply = self.engine.request("play.start", {}, timeout=5)
        self.receipt_elapsed_seconds = time.monotonic() - started
        require("error" not in reply and isinstance(reply.get("result"), dict), "actual editor rejected Play admission")
        receipt = reply["result"]
        require(receipt.get("accepted") is True and receipt.get("pending") is True and
                receipt.get("startedNow") is False and receipt.get("state") == "Edit" and
                receipt.get("mode") == "Edit" and receipt.get("phase") == "pending" and
                receipt.get("playGeneration") == "" and isinstance(receipt.get("operationId"), str) and
                re.fullmatch(r"[0-9a-f]{32}", receipt["operationId"]),
                "actual remote Play receipt did not describe its pending Edit operation")
        require(self.receipt_elapsed_seconds < 5, "actual pending Play receipt exceeded its five-second deadline")
        self.operation_id = receipt["operationId"]
        self.start_receipt = receipt

    def await_play_started(self) -> None:
        if self.role == "player" or self.play_generation:
            return
        require(self.operation_id is not None, "no actual Play operation was admitted")
        actual = wait_engine_event(self.engine, "play.state", lambda value:
            value.get("operationId") == self.operation_id and value.get("phase") in
            ("playing", "cancelled", "failed", "completed"), timeout=5, after_sequence=self.start_event_cursor)
        require(actual.get("phase") == "playing" and actual.get("state") == "Play" and
                actual.get("mode") == "Play" and bool(actual.get("playGeneration")),
                "admitted Play operation did not publish its actual committed Play generation")
        self.play_generation = actual["playGeneration"]
        self.playing = True

    def detach(self) -> dict[str, Any]:
        acknowledgement = self.dap.request_packet("disconnect", {"terminateDebuggee": False, "restart": False}, timeout=5)
        require(acknowledgement.get("success") is True, "actual DAP detach was not acknowledged")
        self.dap.close()
        return acknowledgement

    def stop_owned(self) -> dict[str, Any]:
        require(self.operation_id is not None, "no actual Play operation is owned by this fixture")
        operation = self.operation_id
        terminal_phases = ("cancelled", "completed", "failed")
        # A prior actual terminal push already completes cleanup. An idempotent stop need not
        # manufacture another event when the engine's deduplication sees identical state.
        def terminal_observation() -> dict[str, Any] | None:
            for packet in self.engine.notifications:
                params = packet.get("params", {})
                data = params.get("data", {})
                if packet.get("method") != "engine.event" or params.get("event") != "play.state":
                    continue
                require(self.engine.identity is not None and all(params.get(key) == self.engine.identity[key]
                        for key in ("instanceId", "generation", "projectPath")), "terminal push changed authenticated identity")
                if data.get("operationId") == operation and params.get("sequence", 0) > self.start_event_cursor:
                    if data.get("phase") == "playing" and data.get("playGeneration"):
                        self.play_generation = data["playGeneration"]
                    elif data.get("phase") in terminal_phases and data.get("state") == "Edit" and not data.get("playGeneration"):
                        return data
            return None
        terminal = terminal_observation()
        if terminal:
            self.operation_id, self.play_generation, self.playing = None, None, False
            return {"actualTerminalPush": terminal, "alreadyTerminal": True}
        guard = {"expectedOperationId": operation}
        if self.play_generation:
            guard["expectedPlayGeneration"] = self.play_generation
        cursor = engine_event_cursor(self.engine)
        reply = self.engine.request("play.stop", guard, timeout=5)
        terminal = terminal_observation()
        if "error" in reply and terminal:
            # Termination may have published its matching terminal event while this guarded
            # request was in flight. That actual event completes the operation's ownership.
            self.operation_id, self.play_generation, self.playing = None, None, False
            return {"actualStopGuard": guard, "actualTerminalPush": terminal, "alreadyTerminal": True}
        result = reply.get("result")
        require("error" not in reply and isinstance(result, dict) and result.get("operationId") == operation
                and result.get("state") == "Edit" and result.get("phase") in terminal_phases
                and not result.get("playGeneration"), "guarded stop did not terminate the exact admitted operation")
        pushed = wait_engine_event(self.engine, "play.state", lambda value:
            value.get("operationId") == operation and value.get("state") == "Edit"
            and value.get("phase") == result["phase"] and not value.get("playGeneration"),
            timeout=5, after_sequence=cursor)
        self.operation_id, self.play_generation, self.playing = None, None, False
        return {"actualStopGuard": guard, "actualOwnerStop": result, "actualTerminalPush": pushed}

    def stopped(self, marker: str | None = None, after: int = 0, reason: str | None = None) -> dict[str, Any]:
        event = self.dap.wait_event("stopped", after, (lambda value: value.get("reason") == reason) if reason else None)
        require(type(event.get("threadId")) is int and event["threadId"] > 0, "real stop omitted its Lua thread")
        stack = self.dap.request("stackTrace", {"threadId": event["threadId"], "startFrame": 0, "levels": 0})
        frames = stack.get("stackFrames")
        require(isinstance(frames, list) and frames, "real stopped Lua VM returned no stack frames")
        top = frames[0]
        require(physical_source(self.project, top.get("source", {})) == self.script.resolve(),
                "real stopped source is not the loaded acceptance script")
        if marker:
            line = source_line(self.source, marker)
            require(top.get("line") == line, "real VM stopped on the wrong executable line")
            submitted = self.active_breakpoints.get(line)
            if submitted:
                if submitted.get("verified") is not True:
                    changed = self.dap.wait_event("breakpoint", predicate=lambda value:
                        value.get("reason") == "changed"
                        and ("id" not in submitted or value.get("breakpoint", {}).get("id") == submitted["id"])
                        and value.get("breakpoint", {}).get("verified") is True
                        and value.get("breakpoint", {}).get("line") == line)
                    submitted = changed["breakpoint"]
                require(type(submitted.get("id")) is int and submitted["id"] > 0
                        and submitted["id"] in event.get("hitBreakpointIds", []),
                        "actual stop did not identify the executed and verified source breakpoint")
        return {"event": event, "stack": stack, "top": top}

    def evaluate(self, expression: str, stop: dict[str, Any], context: str = "watch") -> dict[str, Any]:
        return self.dap.request("evaluate", {"expression": expression, "frameId": stop["top"]["id"], "context": context})

    def scopes(self, stop: dict[str, Any]) -> list[dict[str, Any]]:
        result = self.dap.request("scopes", {"frameId": stop["top"]["id"]}).get("scopes")
        require(isinstance(result, list) and result, "real VM reported no stopped scopes")
        return result

    def variables(self, reference: int) -> list[dict[str, Any]]:
        result = self.dap.request("variables", {"variablesReference": reference}).get("variables")
        require(isinstance(result, list), "real VM variables response omitted its entries")
        return result


def numeric(variable: dict[str, Any]) -> float:
    return float(variable.get("value", variable.get("result", "")))


def by_name(values: list[dict[str, Any]], name: str) -> dict[str, Any]:
    matches = [value for value in values if value.get("name") == name]
    require(len(matches) == 1, f"real VM omitted or duplicated variable/scope {name}")
    return matches[0]


def engine_event_cursor(engine: EngineOracle) -> int:
    return max((packet.get("params", {}).get("sequence", 0) for packet in engine.notifications
                if packet.get("method") == "engine.event"), default=0)


def wait_engine_event(engine: EngineOracle, topic: str, predicate: Callable[[dict[str, Any]], bool],
                      timeout: float = 15, after_sequence: int = -1) -> dict[str, Any]:
    """Drain a real authenticated push stream under a deadline; never poll an engine state RPC."""
    def matching(packet: dict[str, Any]) -> dict[str, Any] | None:
        params = packet.get("params", {})
        if packet.get("method") != "engine.event" or params.get("event") != topic:
            return None
        if type(params.get("sequence")) is not int or params["sequence"] <= after_sequence:
            return None
        require(engine.identity is not None and all(params.get(key) == engine.identity[key]
                for key in ("instanceId", "generation", "projectPath")), "actual engine push changed its authenticated identity")
        data = params.get("data")
        return data if isinstance(data, dict) and predicate(data) else None

    for packet in engine.notifications:
        found = matching(packet)
        if found is not None:
            return found
    deadline = time.monotonic() + timeout
    while True:
        while b"\n" not in engine.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("actual engine push did not arrive before its deadline")
            engine.socket.settimeout(remaining)
            chunk = engine.socket.recv(65536)
            if not chunk:
                raise EOFError("real engine closed its authenticated push stream")
            engine.buffer += chunk
            require(len(engine.buffer) <= 16 << 20 or b"\n" in engine.buffer, "engine push frame exceeded 16 MiB")
        line, _, engine.buffer = engine.buffer.partition(b"\n")
        require(len(line) <= 16 << 20 and time.monotonic() < deadline, "engine push frame/deadline bound exceeded")
        packet = json.loads(line)
        require(isinstance(packet, dict) and "id" not in packet, "unexpected outstanding engine response during push wait")
        size = len(line)
        engine.notifications.append(packet)
        engine.notification_sizes.append(size)
        engine.notification_bytes += size
        while len(engine.notifications) > 256 or engine.notification_bytes > 1 << 20:
            engine.notifications.popleft()
            engine.notification_bytes -= engine.notification_sizes.popleft()
        found = matching(packet)
        if found is not None:
            return found
