# SPDX-License-Identifier: Apache-2.0
# Real watchdog/pause and frozen same-Release overhead oracles. No supplied VM frames or timing.
from __future__ import annotations

import hashlib
import math
import os
from pathlib import Path
import re
import statistics
import time
from typing import Any

from lua_dap_real_support import RealLuaSession, engine_event_cursor, physical_source, require, source_line, wait_engine_event
from lua_dap_real_scenarios import continue_to, stopped_at

PAUSE_SECONDS = 60
MEASUREMENT_PAIRS = 7
WARMUP_PAIRS = 1
MAX_OVERHEAD_RATIO = 1.25


def verify_process_image(pid: int, expected: Path) -> dict[str, Any]:
    """Read the OS process identity; a 40-character corpus SHA alone establishes no execution."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                     wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        require(bool(handle), "cannot observe the actual authenticated engine process")
        try:
            count = wintypes.DWORD(32768)
            buffer = ctypes.create_unicode_buffer(count.value)
            require(bool(kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(count))),
                    "cannot identify the actual authenticated engine executable")
            actual = Path(buffer.value)
        finally:
            kernel.CloseHandle(handle)
    else:
        actual = (Path("/proc") / str(pid) / "exe").resolve(strict=True)
    require(os.path.normcase(str(actual.resolve())) == os.path.normcase(str(expected.resolve(strict=True))),
            "authenticated endpoint belongs to a different engine executable")
    return {"processId": pid, "image": str(actual), "sha256": hashlib.sha256(actual.read_bytes()).hexdigest()}


def watchdog(session: RealLuaSession, oracle: dict[str, Any]) -> dict[str, Any]:
    errors = oracle.get("scriptErrors")
    require(isinstance(errors, list) and errors, "real detached CLI watchdog oracle emitted no script errors")
    expected = next((item for item in errors if "limit" in item.get("message", "").lower()), None)
    require(expected and expected.get("path") and expected.get("line"), "detached watchdog oracle has no real file:line")
    require(expected["line"] == source_line(session.source, "R7_WATCHDOG_LOOP"),
            "detached CLI did not diagnose the actual infinite-loop line")
    session.initialize()
    session.attach()
    stop = stopped_at(session, "R7_WATCHDOG_GATE")
    before = session.dap.request("frost.metrics")
    session.evaluate("self.armed = true", stop, "repl")
    session.breakpoints([])
    after = session.dap.event_cursor
    session.dap.request("continue", {"threadId": stop["event"]["threadId"]})
    session.dap.wait_event("continued", after)
    actual = wait_engine_event(session.engine, "diagnostic", lambda value:
        expected["message"] in value.get("message", ""), timeout=30)
    require(physical_source(session.project, {"path": actual.get("path")}) == session.script.resolve()
            and actual.get("line") == expected["line"],
            "attached watchdog diagnostic differs from the actual detached CLI file:line oracle")
    metrics = session.dap.request("frost.metrics")
    require(metrics.get("countEvents", 0) > before.get("countEvents", 0),
            "debugger attachment suppressed the VM's actual instruction-count hook")
    return {"detachedOracle": expected, "actualAttachedDiagnostic": actual,
            "metricsBefore": before, "metricsAfter": metrics}


def hold_actual_pause(session: RealLuaSession, stop: dict[str, Any], expression: str) -> dict[str, Any]:
    before = session.dap.request("frost.metrics")
    identity_keys = ("vmGeneration", "osThreadId", "stopGeneration")
    require(all(type(before.get(key)) is int and before[key] > 0 and before[key] == stop["event"].get(key)
                for key in identity_keys), "actual paused metrics changed the hook stop's VM/thread/generation identity")
    host_before = session.dap.request("frost.hostMetrics")
    keys = ("normalFrames", "imguiFrames", "automationPolls", "restrictedPumpCalls")
    require(all(type(host_before.get(key)) is int for key in keys) and host_before.get("paused") is True,
            "real host pause metrics are unavailable or do not describe an actually paused VM")
    started = time.monotonic()
    deadline = started + PAUSE_SECONDS
    samples = []
    while time.monotonic() < deadline:
        host = session.dap.request("frost.hostMetrics")
        require(host.get("paused") is True, "real VM left its pause before the required minute")
        require(all(host.get(key) == host_before[key] for key in keys[:-1]),
                "paused host entered a normal frame, nested ImGui frame or normal automation Poll")
        require(host.get("restrictedPumpCalls", 0) >= host_before["restrictedPumpCalls"],
                "restricted host pump counter regressed")
        samples.append({"elapsedSeconds": time.monotonic() - started, **host})
        # This is the explicit wall-clock acceptance interval, never an assumed completion wait.
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(0.1, remaining))
    held_seconds = time.monotonic() - started
    host_after = session.dap.request("frost.hostMetrics")
    require(host_after["restrictedPumpCalls"] > host_before["restrictedPumpCalls"],
            "real paused host never serviced its restricted OS/DAP pump")
    actual = session.dap.request("stackTrace", {"threadId": stop["event"]["threadId"], "startFrame": 0, "levels": 1})
    top = actual.get("stackFrames", [{}])[0]
    require(all(top.get(key) == stop["top"].get(key) for key in ("name", "line", "source")),
            "real paused frame location changed without resuming")
    held_metrics = session.dap.request("frost.metrics")
    require(held_metrics.get("paused") is True and all(held_metrics.get(key) == before[key] for key in identity_keys),
            "real pause generation changed during its required minute")
    # A fresh stackTrace may issue another valid handle. The original live handle must still
    # inspect the same local, without demanding a particular frame-handle allocation policy.
    require(session.evaluate(expression, stop).get("result") == session.evaluate(expression, {**stop, "top": top}).get("result"),
            "original and refreshed real paused frames no longer inspect the same Lua local")
    return {"heldSeconds": held_seconds, "metricsBefore": before, "hostBefore": host_before,
            "hostAfter": host_after, "hostSamples": samples, "heldMetrics": held_metrics,
            "actualStopBefore": stop["top"]}


def pause_for_real_minute(session: RealLuaSession) -> dict[str, Any]:
    session.initialize()
    session.attach()
    stop = stopped_at(session)
    held = hold_actual_pause(session, stop, "speed")
    resumed = continue_to(session, stop, "R7_AFTER_CALL")
    after = session.dap.request("frost.metrics")
    require(after.get("pausedNanoseconds", 0) - held["metricsBefore"].get("pausedNanoseconds", 0) >= PAUSE_SECONDS * 1_000_000_000,
            "real debugger pause accounting did not preserve the measured minute")
    require(resumed["top"].get("line") != stop["top"].get("line"), "real VM did not execute after the minute pause")
    return {**held, "metricsAfter": after, "actualStopAfter": resumed["top"], "actualStartReceipt": session.start_receipt}


def pending_entry_pause_and_owned_stop(session: RealLuaSession) -> dict[str, Any]:
    session.initialize()
    session.attach()
    stop = stopped_at(session, "R7_ON_CREATE")
    require(session.start_receipt is not None and session.operation_id is not None and session.play_generation is None,
            "entry pause did not retain its pending operation without inventing committed Play")
    operation = session.operation_id
    held = hold_actual_pause(session, stop, "self")
    require(held["heldSeconds"] > max(5, session.negotiation["heartbeatTimeoutMs"] / 1000),
            "actual entry pause did not outlive both RPC and negotiated heartbeat deadlines")
    acknowledgement = session.detach()
    # Only authenticated DAP serviced the hook pause. This ordinary RPC runs after the actual
    # detach ACK/unwind and observes the same real VM's accumulated pause accounting.
    metrics = session.engine.request("script.debug.metrics", {}, timeout=5).get("result", {})
    require(metrics.get("vmGeneration") == held["metricsBefore"]["vmGeneration"] and
            metrics.get("script", {}).get("pausedNanoseconds", 0) - held["metricsBefore"].get("pausedNanoseconds", 0)
            >= PAUSE_SECONDS * 1_000_000_000, "detached entry VM did not account for its actual minute pause")
    terminal = session.stop_owned()
    require(terminal.get("actualStopGuard", {}).get("expectedOperationId") == operation and
            terminal["actualTerminalPush"].get("phase") in ("cancelled", "completed") and
            not terminal["actualTerminalPush"].get("operationError"),
            "entry detach did not reach a guarded successful terminal Edit for its accepted operation")
    return {**held, **terminal, "actualStartReceipt": session.start_receipt,
            "receiptElapsedSeconds": session.receipt_elapsed_seconds, "rpcDeadlineSeconds": 5,
            "negotiatedHeartbeatTimeoutMs": session.negotiation["heartbeatTimeoutMs"],
            "actualDetachAcknowledgement": acknowledgement, "actualMetricsAfterDetach": metrics}


def workload_oracle(source: str) -> dict[str, Any]:
    def constant(name: str) -> int:
        found = re.findall(rf"local {name} = (\d+)", source)
        require(len(found) == 1, f"frozen workload constant {name} changed")
        return int(found[0])
    iterations, chunk, modulus = constant("Iterations"), constant("ChunkSize"), constant("Modulus")
    seed = re.findall(r"local value = (\d+)", source)
    require(len(seed) == 1, "frozen workload seed changed")
    value = int(seed[0])
    for i in range(1, iterations + 1):
        value = (value * 33 + i) % modulus
    chunks = (iterations + chunk - 1) // chunk
    return {"iterations": iterations, "checksum": value, "sites": [1, chunks, chunks, chunks, 1],
            "sha256": hashlib.sha256(source.encode()).hexdigest()}


def read_workload_result(data: dict[str, Any], oracle: dict[str, Any]) -> dict[str, Any]:
    result = re.search(r"R7_PERF_RESULT checksum=(\d+) iterations=(\d+) sites=(\d+),(\d+),(\d+),(\d+),(\d+) "
                       r"luaClockSeconds=([0-9.]+) nativeSum=([-0-9.]+)", data.get("message", ""))
    require(result, "actual frozen workload output is malformed")
    observed = {"checksum": int(result[1]), "iterations": int(result[2]),
                "sites": [int(result[i]) for i in range(3, 8)], "luaClockSeconds": float(result[8]),
                "nativeSum": float(result[9])}
    require(all(observed[key] == oracle[key] for key in ("checksum", "iterations", "sites")),
            "actual workload/checksum/executed breakpoint sites differ from the independent frozen oracle")
    require(math.isfinite(observed["nativeSum"]) and observed["nativeSum"] > 0,
            "representative native work did not produce an actual finite result")
    return observed


def one_measurement(endpoint: dict[str, Any], secret_sink: list[str], attached: bool,
                    oracle: dict[str, Any]) -> dict[str, Any]:
    session = RealLuaSession(endpoint, secret_sink)
    try:
        require(hashlib.sha256(session.source.encode()).hexdigest() == oracle["sha256"],
                "frozen workload bytes changed between paired measurements")
        session.initialize()
        session.attach()
        stop = stopped_at(session, "R7_PERF_GATE")
        loaded = stop["top"]["source"]
        actual_source = session.dap.request("source", {"source": loaded, "sourceReference": loaded.get("sourceReference", 0)})
        require(actual_source.get("content") == session.source,
                "actual measured VM did not load the frozen workload bytes")
        session.evaluate("self.armed = true", stop, "repl")
        submitted = session.breakpoints([f"R7_PERF_SITE_{i}" for i in range(1, 6)],
            [{"condition": "false"} for _ in range(5)]) if attached else session.breakpoints([])
        before = session.dap.request("frost.metrics")
        require(type(before.get("executionNanoseconds")) is int,
                "real engine exposes no steady elapsed Lua execution clock")
        require(before.get("clock") == "steady_monotonic_nanoseconds" and
                before.get("executionThreadCpuNanoseconds") is None,
                "workload metrics must identify steady elapsed time and the unavailable thread CPU clock")
        requested_at = time.monotonic()
        cursor = engine_event_cursor(session.engine)
        if attached:
            session.dap.request("continue", {"threadId": stop["event"]["threadId"]})
        else:
            session.dap.request("disconnect", {"terminateDebuggee": False, "restart": False})
            session.dap.close()
        pushed = wait_engine_event(session.engine, "log.entry", lambda value:
            "R7_PERF_RESULT " in value.get("message", ""), timeout=30, after_sequence=cursor)
        response_elapsed = time.monotonic() - requested_at
        observed = read_workload_result(pushed, oracle)
        after_reply = session.engine.request("script.debug.metrics", {})
        require("error" not in after_reply and isinstance(after_reply.get("result"), dict),
                "real host exposes no detached-VM metrics provider")
        after = after_reply["result"].get("script")
        require(isinstance(after, dict) and after_reply["result"].get("vmGeneration", 0) > 0,
                "real host metrics do not identify their actual live VM collector")
        require(type(after.get("executionNanoseconds")) is int, "real host metrics omit steady execution elapsed")
        elapsed = after["executionNanoseconds"] - before["executionNanoseconds"]
        require(elapsed > 0, "actual measured workload execution duration is not positive")
        if attached:
            for breakpoint in submitted:
                if breakpoint.get("verified") is not True:
                    session.dap.wait_event("breakpoint", predicate=lambda value, item=breakpoint:
                        value.get("reason") == "changed" and value.get("breakpoint", {}).get("id") == item["id"]
                        and value.get("breakpoint", {}).get("verified") is True)
            require(after.get("lineEvents", 0) > before.get("lineEvents", 0),
                    "attached five-breakpoint workload did not execute the actual line hook")
        return {"attached": attached, "executionNanoseconds": elapsed, "metricsBefore": before,
                "metricsAfter": after, "actualWorkload": observed,
                "responseElapsedSecondsIncludingDispatch": response_elapsed,
                "luaClockLabel": "Lua os.clock return; platform semantics, not asserted to be CPU time",
                "timingClock": "engine steady monotonic protected-call elapsed, actual pauses excluded"}
    finally:
        session.close()


def overhead(endpoint: dict[str, Any], secret_sink: list[str], source: str) -> dict[str, Any]:
    oracle = workload_oracle(source)  # Freeze before either side is executed; never tune to the result.
    warmup, pairs = [], []
    for index in range(WARMUP_PAIRS + MEASUREMENT_PAIRS):
        order = (False, True) if index % 2 == 0 else (True, False)
        samples = [one_measurement(endpoint, secret_sink, attached, oracle) for attached in order]
        base = next(item for item in samples if not item["attached"])
        attached = next(item for item in samples if item["attached"])
        require(base["actualWorkload"]["nativeSum"] == attached["actualWorkload"]["nativeSum"],
                "debugger attachment changed the actual frozen workload's native arithmetic result")
        pair = {"order": [item["attached"] for item in samples], "measurements": samples,
                "ratio": attached["executionNanoseconds"] / base["executionNanoseconds"]}
        (warmup if index < WARMUP_PAIRS else pairs).append(pair)
    ratio = statistics.median(pair["ratio"] for pair in pairs)
    return {"ok": ratio <= MAX_OVERHEAD_RATIO, "maximumRatio": MAX_OVERHEAD_RATIO,
            "medianPairedRatio": ratio, "oracle": oracle, "warmupPairs": warmup, "pairs": pairs}
