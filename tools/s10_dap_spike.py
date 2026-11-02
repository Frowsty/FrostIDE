#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# s10_dap_spike.py: spike S10 (PLAN.md M4; ADR 0029 decision 12; the results are in ADR 0049).
# Can the pinned lldb-dap debug the fixture debuggee (tests/sdk/fixtures/lldb_debuggee) well enough
# for U1: an MSVC build (cl.exe /Zi /Od, read through its PDB) on Windows, a clang -g build on Linux?
#
# Steps:
#   1. Build the debuggee. Windows: the MSVC environment is captured from vcvarsall.bat (run by
#      the absolute cmd.exe, toolset pinned in sdk/sdk.lock.json) and cl.exe is run at its absolute
#      path with it, with a private _MSPDBSRV_ENDPOINT_ so the mspdbsrv.exe it starts is this run's
#      own. Linux: the pinned clang++ -g -O0.
#   2. Drive lldb-dap over stdio (Content-Length framed DAP) through scenarios: "run" (arguments and
#      working directory, breakpoints in main.cpp and worker.cpp, stop reasons, stackTrace, threads,
#      scopes and variables with the STL types' values and children, evaluate, next / stepIn /
#      stepOut, continue to the exit code, the program's output) and one per crash mode (a null
#      dereference, std::abort() without and with a function breakpoint on abort, an unhandled C++
#      exception).
#   3. Print one JSON report (each check's result and evidence, the observations that are recorded
#      but not gated, and the timings) to stdout or --json.
# Exit status: 0 when every check passed, 1 when one failed, 2 when the run itself failed.
#
# Process hygiene: on Windows the script puts itself into a Job Object that kills every process
# still in it when the script ends (cmd, cl, link, mspdbsrv, lldb-dap, the debuggee); on Linux
# lldb-dap leads its own session and the process group is killed at the end. lldb-dap starts with
# --no-lldbinit and without _NT_SYMBOL_PATH, _NT_ALT_SYMBOL_PATH and DEBUGINFOD_URLS. Run it on a
# hidden desktop on Windows (run_hidden_nobox.py), as every on-machine test is.
#
# Usage:
#   python -I tools/s10_dap_spike.py [--lldb-dap PATH] [--work DIR] [--json REPORT] [--cxxflag FLAG]...
# Standard library only.

import argparse
import json
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
LOCK = json.loads((REPO / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))
FIXTURE = REPO / "tests" / "sdk" / "fixtures" / "lldb_debuggee"
WINDOWS = os.name == "nt"
PATIENCE_S = 60.0


class SpikeError(Exception):
    """The run cannot continue (no lldb-dap, no compiler, a build failure)."""


# ---------------------------------------------------------------------------------------------
# Process containment
# ---------------------------------------------------------------------------------------------

def contain_self() -> None:
    """Windows: puts this process into a kill-on-close Job Object, so every process it starts (and
    their children, mspdbsrv.exe included) ends when this script does."""
    if not WINDOWS:
        return
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount", "ReadTransferCount",
                     "WriteTransferCount", "OtherTransferCount")]

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    job = kernel32.CreateJobObjectW(None, None)
    limits = ExtendedLimits()
    limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    ok = job and kernel32.SetInformationJobObject(wintypes.HANDLE(job), 9, ctypes.byref(limits),
                                                  ctypes.sizeof(limits))
    ok = ok and kernel32.AssignProcessToJobObject(wintypes.HANDLE(job), wintypes.HANDLE(kernel32.GetCurrentProcess()))
    if not ok:
        raise SpikeError(f"could not contain the spike in a Job Object (win32 error {ctypes.get_last_error()})")
    contain_self.job = job  # The handle stays open until the process exits.


# ---------------------------------------------------------------------------------------------
# Building the debuggee
# ---------------------------------------------------------------------------------------------

