# SPDX-License-Identifier: Apache-2.0
# Mandatory real-engine acceptance driver. Private bootstrap material arrives only through stdin.
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import time
from typing import Any

from lua_dap_real_scenarios import CATALOGUE, run_scenario
from lua_dap_real_safety import overhead, pause_for_real_minute, pending_entry_pause_and_owned_stop, verify_process_image, watchdog
from lua_dap_real_support import EngineOracle, RealLuaSession, require


def listeners(process_id: int) -> list[int]:
    """Read actual OS-owned IPv4/IPv6 TCP listeners; missing native inspection fails closed."""
    if os.name == "nt":
        from ctypes import wintypes

        class Row4(ctypes.Structure):
            _fields_ = [(name, wintypes.DWORD) for name in
                        ("state", "localAddress", "localPort", "remoteAddress", "remotePort", "pid")]

        class Row6(ctypes.Structure):
            _fields_ = [("localAddress", ctypes.c_ubyte * 16), ("localScope", wintypes.DWORD),
                        ("localPort", wintypes.DWORD), ("remoteAddress", ctypes.c_ubyte * 16),
                        ("remoteScope", wintypes.DWORD), ("remotePort", wintypes.DWORD),
                        ("state", wintypes.DWORD), ("pid", wintypes.DWORD)]

        api = ctypes.WinDLL("iphlpapi", use_last_error=True).GetExtendedTcpTable
        api.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), wintypes.BOOL,
                        wintypes.ULONG, ctypes.c_int, wintypes.ULONG]
        api.restype = wintypes.DWORD
        result = []
        for family, row_type in ((socket.AF_INET, Row4), (socket.AF_INET6, Row6)):
            size = wintypes.DWORD(65536)
            for _ in range(4):
                require(size.value <= 16 * 1024 * 1024, "native TCP table exceeded its inspection bound")
                buffer = ctypes.create_string_buffer(size.value)
                code = api(buffer, ctypes.byref(size), False, family, 5, 0)  # TCP_TABLE_OWNER_PID_ALL
                if code == 122:  # ERROR_INSUFFICIENT_BUFFER
                    continue
                require(code == 0, "native process TCP listener inspection failed")
                count = wintypes.DWORD.from_buffer_copy(buffer.raw[:4]).value
                require(4 + count * ctypes.sizeof(row_type) <= len(buffer), "native TCP table is truncated")
                for index in range(count):
                    offset = 4 + index * ctypes.sizeof(row_type)
                    row = row_type.from_buffer_copy(buffer.raw[offset:offset + ctypes.sizeof(row_type)])
                    if row.pid == process_id and row.state == 2:  # MIB_TCP_STATE_LISTEN
                        result.append(socket.ntohs(row.localPort & 0xffff))
                break
            else:
                raise AssertionError("native TCP table kept changing beyond its bounded retries")
        return sorted(set(result))
    require(sys.platform.startswith("linux"), "native listener oracle supports Windows and Linux only")
    process = Path("/proc") / str(process_id)
    inodes = set()
    for descriptor in (process / "fd").iterdir():
        try:
            target = os.readlink(descriptor)
        except FileNotFoundError:
            continue
        if target.startswith("socket:[") and target.endswith("]"):
            inodes.add(target[8:-1])
    require(inodes, "actual engine has no observable socket descriptors")
    result = []
    for name in ("tcp", "tcp6"):
        for line in (process / "net" / name).read_text().splitlines()[1:]:
            fields = line.split()
            require(len(fields) >= 10, "native Linux TCP table is malformed")
            if fields[3] == "0A" and fields[9] in inodes:
                result.append(int(fields[1].split(":")[1], 16))
    return sorted(set(result))


def dist_absence(payload: dict[str, Any], private: list[str]) -> dict[str, Any]:
    endpoint = payload["endpoint"]
    engine = EngineOracle(endpoint)
    try:
        engine.authenticate()
        info = engine.request("server.info", {}).get("result", {})
        pid = info.get("processId")
        require(type(pid) is int and pid > 0, "Dist server omitted actual OS process identity")
        image = verify_process_image(pid, Path(payload["engineImage"]))
        before = listeners(pid)
        require(before == [endpoint["port"]], "Dist engine has an unexpected additional listener before activation")
        reply = engine.request("script.debug.bootstrap", {})
        require("error" in reply and "result" not in reply,
                "Dist granted a script-debug bootstrap instead of refusing activation")
        # Drain an ordinary authenticated reply as an ordering barrier, not an assumed sleep.
        require("result" in engine.request("server.info", {}), "Dist control endpoint stopped after refused activation")
        after = listeners(pid)
        require(after == before, "Dist script-debug activation opened a TCP listener")
        return {"image": image, "listenersBefore": before, "listenersAfter": after,
                "actualBootstrapErrorCode": reply["error"].get("code"), "ok": True}
    finally:
        engine.__exit__()