def capture_msvc_environment() -> tuple[dict[str, str], Path]:
    """The environment vcvarsall.bat x64 sets up for the pinned toolset, and cl.exe's absolute path."""
    version = LOCK["msvc"]["vcToolsVersion"]
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    listed = subprocess.run([str(vswhere), "-all", "-products", "*", "-property", "installationPath"],
                            capture_output=True, text=True, check=True).stdout
    installs = [Path(p.strip()) for p in listed.splitlines() if p.strip()]
    install = next((p for p in installs if (p / "VC" / "Tools" / "MSVC" / version / "include").is_dir()), None)
    if install is None:
        raise SpikeError(f"MSVC toolset {version} not found by vswhere")
    vcvarsall = install / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    cmd = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
    # /s strips the outer quotes, so cmd runs: "<vcvarsall>" x64 -vcvars_ver=<v> >nul 2>&1 && set
    line = f'"{cmd}" /d /s /c ""{vcvarsall}" x64 -vcvars_ver={version} >nul 2>&1 && set"'
    result = subprocess.run(line, capture_output=True, text=True, creationflags=0x08000000)  # CREATE_NO_WINDOW
    if result.returncode != 0:
        raise SpikeError(f"vcvarsall failed ({result.returncode}): {result.stderr[-2000:]}")
    env = {}
    for entry in result.stdout.splitlines():
        name, sep, value = entry.partition("=")
        if sep and name:
            env[name] = value
    cl = install / "VC" / "Tools" / "MSVC" / version / "bin" / "Hostx64" / "x64" / "cl.exe"
    if not cl.is_file() or "INCLUDE" not in env:
        raise SpikeError(f"no usable MSVC environment from {vcvarsall}")
    return env, cl


def build_debuggee(work: Path, extra_flags: list[str]) -> tuple[Path, dict]:
    """Builds the fixture into work/bin with `extra_flags` added to the compiler's; returns the
    executable and the build's facts."""
    out = work / "bin"
    out.mkdir(parents=True, exist_ok=True)
    sources = [str(FIXTURE / "main.cpp"), str(FIXTURE / "worker.cpp")]
    started = time.monotonic()
    if WINDOWS:
        env, cl = capture_msvc_environment()
        env["_MSPDBSRV_ENDPOINT_"] = f"frost-s10-{os.getpid()}"
        exe = out / "lldb_debuggee.exe"
        command = [str(cl), "/nologo", "/std:c++17", "/EHsc", "/Zi", "/Od", "/MT", *extra_flags, f"/Fe:{exe}",
                   f"/Fo:{out}\\", f"/Fd:{out / 'vc.pdb'}", *sources, "/link", "/DEBUG:FULL", "/INCREMENTAL:NO"]
        facts = {"compiler": str(cl), "flags": " ".join(["/std:c++17 /EHsc /Zi /Od /MT", *extra_flags])
                 + ", link /DEBUG:FULL"}
    else:
        clang = Path(os.environ.get("FROST_LLVM_ROOT", Path.home() / "sdk" / f"llvm-{LOCK['linux']['llvm']['version']}"))
        exe = out / "lldb_debuggee"
        # libstdc++ 14, the Linux floor, as cmake/toolchains/linux-clang.cmake selects it.
        gcc = "--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14"
        command = [str(clang / "bin" / "clang++"), gcc, "-std=c++17", "-g", "-O0", "-pthread", *extra_flags, "-o",
                   str(exe), *sources]
        env = dict(os.environ)
        facts = {"compiler": command[0], "flags": " ".join([gcc, "-std=c++17 -g -O0 -pthread", *extra_flags])}
    result = subprocess.run(command, cwd=out, env=env, capture_output=True, text=True,
                            creationflags=0x08000000 if WINDOWS else 0)
    if result.returncode != 0:
        raise SpikeError(f"building the debuggee failed:\n{result.stdout[-3000:]}\n{result.stderr[-3000:]}")
    facts["seconds"] = round(time.monotonic() - started, 2)
    facts["executable"] = str(exe)
    if WINDOWS:
        facts["pdb"] = str(exe.with_suffix(".pdb")) if exe.with_suffix(".pdb").is_file() else None
    return exe, facts


def marker_line(name: str, marker: str) -> int:
    """The 1-based line of fixture file `name` that holds `marker`."""
    for number, line in enumerate((FIXTURE / name).read_text(encoding="utf-8").splitlines(), start=1):
        if marker in line:
            return number
    raise SpikeError(f"{marker} not found in {name}")


# ---------------------------------------------------------------------------------------------
# The DAP connection
# ---------------------------------------------------------------------------------------------