def run_one(payload: dict[str, Any], private: list[str], action: Any, role: str = "editor") -> dict[str, Any]:
    session = RealLuaSession(payload["endpoint"], private, role)
    try:
        provenance = verify_process_image(session.dap.bootstrap["processId"], Path(payload["engineImage"]))
        observed = action(session)
        return {"actualEngine": provenance, "sourceSha256": hashlib.sha256(session.source.encode()).hexdigest(),
                "observed": observed}
    finally:
        session.close()


def player_initial_on_create(session: RealLuaSession) -> dict[str, Any]:
    session.initialize()
    session.attach()
    pending = session.breakpoints(["R7_ON_CREATE"])
    require(all(item.get("verified") is False for item in pending),
            "player claimed executable breakpoints before its actual initial VM existed")
    before = session.engine.request("script.debug.metrics", {}).get("result", {})
    require(before.get("vmGeneration") == 0 and "script" not in before,
            "player initial Lua VM ran before authenticated pending configuration")
    session.start()
    stopped = session.stopped("R7_ON_CREATE")
    actual_self = session.evaluate("self", stopped)
    require(actual_self.get("variablesReference", 0) > 0 and actual_self.get("type") == "table",
            "player initial OnCreate stop has no actual script-instance self table")
    return {"actualPreVmMetrics": before, "pendingBreakpoints": pending, "actualInitialStop": stopped,
            "actualInitialSelf": actual_self}


def conformance(payload: dict[str, Any], private: list[str]) -> dict[str, Any]:
    historical = json.loads(Path(payload["catalogue"]).read_text(encoding="utf-8"))
    original = {(item["id"], item["category"]) for item in historical["scenarios"]}
    require(len(original) == len(historical["scenarios"]) == len(CATALOGUE) and
            original == {(identity, category) for identity, category, _ in CATALOGUE},
            "real conformance lost or duplicated an original scenario identity/category")
    results = []
    for identity, category, command in CATALOGUE:
        item = {"id": identity, "category": category, "command": command, "passed": False}
        began = time.monotonic()
        try:
            item.update(run_one(payload, private, lambda session: run_scenario(session, identity)))
            item["passed"] = True
        except Exception as error:
            item["failure"] = f"{type(error).__name__}: {error}"
        item["elapsedSeconds"] = time.monotonic() - began
        results.append(item)
    # Historical passed/failed fields are deliberately never used as expected verdicts.
    return {"ok": all(item["passed"] for item in results), "totalCount": len(results),
            "passedCount": sum(item["passed"] for item in results), "skippedCount": 0, "scenarios": results}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", required=True, choices=("conformance", "watchdog", "pause", "pending", "overhead", "dist", "player"))
    args = parser.parse_args()
    private: list[str] = []
    verdict: dict[str, Any] = {"ok": False, "suite": args.suite}
    try:
        raw = sys.stdin.buffer.read(2 * 1024 * 1024 + 1)
        require(len(raw) <= 2 * 1024 * 1024, "private launcher input exceeded its bound")
        payload = json.loads(raw)
        private.append(payload["endpoint"]["secret"])
        require(Path(payload["engineImage"]).is_absolute(), "launcher did not bind an absolute engine image")
        verdict["pinnedEngineSha"] = payload["pinnedEngineSha"]
        if args.suite == "conformance":
            verdict.update(conformance(payload, private))
        elif args.suite == "watchdog":
            verdict.update(run_one(payload, private, lambda session: watchdog(session, payload["watchdogOracle"])))
            verdict["ok"] = True
        elif args.suite == "pause":
            verdict.update(run_one(payload, private, pause_for_real_minute))
            verdict["ok"] = True
        elif args.suite == "pending":
            verdict.update(run_one(payload, private, pending_entry_pause_and_owned_stop))
            verdict["ok"] = True
        elif args.suite == "player":
            verdict.update(run_one(payload, private, player_initial_on_create, "player"))
            verdict["ok"] = True
        elif args.suite == "overhead":
            require(payload["buildType"] == "Release", "overhead requires the actual same Release engine build")
            # The independent OS image check precedes both paired execution paths.
            engine = EngineOracle(payload["endpoint"])
            try:
                engine.authenticate()
                pid = engine.request("server.info", {}).get("result", {}).get("processId")
                require(type(pid) is int and pid > 0, "Release engine omitted actual native pid")
                verdict["actualEngine"] = verify_process_image(pid, Path(payload["engineImage"]))
            finally:
                engine.__exit__()
            source = (Path(payload["endpoint"]["projectPath"]) / "Assets/Scripts/FeatureTest/Hero.lua").read_bytes().decode()
            verdict.update(overhead(payload["endpoint"], private, source))
        else:
            require(payload["buildType"] == "Dist", "Dist absence requires the actual Dist engine build")
            verdict.update(dist_absence(payload, private))
    except Exception as error:
        verdict["ok"] = False
        verdict["failure"] = f"{type(error).__name__}: {error}"
    output = json.dumps(verdict, separators=(",", ":"))
    for secret in sorted(set(private), key=len, reverse=True):
        if secret:
            output = output.replace(secret, "[redacted]")
    print(output, flush=True)
    return 0 if verdict["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