class Dap:
    """One lldb-dap process: framed requests out, a reader thread queueing messages in."""

    def __init__(self, lldb_dap: Path):
        env = {k: v for k, v in os.environ.items()
               if k.upper() not in ("_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH", "DEBUGINFOD_URLS")}
        env["LLDB_LAUNCH_INFERIORS_WITHOUT_CONSOLE"] = "true"
        kwargs = {"creationflags": 0x08000000} if WINDOWS else {"start_new_session": True}
        self.started = time.monotonic()
        self.proc = subprocess.Popen([str(lldb_dap), "--no-lldbinit"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, **kwargs)
        self.seq = 0
        self.inbox: queue.Queue = queue.Queue()
        self.pending: list[dict] = []
        self.log: list[dict] = []
        self.stderr = bytearray()
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr.read()), daemon=True).start()

    def _read(self) -> None:
        stream = self.proc.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                byte = stream.read(1)
                if not byte:
                    self.inbox.put(None)
                    return
                header += byte
            match = re.search(rb"Content-Length:\s*(\d+)", header, re.IGNORECASE)
            body = stream.read(int(match.group(1))) if match else b""
            try:
                self.inbox.put(json.loads(body))
            except ValueError:
                self.inbox.put({"type": "unreadable", "raw": body[:200].decode("utf-8", "replace")})

    def send(self, command: str, arguments: dict | None = None) -> int:
        self.seq += 1
        message = {"seq": self.seq, "type": "request", "command": command, "arguments": arguments or {}}
        payload = json.dumps(message).encode("utf-8")
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(payload) + payload)
        self.proc.stdin.flush()
        return self.seq

    def take(self, match, timeout: float = PATIENCE_S) -> dict | None:
        """The first queued (or arriving) message `match` accepts, or None after `timeout`."""
        for index, message in enumerate(self.pending):
            if match(message):
                return self.pending.pop(index)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            try:
                message = self.inbox.get(timeout=left)
            except queue.Empty:
                return None
            if message is None:
                return None
            self.log.append(message)
            if match(message):
                return message
            self.pending.append(message)

    def response(self, seq: int, timeout: float = PATIENCE_S) -> dict | None:
        return self.take(lambda m: m.get("type") == "response" and m.get("request_seq") == seq, timeout)

    def request(self, command: str, arguments: dict | None = None, timeout: float = PATIENCE_S) -> dict:
        """The response (never None: a missing one becomes {"success": False, "message": ...})."""
        answer = self.response(self.send(command, arguments), timeout)
        return answer or {"success": False, "message": f"no response to {command} in {timeout} s"}

    def event(self, name: str, timeout: float = PATIENCE_S) -> dict | None:
        return self.take(lambda m: m.get("type") == "event" and m.get("event") == name, timeout)

    def output(self) -> str:
        """Every 'output' event's text seen so far (queued ones included)."""
        texts = [m.get("body", {}).get("output", "") for m in self.log
                 if m.get("type") == "event" and m.get("event") == "output"]
        return "".join(texts)

    def close(self) -> None:
        """Disconnects (terminating the debuggee) and ends lldb-dap and its tree."""
        if self.proc.poll() is None:
            try:
                self.request("disconnect", {"terminateDebuggee": True}, timeout=10)
                self.proc.stdin.close()
                self.proc.wait(timeout=10)
            except (OSError, ValueError, subprocess.TimeoutExpired):
                pass
        if self.proc.poll() is None:
            if WINDOWS:
                subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32" / "taskkill.exe"), "/T", "/F",
                                "/PID", str(self.proc.pid)], capture_output=True)
            else:
                os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait(timeout=10)
        elif not WINDOWS:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)  # A leftover lldb-server or debuggee in the group.
            except ProcessLookupError:
                pass


# ---------------------------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.checks: list[dict] = []
        self.observations: list[dict] = []
        self.timings: dict[str, float] = {}
        self.records: dict[str, object] = {}

    def check(self, name: str, ok: bool, evidence=None) -> bool:
        """A gated check: a failed one fails the run."""
        self.checks.append({"check": name, "ok": bool(ok), "evidence": evidence})
        return bool(ok)

    def observe(self, name: str, ok: bool, evidence=None) -> None:
        """Recorded, not gated: what a formatter shows where the plan only asks for a record."""
        self.observations.append({"observation": name, "as_expected": bool(ok), "evidence": evidence})


def body(message: dict | None) -> dict:
    return (message or {}).get("body") or {}


def start_session(lldb_dap: Path, exe: Path, args: list[str], cwd: Path, report: Report, tag: str):
    """initialize + launch; returns (dap, launch seq) once 'initialized' has arrived, or (dap, 0)."""
    dap = Dap(lldb_dap)
    init = dap.request("initialize", {"clientID": "frost-s10", "adapterID": "lldb-dap", "linesStartAt1": True,
                                      "columnsStartAt1": True, "pathFormat": "path",
                                      "supportsVariableType": True})
    report.timings[f"{tag}.initialize_s"] = round(time.monotonic() - dap.started, 3)
    if not report.check(f"{tag}: initialize", init.get("success"), init.get("message")):
        return dap, 0
    if tag == "run":
        caps = body(init)
        report.records["capabilities"] = sorted(k for k, v in caps.items() if v is True)
    seq = dap.send("launch", {"program": str(exe), "args": args, "cwd": str(cwd), "stopOnEntry": False})
    initialized = dap.event("initialized")
    report.check(f"{tag}: initialized event", initialized is not None)
    return dap, seq


def set_breakpoints(dap: Dap, source: Path, lines: list[int]) -> list[dict]:
    answer = dap.request("setBreakpoints", {"source": {"path": str(source)},
                                            "breakpoints": [{"line": line} for line in lines]})
    return body(answer).get("breakpoints", [])


def configuration_done(dap: Dap, seq: int, report: Report, tag: str) -> bool:
    """configurationDone, the launch response, then the baseline 'threads' request a client sends
    (VS Code does): lldb-dap answers the first 'threads' request after a launch from the list it
    cached at launch time, whenever that request arrives."""
    done = dap.request("configurationDone")
    launched = dap.response(seq)
    report.check(f"{tag}: configurationDone", done.get("success"), done.get("message"))
    ok = report.check(f"{tag}: launch succeeded", launched and launched.get("success"),
                      (launched or {}).get("message"))
    if ok:
        report.records[f"threads.baseline.{tag}"] = body(dap.request("threads")).get("threads", [])
    return ok


def stack(dap: Dap, thread_id: int, levels: int = 20) -> list[dict]:
    answer = dap.request("stackTrace", {"threadId": thread_id, "startFrame": 0, "levels": levels})
    return [{"name": f.get("name"), "line": f.get("line"),
             "source": Path(f["source"]["path"]).name if f.get("source", {}).get("path") else None,
             "id": f.get("id")} for f in body(answer).get("stackFrames", [])]


def variables(dap: Dap, reference: int, depth: int = 1) -> list[dict]:
    """The variables under `reference`, with children down to `depth` levels."""
    answer = dap.request("variables", {"variablesReference": reference})
    out = []
    for v in body(answer).get("variables", []):
        entry = {"name": v.get("name"), "type": v.get("type"), "value": v.get("value")}
        child = v.get("variablesReference", 0)
        if child and depth > 0:
            entry["children"] = variables(dap, child, depth - 1)[:12]
        out.append(entry)
    return out


def wait_stop(dap: Dap, report: Report, name: str, timeout: float = PATIENCE_S) -> dict | None:
    started = time.monotonic()
    stopped = dap.event("stopped", timeout)
    report.timings[f"{name}_s"] = round(time.monotonic() - started, 3)
    return stopped


def run_scenario(lldb_dap: Path, exe: Path, work: Path, report: Report) -> None:
    main_cpp, worker_cpp = FIXTURE / "main.cpp", FIXTURE / "worker.cpp"
    lines = {m: marker_line("main.cpp", f"BP:{m}") for m in ("main", "call", "exit")}
    lines |= {m: marker_line("worker.cpp", f"BP:{m}") for m in ("worker", "other")}
    cwd = work / "cwd with space"
    cwd.mkdir(parents=True, exist_ok=True)
    dap, seq = start_session(lldb_dap, exe, ["run", "42"], cwd, report, "run")
    try:
        if not seq:
            return
        bp_main = set_breakpoints(dap, main_cpp, [lines["main"], lines["exit"]])
        bp_worker = set_breakpoints(dap, worker_cpp, [lines["worker"]])
        report.records["setBreakpoints"] = {"main.cpp": bp_main, "worker.cpp": bp_worker}
        launch_started = time.monotonic()
        if not configuration_done(dap, seq, report, "run"):
            return

        # 1. The worker thread's breakpoint (another file), with main blocked in join().
        stopped = wait_stop(dap, report, "run.first_stop")
        report.timings["run.launch_to_first_stop_s"] = round(time.monotonic() - launch_started, 3)
        report.check("run: stop at the breakpoint in worker.cpp (reason breakpoint)",
                     body(stopped).get("reason") == "breakpoint", body(stopped))
        thread = body(stopped).get("threadId")
        frames = stack(dap, thread) if thread else []
        report.records["stack.worker"] = frames
        report.check("run: worker stack top is workerMain at BP:worker",
                     bool(frames) and "workerMain" in (frames[0]["name"] or "") and frames[0]["line"] == lines["worker"]
                     and frames[0]["source"] == "worker.cpp", frames[:3])
        threads = body(dap.request("threads")).get("threads", [])
        report.records["threads.at_worker"] = threads
        report.check("run: threads lists at least two threads", len(threads) >= 2, threads)
        bp_verified = [b.get("verified") for b in bp_main + bp_worker]
        report.check("run: every breakpoint verified (bound)", all(bp_verified), bp_verified)

        # 2. main's breakpoint, through continue.
        dap.request("continue", {"threadId": thread})
        stopped = wait_stop(dap, report, "run.continue_to_main")
        thread = body(stopped).get("threadId")
        frames = stack(dap, thread, 30) if thread else []
        report.records["stack.main"] = frames
        report.check("run: stop at BP:main in main.cpp",
                     body(stopped).get("reason") == "breakpoint" and bool(frames) and frames[0]["line"] == lines["main"]
                     and frames[0]["source"] == "main.cpp" and "main" in (frames[0]["name"] or ""), frames[:2])
        report.check("run: stack has frames below main (CRT start-up)", len(frames) >= 2,
                     [f["name"] for f in frames])
        scopes = body(dap.request("scopes", {"frameId": frames[0]["id"]})).get("scopes", []) if frames else []
        report.records["scopes"] = [{"name": s.get("name"), "ref": s.get("variablesReference")} for s in scopes]
        local_scope = next((s for s in scopes if s.get("name", "").lower().startswith("local")), None)
        local_vars = variables(dap, local_scope["variablesReference"], depth=1) if local_scope else []
        report.records["locals"] = local_vars
        by_name = {v["name"]: v for v in local_vars}
        report.check("run: locals has counter == 42 (int)", (by_name.get("counter") or {}).get("value") == "42",
                     by_name.get("counter"))
        argc_var = by_name.get("argc")
        if argc_var is None:  # Arguments may sit in their own scope.
            for scope in scopes:
                if scope is not local_scope:
                    found = {v["name"]: v for v in variables(dap, scope["variablesReference"], 0)}
                    argc_var = argc_var or found.get("argc")
        report.check("run: argc == 3", (argc_var or {}).get("value") == "3", argc_var)
        origin = by_name.get("origin") or {}
        children = {c["name"]: c.get("value") for c in origin.get("children", [])}
        report.check("run: struct origin expands to x=3, y=4", children.get("x") == "3" and children.get("y") == "4",
                     origin)
        pointer = by_name.get("counterPointer") or {}
        pointee = [c.get("value") for c in pointer.get("children", [])]
        report.check("run: pointer counterPointer has a value and dereferences to 42",
                     bool(pointer.get("value")) and "42" in pointee, pointer)
        squares = by_name.get("squares") or {}
        report.check("run: array squares expands to 0,1,4,9",
                     [c.get("value") for c in squares.get("children", [])][:4] == ["0", "1", "4", "9"], squares)
        for name in ("greeting", "primes", "ranks", "boxed", "maybe", "wide", "mode"):
            report.records[f"stl.{name}"] = by_name.get(name)
        # The MSVC STL formatters are C++ and gate on Windows; on Linux the libstdc++ container
        # formatters need Python, which this lldb-dap lacks, so their output is only recorded.
        stl = report.check if WINDOWS else report.observe
        greeting = (by_name.get("greeting") or {}).get("value") or ""
        stl("run: std::string greeting shows \"hello frost\"", "hello frost" in greeting, by_name.get("greeting"))
        primes = by_name.get("primes") or {}
        prime_values = [c.get("value") for c in primes.get("children", [])]
        stl("run: std::vector primes shows 5 elements 2,3,5,7,11",
            prime_values[:5] == ["2", "3", "5", "7", "11"] and "size=5" in (primes.get("value") or ""), primes)

        # Arithmetic, member and element access, a call into the debuggee, and STL member calls
        # (recorded only: whether those resolve depends on what the compiler emitted).
        for expression in ("counter + origin.x", "origin.y * 2", "primes[2]", "*boxed",
                           "debuggee::sumWithBias(origin, 1)", "ranks.size()", "primes.size()", "greeting.size()",
                           "maybe.value()"):
            started = time.monotonic()
            answer = dap.request("evaluate", {"expression": expression, "frameId": frames[0]["id"] if frames else 0,
                                              "context": "watch"})
            report.timings[f"evaluate[{expression}]_s"] = round(time.monotonic() - started, 3)
            error = (body(answer).get("error") or {}).get("format") or answer.get("message")
            report.records[f"evaluate[{expression}]"] = {"success": answer.get("success"),
                                                         "result": body(answer).get("result"),
                                                         "error": error.strip() if error else None}
        report.check("run: evaluate counter + origin.x == 45",
                     body_result(report, "counter + origin.x") == "45", report.records["evaluate[counter + origin.x]"])
        report.check("run: evaluate calls debuggee::sumWithBias(origin, 1) == 8",
                     body_result(report, "debuggee::sumWithBias(origin, 1)") == "8",
                     report.records["evaluate[debuggee::sumWithBias(origin, 1)]"])

        # 3. Stepping: next onto BP:call, stepIn into sumWithBias, stepOut back to main.
        for request, label, expect in (("next", "next", ("main.cpp", lines["call"], "main")),
                                       ("stepIn", "stepIn", ("worker.cpp", lines["other"], "sumWithBias")),
                                       ("stepOut", "stepOut", ("main.cpp", lines["call"], "main"))):
            started = time.monotonic()
            dap.request(request, {"threadId": thread})
            stopped = dap.event("stopped")
            report.timings[f"step.{label}_s"] = round(time.monotonic() - started, 3)
            top = (stack(dap, thread, 1) or [{}])[0]
            report.check(f"run: {label} stops in {expect[2]} at {expect[0]}:{expect[1]}",
                         top.get("source") == expect[0] and top.get("line") == expect[1]
                         and expect[2] in (top.get("name") or ""),
                         {"reason": body(stopped).get("reason"), "top": top})

        # 4. Continue to BP:exit, then to the exit with the exit code.
        dap.request("continue", {"threadId": thread})
        stopped = dap.event("stopped")
        top = (stack(dap, body(stopped).get("threadId") or thread, 1) or [{}])[0]
        report.check("run: continue stops at BP:exit", top.get("line") == lines["exit"], top)
        dap.request("continue", {"threadId": thread})
        exited = dap.event("exited")
        report.check("run: exited with code 42", body(exited).get("exitCode") == 42, body(exited))
        report.check("run: terminated event", dap.event("terminated", 10) is not None)
        output = dap.output()
        report.records["output"] = output[-2000:]
        report.check("run: program output shows the cwd and argc=3",
                     "cwd with space" in output and "argc=3" in output, output[-500:])
    finally:
        dap.close()
        report.records["run.stderr"] = dap.stderr.decode("utf-8", "replace")[-2000:]


def body_result(report: Report, expression: str):
    return (report.records.get(f"evaluate[{expression}]") or {}).get("result")


def crash_scenario(lldb_dap: Path, exe: Path, work: Path, report: Report, mode: str) -> None:
    """One run of the debuggee in a crash mode. "abort-bp" is "abort" with a function breakpoint on
    abort(), the way to stop there on Windows, where the fixture's abort() (with _CALL_REPORTFAULT
    off) raises no exception and the process just exits with code 3."""
    dap, seq = start_session(lldb_dap, exe, [mode.removesuffix("-bp")], work, report, mode)
    try:
        if not seq:
            return
        if mode == "abort-bp":
            answer = dap.request("setFunctionBreakpoints", {"breakpoints": [{"name": "abort"}]})
            report.records["abort-bp.setFunctionBreakpoints"] = body(answer).get("breakpoints")
        if not configuration_done(dap, seq, report, mode):
            return
        started = time.monotonic()
        message = dap.take(lambda m: m.get("type") == "event" and m.get("event") in ("stopped", "exited"))
        report.timings[f"{mode}.to_stop_s"] = round(time.monotonic() - started, 3)
        kind = (message or {}).get("event")
        evidence = {"event": kind, "body": body(message)}
        if kind == "stopped":
            thread = body(message).get("threadId")
            evidence["stack"] = [f"{f['name']} {f['source']}:{f['line']}" for f in stack(dap, thread, 8)]
            info = dap.request("exceptionInfo", {"threadId": thread})
            evidence["exceptionInfo"] = body(info) if info.get("success") else info.get("message")
        report.records[f"crash.{mode}"] = evidence
        stopped_ok = kind == "stopped" and body(message).get("reason") in ("exception", "signal")
        if mode == "crash":
            fault = marker_line("worker.cpp", "FAULT:null")
            frames = evidence.get("stack", [])
            report.check("crash: null dereference stops with reason exception at FAULT:null",
                         stopped_ok and bool(frames) and frames[0].endswith(f"worker.cpp:{fault}"), evidence)
        elif mode == "abort" and WINDOWS:
            report.check("abort: exits with code 3 (no exception is raised)",
                         kind == "exited" and body(message).get("exitCode") == 3, evidence)
        elif mode == "abort-bp":
            frames = evidence.get("stack", [])
            report.check("abort-bp: the function breakpoint on abort stops in abort",
                         kind == "stopped" and body(message).get("reason") in ("breakpoint", "function breakpoint")
                         and bool(frames)
                         and "abort" in frames[0], evidence)
        else:
            report.check(f"{mode}: stops with an exception or signal", stopped_ok, evidence)
    finally:
        dap.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    sdk = Path(os.environ.get("FROST_SDK", r"E:\sdk" if WINDOWS else str(Path.home() / "sdk")))
    version = LOCK["windows" if WINDOWS else "linux"]["lldb"]["version"]
    default_dap = sdk / f"lldb-{version}" / "bin" / ("lldb-dap.exe" if WINDOWS else "lldb-dap")
    parser.add_argument("--lldb-dap", type=Path, default=default_dap, help="lldb-dap to test (default: the pin)")
    parser.add_argument("--work", type=Path, default=None, help="build and working directory (default: a temp dir)")
    parser.add_argument("--json", type=Path, default=None, help="write the report here instead of stdout")
    parser.add_argument("--cxxflag", action="append", default=[],
                        help="an extra compiler flag for the debuggee (repeatable), e.g. -fstandalone-debug")
    args = parser.parse_args()

    report = Report()
    work = args.work or Path(tempfile.mkdtemp(prefix="frost-s10-"))
    try:
        contain_self()
        if not args.lldb_dap.is_file():
            raise SpikeError(f"lldb-dap not found at {args.lldb_dap}")
        exe, facts = build_debuggee(work, args.cxxflag)
        report.records["build"] = facts
        report.records["lldb-dap"] = str(args.lldb_dap)
        version_text = subprocess.run([str(args.lldb_dap), "--version"], capture_output=True, text=True,
                                      creationflags=0x08000000 if WINDOWS else 0).stdout.strip()
        report.records["lldb-dap --version"] = version_text
        started = time.monotonic()
        run_scenario(args.lldb_dap, exe, work, report)
        for mode in ("crash", "abort", "abort-bp", "throw"):
            crash_scenario(args.lldb_dap, exe, work, report, mode)
        report.timings["total_s"] = round(time.monotonic() - started, 3)
    except SpikeError as error:
        print(f"s10_dap_spike: {error}", file=sys.stderr)
        return 2
    finally:
        if args.work is None:
            shutil.rmtree(work, ignore_errors=True)
    result = {"platform": "windows" if WINDOWS else "linux", "passed": sum(c["ok"] for c in report.checks),
              "failed": sum(not c["ok"] for c in report.checks), "checks": report.checks,
              "observations": report.observations, "timings": report.timings, "records": report.records}
    text = json.dumps(result, indent=1, ensure_ascii=False)
    if args.json:
        args.json.write_text(text, encoding="utf-8")
    else:
        print(text)
    for c in report.checks:
        print(("PASS " if c["ok"] else "FAIL ") + c["check"], file=sys.stderr)
    for o in report.observations:
        print(("NOTE (as expected) " if o["as_expected"] else "NOTE (differs) ") + o["observation"], file=sys.stderr)
    return 0 if result["failed"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
