# SPDX-License-Identifier: Apache-2.0
#
# Journey j1_cmake (FND-7a; PLAN.md section 7, R1 DD-1a): the real FrostIDE (Debug, offscreen, on a
# fresh profile) driven through its automation endpoint (src/app/src/automation_endpoint.hpp, ADR
# 0080) against the pinned FrostEngine corpus (tests/corpus/corpora.lock.json) with the pinned
# cmake, Ninja and clangd. Every expected value comes from an oracle that ran here, never from a
# constant:
#   1. open the corpus folder and trust it through the trust dialog's grant;
#   2. the presets FrostIDE lists equal `cmake --list-presets=all`'s configure presets (and the build
#      presets the profiles pair with are among its build presets) [needs CMK-2];
#   3. configure the platform's preset (windows-debug, linux-debug in WSL): cmake's exit code and the
#      File API model, and the build directory the preset names. FrostIDE resolves presets itself and
#      runs the result (cmake_profile.hpp, "Command lines"), so the configure's command line (spawn
#      log) must apply what cmake's own `cmake --preset` applies: the oracle is the cache that
#      `cmake --preset <p> -B <scratch> -C <a script that stops>` writes before the project runs
#      (every preset cache variable, the generator, toolset and platform);
#   4. open a source file: clangd runs, on the IDE-owned compilation database for that configuration
#      (a directory FrostIDE owns, outside the corpus tree, holding the file) [needs CPP-3];
#   5. Go to Definition in three modules (Frost, Frost-Runtime, Frost-Services) lands where the same
#      clangd binary (its spawn-log path and sha256), asked textDocument/definition on the same
#      database, says;
#   6. a seeded compile error in one small target (FrostTestDetachedChild: one source, no library
#      dependencies) is built through the preset; Problems shows it at the seeded line and its
#      activation opens that line;
#   7. the fix rebuilds clean (no error left); Clean succeeds. Then the build preset oracle: from the
#      clean tree, `cmake --build --preset <build preset> --target <target>` builds the target, the
#      tree is cleaned again and the build's own command line (spawn log) builds it; both must run
#      the same Ninja edges. (A dry run cannot show them: the engine's CONFIGURE_DEPENDS globs make
#      Ninja's dry run stop at the regeneration edge.) The tree is left clean.
#   8. after frost quits, no freeze report its own stall watchdog wrote in the run's profile is over
#      STALL_LIMIT_MS (FND-9b, ADR 0078 decision 7); every journey built on this machinery (j2_run,
#      trust_malicious_repo) ends with the same step, and keeps the reports beside its evidence.
# Steps marked [needs ...] test the intended behaviour of a parallel R1 epic and fail until it lands.
#
# The evidence (one JSON file per run, under --evidence): the corpus (id, sha, path, preset), the
# frost image and its sha256, the spawn-log paths, every tool the spawn log saw with its sha256,
# and per step its oracle value, observed value, timing and verdict. The corpus clone is shared, so
# the run holds the corpus build lock (tools/corpora.py BuildLock) and restores the seeded file
# from its original bytes in every outcome (and at the start, if an interrupted run left it).
#
# Run by ctest (tests/journeys/CMakeLists.txt, label corpus: verify -Corpus only):
#   j1_cmake.py --frost <frost image> --config <ctest config> --evidence <directory>
# FROST_CORPORA_ROOT names the corpora root. A missing corpus, a missing pinned engine
# configuration, a non-Debug tree or a missing tool fails the journey (exit 1); it never skips.

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, Callable

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import corpora  # noqa: E402  (tools/ is put on the path above)

WINDOWS = os.name == "nt"
JOURNEY = "j1_cmake"
PROTOCOL = "frost-automation-endpoint/1"
EVIDENCE_FORMAT = "frost-journey-evidence/1"

# The seeded error: one line added after an anchor inside FrostTestDetachedChild's Run().
SEED_FILE = "Frost-Tests/Helpers/DetachedChildMain.cpp"
SEED_TARGET = "FrostTestDetachedChild"
SEED_ANCHOR = ("\t\tif (arguments.size() < 3)", "\t\t\treturn 2;")
SEED_TEXT = "\t\tconst int frostJourneySeededError = frost_journey_undeclared_identifier;"

# Go to Definition: (module, file, the text whose first character the caret goes to). The expected
# landing is clangd's own answer, asked here.
NAVIGATION = (
    ("Frost", "Frost/src/Frost/Audio/AudioEngine.cpp", "AudioAttenuationModel Attenuation"),
    ("Frost-Runtime", "Frost-Runtime/src/RuntimeApp.cpp", "GameContent>> content"),
    ("Frost-Services", "Frost-Services/src/ServicesApp.cpp", "TerminationSignalHandler::IsTerminationRequested"),
)

# Limits. The whole journey ends (frost is killed) after JOURNEY_SECONDS; each wait has its own cap.
JOURNEY_SECONDS = 3 * 3600
CONFIGURE_SECONDS = 3600
BUILD_SECONDS = 1800
CLANGD_SECONDS = 600
NAVIGATE_SECONDS = 600
POLL_SECONDS = 0.25
# The interim stall gate of the milestone journeys (ADR 0078 decision 7; FND-9b for R3).
STALL_LIMIT_MS = 250


def now_utc() -> str:
    return datetime.datetime.now(datetime.UTC).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def same_path(a: str | Path, b: str | Path) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def is_under(path: str | Path, root: str | Path) -> bool:
    try:
        Path(os.path.normcase(os.path.realpath(path))).relative_to(os.path.normcase(os.path.realpath(root)))
        return True
    except ValueError:
        return False


def uri_to_path(uri: str) -> str:
    parsed = urllib.parse.urlparse(uri)
    return urllib.request.url2pathname(urllib.parse.unquote(parsed.path)) if parsed.scheme == "file" else uri


def system_tool(name: str) -> str:
    return os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", name)


def kill_tree(process: subprocess.Popen) -> None:
    """Ends `process` and every process it started (Windows: taskkill /T; Linux: its session)."""
    if process.poll() is not None and not WINDOWS:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        return
    if WINDOWS:
        subprocess.run([system_tool("taskkill.exe"), "/T", "/F", "/PID", str(process.pid)], capture_output=True,
                       check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    try:
        process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        pass


# --- The evidence --------------------------------------------------------------------------------


class Evidence:
    """The journey's record; written whole at the end (and on every failure path)."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.started = time.monotonic()
        self.data: dict[str, Any] = {"format": EVIDENCE_FORMAT, "journey": JOURNEY,
                                     "platform": "windows" if WINDOWS else "linux", "started": now_utc(),
                                     "steps": [], "spawnLogs": [], "tools": {}}

    def step(self, name: str, *, ok: bool, oracle: Any = None, observed: Any = None, needs: str | None = None,
             seconds: float = 0.0, note: str = "") -> bool:
        record = {"name": name, "ok": bool(ok), "durationMs": round(seconds * 1000), "oracle": oracle,
                  "observed": observed}
        if needs:
            record["needsEpic"] = needs
        if note:
            record["note"] = note
        self.data["steps"].append(record)
        verdict = "ok" if ok else ("FAILED (needs " + needs + ")" if needs else "FAILED")
        print(f"{JOURNEY}: {name}: {verdict} ({record['durationMs']} ms)" + (f": {note}" if note and not ok else ""),
              flush=True)
        return ok

    def write(self) -> None:
        self.data["finished"] = now_utc()
        self.data["durationMs"] = round((time.monotonic() - self.started) * 1000)
        self.data["ok"] = bool(self.data["steps"]) and all(step["ok"] for step in self.data["steps"])
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".partial")
        temporary.write_text(json.dumps(self.data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        os.replace(temporary, self.path)


def spawn_records(directory: Path) -> list[dict]:
    """Every record of every spawn-log session in `directory`, in file and line order."""
    records: list[dict] = []
    for log in sorted(directory.glob("spawn-*.jsonl")):
        # FrostIDE rewrites the log atomically after each record; on Windows a read that meets the
        # replace fails with a sharing violation (PermissionError) for a moment, so it is retried.
        for attempt in range(100):
            try:
                text = log.read_text(encoding="utf-8", errors="replace")
                break
            except PermissionError:
                if attempt == 99:
                    raise
                time.sleep(0.02)
        for line in text.splitlines():
            if line.strip():
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    records.append({"event": "unparsable", "line": line})
    return records


def tools_seen(records: list[dict]) -> dict[str, dict]:
    """{file name: {executable, sha256, starts}} over the spawn log's start records."""
    tools: dict[str, dict] = {}
    for record in records:
        if record.get("event") != "start":
            continue
        executable = record.get("executable", "")
        # The base name at either separator: the log may come from the other OS (Path splits only at /).
        name = re.split(r"[\\/]", executable)[-1].lower()
        entry = tools.setdefault(name, {"executable": executable, "sha256": record.get("sha256"), "starts": 0})
        entry["starts"] += 1
    return tools


# --- The endpoint client --------------------------------------------------------------------------


class EndpointError(RuntimeError):
    pass


class Endpoint:
    """A synchronous client: one request, then its reply (the server answers in order)."""

    def __init__(self, server: str, token: str) -> None:
        if WINDOWS:
            self._pipe = open(server, "r+b", buffering=0)  # noqa: SIM115 (closed in close())
            self._socket = None
        else:
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._socket.connect(server)
            self._pipe = None
        self._buffer = b""
        self._next_id = 0
        hello = self.call("hello", token=token)
        if hello.get("protocol") != PROTOCOL:
            raise EndpointError(f"the endpoint speaks {hello.get('protocol')!r}, not {PROTOCOL}")

    def _send(self, data: bytes) -> None:
        if self._pipe is not None:
            self._pipe.write(data)
        else:
            self._socket.sendall(data)

    def _receive(self) -> bytes:
        chunk = self._pipe.read(65536) if self._pipe is not None else self._socket.recv(65536)
        if not chunk:
            raise EndpointError("the endpoint closed the connection (frost exited?)")
        return chunk

    def call(self, method: str, **params: Any) -> dict:
        self._next_id += 1
        request = {"id": self._next_id, "method": method, "params": params}
        self._send((json.dumps(request) + "\n").encode("utf-8"))
        while True:
            while b"\n" not in self._buffer:
                self._buffer += self._receive()
            line, _, self._buffer = self._buffer.partition(b"\n")
            reply = json.loads(line.decode("utf-8"))
            if reply.get("id") != self._next_id:
                continue
            if "error" in reply:
                error = reply["error"]
                raise EndpointError(f"{method}: {error.get('code')}: {error.get('message')}")
            return reply.get("result", {})

    def close(self) -> None:
        for stream in (self._pipe, self._socket):
            if stream is not None:
                try:
                    stream.close()
                except OSError:
                    pass


# --- The clangd oracle ---------------------------------------------------------------------------


class ClangdOracle:
    """The same clangd FrostIDE ran, as a plain LSP client asks it: textDocument/definition."""

    def __init__(self, executable: str, database: str, root: Path) -> None:
        self.process = subprocess.Popen(
            [executable, f"--compile-commands-dir={database}", "--background-index=false", "--log=error"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=str(root),
            start_new_session=not WINDOWS)
        self.messages: queue.Queue[dict | None] = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()
        self.request("initialize", {"processId": os.getpid(), "rootUri": root.as_uri(),
                                    "capabilities": {"textDocument": {"definition": {"linkSupport": False}}}},
                     timeout=120)
        self.notify("initialized", {})

    def _read(self) -> None:
        stream = self.process.stdout
        try:
            while True:
                length = None
                while True:
                    header = stream.readline()
                    if not header:
                        self.messages.put(None)
                        return
                    header = header.strip()
                    if not header:
                        break
                    name, _, value = header.partition(b":")
                    if name.lower() == b"content-length":
                        length = int(value.strip())
                if length is None:
                    continue
                self.messages.put(json.loads(stream.read(length).decode("utf-8")))
        except (OSError, ValueError):
            self.messages.put(None)

    def _write(self, message: dict) -> None:
        body = json.dumps(message).encode("utf-8")
        self.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.process.stdin.flush()

    def notify(self, method: str, params: dict) -> None:
        self._write({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: dict, timeout: float) -> Any:
        self.next_id += 1
        wanted = self.next_id
        self._write({"jsonrpc": "2.0", "id": wanted, "method": method, "params": params})
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"clangd did not answer {method} within {timeout:.0f} s")
            try:
                message = self.messages.get(timeout=remaining)
            except queue.Empty:
                continue
            if message is None:
                raise EndpointError(f"clangd exited during {method}")
            if "method" in message and "id" in message:  # A server request (progress tokens, ...).
                self._write({"jsonrpc": "2.0", "id": message["id"], "result": None})
                continue
            if message.get("id") == wanted:
                if "error" in message:
                    raise EndpointError(f"clangd {method}: {message['error']}")
                return message.get("result")

    def definition(self, path: Path, line0: int, character0: int) -> list[dict]:
        """The definition locations as [{"path", "line" (1-based)}]."""
        uri = path.as_uri()
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": path.read_text(encoding="utf-8", errors="replace")}})
        result = self.request("textDocument/definition", {"textDocument": {"uri": uri},
                                                         "position": {"line": line0, "character": character0}},
                              timeout=NAVIGATE_SECONDS)
        self.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        if isinstance(result, dict):
            result = [result]
        out = []
        for location in result or []:
            target = location.get("uri") or location.get("targetUri", "")
            span = location.get("range") or location.get("targetSelectionRange") or {}
            out.append({"path": uri_to_path(target), "line": span.get("start", {}).get("line", -1) + 1})
        return out

    def close(self) -> None:
        try:
            self.request("shutdown", {}, timeout=30)
            self.notify("exit", {})
            self.process.wait(timeout=30)
        except (OSError, TimeoutError, EndpointError, subprocess.TimeoutExpired):
            pass
        if self.process.poll() is None:
            kill_tree(self.process)


# --- Oracles run here ----------------------------------------------------------------------------


ORACLE_STOP = "frost journey oracle: stop before the project"


def preset_cache(cmake: str, root: Path, preset: str, scratch: Path) -> dict[str, tuple[str, str]]:
    """What `cmake --preset <preset>` puts in the cache before the project runs, as {name: (type,
    value)}: cmake resolves the preset into a scratch build directory and an initial-cache script
    stops it before the project's own code (header comment, step 3)."""
    scratch.mkdir(parents=True, exist_ok=True)
    stop = scratch / "stop.cmake"
    stop.write_text(f'message(FATAL_ERROR "{ORACLE_STOP}")\n', encoding="utf-8")
    build = scratch / "build"
    result = subprocess.run([cmake, "--preset", preset, "-B", str(build), "-C", str(stop)], cwd=str(root),
                            capture_output=True, text=True, errors="replace", timeout=600, check=False)
    if ORACLE_STOP not in result.stdout + result.stderr:
        raise RuntimeError(f"cmake --preset {preset} did not reach the stop script ({result.returncode}): "
                           f"{(result.stdout + result.stderr)[-600:]}")
    return parse_cache((build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace"))


def parse_cache(text: str) -> dict[str, tuple[str, str]]:
    out = {}
    for line in text.splitlines():
        match = re.match(r"^([^#/][^:=]*):([A-Z]+)=(.*)$", line)
        if match:
            out[match.group(1)] = (match.group(2), match.group(3))
    return out


def configure_switches(argv: list[str]) -> dict[str, str]:
    """A configure command line's -D values by name and its -G, -T, -A and -B as "generator",
    "toolset", "platform" and "binaryDir" (both "-DX=v" and "-D X=v" forms)."""
    out: dict[str, str] = {}
    flags = {"-G": "generator", "-T": "toolset", "-A": "platform", "-B": "binaryDir"}
    i = 1
    while i < len(argv):
        arg = argv[i]
        if arg in flags and i + 1 < len(argv):
            out[flags[arg]] = argv[i + 1]
            i += 2
            continue
        define = None
        if arg == "-D" and i + 1 < len(argv):
            define, i = argv[i + 1], i + 1
        elif arg.startswith("-D"):
            define = arg[2:]
        if define and "=" in define:
            name, value = define.split("=", 1)
            out["-D" + name.split(":", 1)[0]] = value
        i += 1
    return out


def preset_differences(oracle: dict[str, tuple[str, str]], switches: dict[str, str]) -> dict[str, Any]:
    """What the configure's switches do not apply of cmake's own preset resolution: every preset
    cache variable (UNINITIALIZED entries, and a typed toolchain file or install prefix), the
    generator, toolset and platform."""
    wanted: dict[str, str] = {}
    for name, (kind, value) in oracle.items():
        if kind == "UNINITIALIZED" or name in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_INSTALL_PREFIX"):
            wanted["-D" + name] = value
    for key, entry in (("generator", "CMAKE_GENERATOR"), ("toolset", "CMAKE_GENERATOR_TOOLSET"),
                       ("platform", "CMAKE_GENERATOR_PLATFORM")):
        wanted[key] = oracle.get(entry, ("", ""))[1]
    return {key: {"cmake --preset": value, "FrostIDE": switches.get(key, "")} for key, value in wanted.items()
            if switches.get(key, "") != value}


# Ninja's own steps around a build, which run whatever is built (the glob check, a regeneration).
NINJA_HOUSEKEEPING = ("Re-checking globbed directories...", "Re-running CMake...")


def ninja_edges(output: str) -> list[str]:
    """The edges a Ninja build ran ("[3/7] Building CXX object ..."), without the counters and
    Ninja's own housekeeping, sorted (the order varies with parallelism)."""
    edges = []
    for line in output.splitlines():
        match = re.match(r"^\[\d+/\d+\]\s*(.+?)\s*$", line)
        if match and match.group(1) not in NINJA_HOUSEKEEPING:
            edges.append(match.group(1))
    return sorted(edges)


def build_edges(argv: list[str], cwd: str, env: dict[str, str]) -> list[str]:
    """Runs a `cmake --build ...` command line and returns the Ninja edges it ran (ninja_edges).
    Raises RuntimeError when it fails."""
    result = subprocess.run(argv, cwd=cwd or None, env=env, capture_output=True, text=True, errors="replace",
                            timeout=BUILD_SECONDS, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(argv)} exited with {result.returncode}: {result.stdout[-600:]}"
                           f"{result.stderr[-400:]}")
    return ninja_edges(result.stdout)


def listed_presets(cmake: str, root: Path) -> dict[str, list[str]]:
    """`cmake --list-presets=all` in `root`: {"configure": [...], "build": [...], ...} (quoted names)."""
    run = subprocess.run([cmake, "--list-presets=all"], cwd=str(root), capture_output=True, text=True,
                         errors="replace", timeout=300, check=False)
    if run.returncode != 0:
        raise EndpointError(f"cmake --list-presets=all failed ({run.returncode}): {run.stderr.strip()}")
    return parse_listed_presets(run.stdout)


def parse_listed_presets(text: str) -> dict[str, list[str]]:
    """The sections of `cmake --list-presets` output ("Available configure presets:" and its quoted
    names, and so on) as {"configure": [names], ...}."""
    sections: dict[str, list[str]] = {}
    current = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Available ") and stripped.endswith(" presets:"):
            current = stripped.removeprefix("Available ").removesuffix(" presets:")
            sections[current] = []
        elif current and stripped.startswith('"'):
            sections[current].append(stripped.split('"')[1])
    return sections


def caret_of(path: Path, needle: str) -> tuple[int, int]:
    """The 1-based line and column of the first `needle` in `path` (ASCII lines: column = character)."""
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        index = line.find(needle)
        if index >= 0:
            return number, index + 1
    raise EndpointError(f"{needle!r} is not in {path} at this pin")


# --- The journey ---------------------------------------------------------------------------------


class Journey:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.frost_process: subprocess.Popen | None = None
        self.endpoint: Endpoint | None = None
        self.oracle: ClangdOracle | None = None
        self.seed_original: bytes | None = None
        self.seed_path: Path | None = None
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%S")
        self.evidence = Evidence(Path(args.evidence) / f"{JOURNEY}-{'windows' if WINDOWS else 'linux'}-{stamp}.json")

    # The steps share this: run `body`, record its outcome; an exception is a failed step.
    def run_step(self, name: str, body: Callable[[], tuple[bool, Any, Any, str]], needs: str | None = None) -> bool:
        started = time.monotonic()
        try:
            ok, oracle, observed, note = body()
        except Exception as error:  # noqa: BLE001 (a failed step records why and the journey goes on)
            ok, oracle, observed, note = False, None, None, f"{type(error).__name__}: {error}"
        return self.evidence.step(name, ok=ok, oracle=oracle, observed=observed, needs=needs,
                                  seconds=time.monotonic() - started, note=note)

    def wait_cmake(self, seconds: float, done: Callable[[dict], bool]) -> dict:
        """Asks the endpoint to wait for an idle CMake service until `done(state)` or the deadline."""
        deadline = time.monotonic() + seconds
        state: dict = {}
        while time.monotonic() < deadline:
            slice_ms = int(min(60.0, max(1.0, deadline - time.monotonic())) * 1000)
            state = self.endpoint.call("wait", **{"for": "cmake", "timeoutMs": slice_ms})
            if not state.get("timedOut") and done(state):
                return state
        return state

    def main(self) -> int:
        try:
            return self._main()
        finally:
            self._cleanup()
            self.evidence.write()
            print(f"{JOURNEY}: evidence {self.evidence.path}", flush=True)

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
        self.evidence.data["frost"] = {"image": str(frost), "sha256": sha256_file(frost) if frost.is_file() else None}
        try:
            cmake = corpora.pinned_tool("cmake", REPO)
            ninja = corpora.pinned_tool("ninja", REPO)
        except RuntimeError as error:
            self.evidence.step("setup", ok=False, note=str(error))
            return 1
        # The engine must be configured with the pinned cmake and Ninja too (FND-6b).
        problem = corpora.problem_or_none(entry, root) or corpora.engine_problem(
            corpus, entry, preset, {"cmake": cmake, "ninja": ninja})
        if problem:
            self.evidence.step("setup", ok=False, note=f"the pinned engine is not ready: {problem} "
                                                       f"(tools/corpora.py fetch; engine --configure)")
            return 1

        with corpora.BuildLock(corpora.build_lock_path(root, entry)):
            self.seed_path = corpus / SEED_FILE
            self._restore_seed_from_git(corpus)
            self.seed_original = self.seed_path.read_bytes()
            self._launch(corpus)
            self._journey(corpus, preset, cmake, ninja)
            self._quit()
        return 0 if all(step["ok"] for step in self.evidence.data["steps"]) else 1

    def _restore_seed_from_git(self, corpus: Path) -> None:
        """An interrupted run may have left the seeded line; the clone's own HEAD has the original."""
        git = corpora.Git()
        status = git.run("status", "--porcelain", "--", SEED_FILE, cwd=corpus).stdout
        if status.strip():
            git.run("checkout", "--", SEED_FILE, cwd=corpus)
            print(f"{JOURNEY}: restored {SEED_FILE} from the clone's HEAD (an earlier run left it changed)")

    def _launch(self, corpus: Path, *, again: bool = False) -> None:
        """Starts frost on a fresh temporary profile; with `again`, on the profile of the last start
        (which must have exited), as a restart of FrostIDE by its user would (j2_run's restart step)."""
        if not again:
            self.tmp = Path(tempfile.mkdtemp(prefix="frost-j1-"))
        profile, self.spawn_dir = self.tmp / "profile", self.tmp / "spawn-log"
        connection = self.tmp / "automation.json"
        if again:
            self.watchdog.cancel()
            connection.unlink(missing_ok=True)  # The last start's; the new frost writes its own.
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", FROST_LOG_LEVEL="debug",
                   FROST_SPAWN_LOG_DIR=str(self.spawn_dir), FROST_AUTOMATION_CONNECTION=str(connection))
        if WINDOWS:
            env["LOCALAPPDATA"], env["APPDATA"] = str(profile / "local"), str(profile / "roaming")
        else:
            env["HOME"] = str(profile / "home")
            for name, sub in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                              ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
                env[name] = str(profile / sub)
        for name in ("LOCALAPPDATA", "APPDATA", "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME",
                     "XDG_STATE_HOME"):
            if name in env and is_under(env[name], self.tmp):
                Path(env[name]).mkdir(parents=True, exist_ok=True)
        self.evidence.data["profile"] = str(profile)
        self.evidence.data["spawnLogDirectory"] = str(self.spawn_dir)
        self.frost_process = subprocess.Popen([os.path.realpath(self.args.frost)], env=env, cwd=str(self.tmp),
                                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                              stderr=subprocess.DEVNULL, start_new_session=not WINDOWS)
        # The watchdog: past the journey's limit frost is killed, so a blocked read ends with EOF.
        self.watchdog = threading.Timer(JOURNEY_SECONDS, lambda: kill_tree(self.frost_process))
        self.watchdog.daemon = True
        self.watchdog.start()

        def connected() -> tuple[bool, Any, Any, str]:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline:
                if self.frost_process.poll() is not None:
                    return False, None, None, f"frost exited with {self.frost_process.returncode} before listening"
                if connection.is_file():
                    try:
                        info = json.loads(connection.read_text(encoding="utf-8"))
                    except json.JSONDecodeError:
                        info = None
                    if info:
                        self.endpoint = Endpoint(info["server"], info["token"])
                        return (info.get("protocol") == PROTOCOL and info.get("pid") == self.frost_process.pid,
                                {"protocol": PROTOCOL, "pid": self.frost_process.pid},
                                {"protocol": info.get("protocol"), "pid": info.get("pid")}, "")
                time.sleep(POLL_SECONDS)
            return False, None, None, "no connection file within 120 s (a build without the endpoint?)"

        if not self.run_step("connect again (the same profile)" if again else "connect", connected):
            raise SystemExit(self._fail_out())

    def _fail_out(self) -> int:
        self._cleanup()
        self.evidence.write()
        print(f"{JOURNEY}: evidence {self.evidence.path}", flush=True)
        return 1

    def _journey(self, corpus: Path, preset: str, cmake: str, ninja: str) -> None:
        ep = self.endpoint
        build_dir = corpus / "build" / preset

        def open_and_trust() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(corpus))
            state = ep.call("state")
            if not state["cmake"]["available"]:
                return False, None, state, "the folder did not open as a CMake project"
            # The platform's preset first, so the configure the trust grant starts is that one.
            ep.call("cmake.select", name=preset)
            ep.call("trust", root=str(corpus))
            state = ep.call("state")
            return (same_path(state["project"], corpus), str(corpus), state["project"], "")

        self.run_step("open and trust the corpus folder", open_and_trust)

        def presets() -> tuple[bool, Any, Any, str]:
            oracle = listed_presets(cmake, corpus)
            listed = ep.call("cmake.profiles")
            configure = sorted({p["configurePreset"] for p in listed["profiles"] if p["configurePreset"]})
            build = sorted({p["buildPreset"] for p in listed["profiles"] if p["buildPreset"]})
            observed = {"configure": configure, "build": build, "presetsError": listed.get("presetsError")}
            expected = {"configure": sorted(oracle.get("configure", [])), "build": sorted(oracle.get("build", []))}
            ok = configure == expected["configure"] and set(build) <= set(expected["build"])
            return ok, expected, observed, "" if ok else "the profiles differ from cmake --list-presets=all"

        self.run_step("presets equal cmake --list-presets=all", presets, needs="CMK-2")

        def configure() -> tuple[bool, Any, Any, str]:
            def configured(state: dict) -> bool:
                last = state["cmake"].get("lastConfigure")
                return bool(last) and last.get("profile") == state["cmake"].get("activeProfile")

            state = self.wait_cmake(120, configured)
            if not configured(state):  # The trust grant started nothing: Reload CMake Project.
                ep.call("cmake.configure")
                state = self.wait_cmake(CONFIGURE_SECONDS, configured)
            cm = state.get("cmake", {})
            last = cm.get("lastConfigure") or {}
            observed = {"profile": cm.get("activeProfile"), "lastConfigure": last,
                        "buildDirectory": cm.get("buildDirectory"), "modelLoaded": cm.get("modelLoaded")}
            ok = (last.get("succeeded") is True and last.get("exitCode") == 0 and cm.get("modelLoaded") is True
                  and same_path(cm.get("buildDirectory", ""), build_dir))
            return ok, {"exitCode": 0, "buildDirectory": str(build_dir), "preset": preset}, observed, ""

        configured_ok = self.run_step(f"configure {preset}", configure)

        def configure_used_pinned_tools() -> tuple[bool, Any, Any, str]:
            # The R1 exit names the pinned cmake and Ninja (sdk/sdk.lock.json): the configure's cmake
            # image and the make program it hands to the build tree.
            records = spawn_records(self.spawn_dir)
            runs = [r for r in records if r.get("event") == "start" and Path(r.get("executable", "")).stem.lower()
                    == "cmake" and "--build" not in r.get("argv", []) and "--version" not in r.get("argv", [])]
            programs = [a.split("=", 1)[1] for r in runs for a in r.get("argv", [])
                        if a.startswith("-DCMAKE_MAKE_PROGRAM")]
            observed = {"cmake": sorted({r["executable"] for r in runs}), "makeProgram": sorted(set(programs))}
            expected = {"cmake": [cmake], "makeProgram": [ninja]}
            ok = bool(runs) and all(same_path(r["executable"], cmake) for r in runs) and \
                all(same_path(p, ninja) for p in programs)
            return ok, expected, observed, "" if ok else "the configure did not run the pinned cmake and Ninja"

        self.run_step("the configure ran the pinned cmake and Ninja (spawn log)", configure_used_pinned_tools,
                      needs="CMK-1")

        def configure_applied_preset() -> tuple[bool, Any, Any, str]:
            runs = [r.get("argv", []) for r in spawn_records(self.spawn_dir) if r.get("event") == "start"
                    and Path(r.get("executable", "")).stem.lower() == "cmake" and "--build" not in r.get("argv", [])
                    and "--version" not in r.get("argv", [])]
            if not runs:
                return False, None, None, "the spawn log has no configure"
            oracle = preset_cache(cmake, corpus, preset, self.tmp / "preset-oracle")
            switches = configure_switches(runs[-1])
            differences = preset_differences(oracle, switches)
            binary = switches.get("binaryDir", "")
            ok = not differences and bool(binary) and same_path(binary, build_dir)
            return (ok, {"cmake --preset": {k: v for k, v in oracle.items() if v[0] == "UNINITIALIZED"},
                         "binaryDir": str(build_dir)},
                    {"argv": runs[-1], "differences": differences},
                    "" if ok else "the configure does not apply what cmake --preset applies")

        self.run_step(f"the configure applies what cmake --preset {preset} applies (cmake's own resolution)",
                      configure_applied_preset, needs="CMK-2")

        first_module, first_file, _ = NAVIGATION[0]
        source = corpus / first_file

        def clangd_runs() -> tuple[bool, Any, Any, str]:
            ep.call("open", path=str(source))
            state = ep.call("wait", **{"for": "clangd", "timeoutMs": CLANGD_SECONDS * 1000})
            observed = state.get("clangd")
            return (not state.get("timedOut") and str(observed.get("state")).lower() == "running", "running",
                    observed, "")

        clangd_ok = self.run_step("clangd runs for an opened source file", clangd_runs)

        def owned_database() -> tuple[bool, Any, Any, str]:
            clangd = ep.call("state")["clangd"]
            directory = clangd.get("compileCommandsDirectory") or ""
            database = Path(directory) / "compile_commands.json"
            holds = False
            if database.is_file():
                holds = any(same_path(Path(item.get("directory", "")) / item.get("file", ""), source)
                            for item in json.loads(database.read_text(encoding="utf-8")))
            observed = {"directory": directory, "source": clangd.get("compileCommandsSource"),
                        "holdsOpenedFile": holds}
            ok = bool(directory) and not is_under(directory, corpus) and holds
            return (ok, {"outsideCorpus": True, "holdsOpenedFile": True, "configuration": preset}, observed,
                    "" if ok else "clangd does not use an IDE-owned database for this configuration")

        self.run_step("clangd uses the IDE-owned compilation database", owned_database, needs="CPP-3")

        if clangd_ok:
            self._navigation(corpus)

        if configured_ok:
            self._seeded_build(corpus)
        else:
            self.evidence.step("seeded build", ok=False, note="not run: the configure failed")

    def _navigation(self, corpus: Path) -> None:
        ep = self.endpoint
        clangd = ep.call("state")["clangd"]
        tools = tools_seen(spawn_records(self.spawn_dir))
        started = tools.get("clangd.exe" if WINDOWS else "clangd")
        if not started:
            self.evidence.step("Go to Definition oracle", ok=False, note="the spawn log has no clangd start")
            return
        self.oracle = ClangdOracle(started["executable"], clangd.get("compileCommandsDirectory") or "", corpus)
        for module, relative, needle in NAVIGATION:
            path = corpus / relative

            def navigate(path: Path = path, needle: str = needle) -> tuple[bool, Any, Any, str]:
                line, column = caret_of(path, needle)
                expected = self.oracle.definition(path, line - 1, column - 1)
                ep.call("open", path=str(path), line=line, column=column)
                editor = ep.call("editor")
                if not (editor.get("available") and same_path(editor["path"], path) and editor["line"] == line):
                    return False, expected, editor, "the editor did not open the caret position"
                ep.call("navigate.definition")
                deadline = time.monotonic() + NAVIGATE_SECONDS
                while time.monotonic() < deadline:
                    editor = ep.call("editor")
                    if editor.get("available") and not (same_path(editor["path"], path) and editor["line"] == line):
                        break
                    time.sleep(POLL_SECONDS)
                observed = {"path": editor.get("path"), "line": editor.get("line")}
                ok = bool(expected) and any(same_path(item["path"], observed["path"] or "") and
                                            item["line"] == observed["line"] for item in expected)
                return ok, expected, observed, "" if ok else "Go to Definition landed elsewhere than clangd says"

            self.run_step(f"Go to Definition in {module} ({needle})", navigate)

    def _seeded_build(self, corpus: Path) -> None:
        ep = self.endpoint
        original = self.seed_original or b""
        newline = b"\r\n" if b"\r\n" in original else b"\n"
        anchor = SEED_ANCHOR[0].encode() + newline + SEED_ANCHOR[1].encode() + newline
        at = original.find(anchor)
        if at < 0:
            self.evidence.step("seed a compile error", ok=False, note=f"the anchor is not in {SEED_FILE} at this pin")
            return
        insert_at = at + len(anchor)
        seeded_line = original[:insert_at].count(b"\n") + 1
        seeded = original[:insert_at] + SEED_TEXT.encode() + newline + original[insert_at:]

        def finished(state: dict) -> bool:
            return bool(state["cmake"].get("lastBuild")) and state["cmake"].get("state") == "Idle"

        def build_seeded() -> tuple[bool, Any, Any, str]:
            self.seed_path.write_bytes(seeded)
            before = len(spawn_records(self.spawn_dir))
            ep.call("cmake.build", action="build", target=SEED_TARGET)
            state = self.wait_cmake(BUILD_SECONDS, finished)
            last = state["cmake"].get("lastBuild") or {}
            builds = [r.get("argv") for r in spawn_records(self.spawn_dir)[before:] if r.get("event") == "start"]
            problems = ep.call("problems")["problems"]
            errors = [p for p in problems if p["severity"] == "error" and p.get("file") and
                      same_path(p["file"], self.seed_path)]
            observed = {"lastBuild": last, "errorsInSeededFile": errors, "launches": builds}
            ok = last.get("succeeded") is False and any(p["line"] == seeded_line and p["jumpable"] for p in errors)
            self.seeded_index = next((problems.index(p) for p in errors if p["line"] == seeded_line), None)
            return ok, {"file": str(self.seed_path), "line": seeded_line, "succeeded": False}, observed, ""

        built = self.run_step("a seeded compile error shows in Problems at its line", build_seeded)

        def activate() -> tuple[bool, Any, Any, str]:
            if getattr(self, "seeded_index", None) is None:
                return False, None, None, "no problem at the seeded line to activate"
            # Problems is a live list: clangd's diagnostics for files opened earlier can arrive between
            # reading it and activating an entry, and shift the indices. So the seeded problem's index
            # is read again right before each activation, and an activation that opened another
            # location (the list moved in between) is retried. Every attempt is in the evidence.
            attempts: list[dict] = []
            editor: dict = {}
            for _ in range(3):
                problems = ep.call("problems")["problems"]
                index = next((i for i, p in enumerate(problems) if p["severity"] == "error" and p.get("file")
                              and same_path(p["file"], self.seed_path) and p["line"] == seeded_line), None)
                if index is None:
                    attempts.append({"index": None, "note": "the seeded problem is no longer listed"})
                    break
                ep.call("problems.activate", index=index)
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    editor = ep.call("editor")
                    if editor.get("available") and same_path(editor["path"], self.seed_path):
                        break
                    time.sleep(POLL_SECONDS)
                attempts.append({"index": index, "editor": editor})
                if editor.get("available") and same_path(editor.get("path", ""), self.seed_path):
                    break
            ok = editor.get("available") and same_path(editor.get("path", ""), self.seed_path) and \
                editor.get("line") == seeded_line
            return bool(ok), {"path": str(self.seed_path), "line": seeded_line}, {"editor": editor,
                                                                                    "attempts": attempts}, ""

        if built:
            self.run_step("activating the problem opens the seeded line", activate)

        def fix_and_rebuild() -> tuple[bool, Any, Any, str]:
            self.seed_path.write_bytes(original)
            ep.call("cmake.build", action="build", target=SEED_TARGET)
            state = self.wait_cmake(BUILD_SECONDS, finished)
            last = state["cmake"].get("lastBuild") or {}
            problems = ep.call("problems")["problems"]
            left = [p for p in problems if p["severity"] == "error" and p["source"] == "build"]
            ok = last.get("succeeded") is True and not left
            return ok, {"succeeded": True, "buildErrors": 0}, {"lastBuild": last, "buildErrors": left}, ""

        self.run_step("the fix rebuilds without errors", fix_and_rebuild)

        def clean() -> tuple[bool, Any, Any, str]:
            ep.call("cmake.build", action="clean")
            state = self.wait_cmake(BUILD_SECONDS, finished)
            last = state["cmake"].get("lastBuild") or {}
            ok = last.get("action") == "Clean" and last.get("succeeded") is True
            return ok, {"action": "Clean", "succeeded": True}, last, ""

        self.run_step("clean", clean)

        def build_runs_the_build_preset() -> tuple[bool, Any, Any, str]:
            # From the clean tree (the Clean above) every edge of the target is due in both builds.
            listed = ep.call("cmake.profiles")
            active = next((p for p in listed["profiles"] if p["name"] == listed.get("active")), {})
            build_preset = active.get("buildPreset") or ""
            if not build_preset:
                return False, "a build preset", active, "the active profile has no build preset"
            builds = [r for r in spawn_records(self.spawn_dir) if r.get("event") == "start"
                      and "--build" in r.get("argv", []) and SEED_TARGET in r.get("argv", [])]
            if not builds:
                return False, None, None, f"the spawn log has no build of {SEED_TARGET}"
            ours = builds[-1]
            cleans = [r for r in spawn_records(self.spawn_dir) if r.get("event") == "start"
                      and "--build" in r.get("argv", []) and "clean" in r.get("argv", [])]
            if not cleans:
                return False, None, None, "the spawn log has no Clean"
            # The compiler's environment: the MSVC developer environment on Windows, as FrostIDE's.
            env = corpora.msvc_environment(REPO) if WINDOWS else dict(os.environ)
            cmake = ours["executable"]
            oracle = build_edges([cmake, "--build", "--preset", build_preset, "--target", SEED_TARGET], str(corpus),
                                 env)
            build_edges(cleans[-1]["argv"], cleans[-1].get("cwd", ""), env)
            try:
                observed = build_edges(ours["argv"], ours.get("cwd", ""), env)
            finally:
                build_edges(cleans[-1]["argv"], cleans[-1].get("cwd", ""), env)
            ok = bool(oracle) and observed == oracle
            return (ok, {"command": f"cmake --build --preset {build_preset} --target {SEED_TARGET}",
                         "edges": oracle},
                    {"argv": ours["argv"], "edges": observed},
                    "" if ok else "the build does not run what cmake --build --preset runs")

        self.run_step("the build runs what cmake --build --preset runs (the same Ninja edges)",
                      build_runs_the_build_preset, needs="CMK-2")

    def _quit(self) -> None:
        if self.endpoint is None or self.frost_process is None:
            return
        try:
            self.endpoint.call("quit")
        except (EndpointError, OSError):
            pass
        try:
            self.frost_process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            pass
        self._stall_step()

    def freezes_directory(self) -> Path:
        """Where frost's own stall watchdog wrote this run's freeze reports: the profile's cache
        (frost/diag/paths.hpp: %LOCALAPPDATA%\\FrostIDE\\caches, $XDG_CACHE_HOME/FrostIDE)."""
        profile = self.tmp / "profile"
        cache = profile / "local" / "FrostIDE" / "caches" if WINDOWS else profile / "cache" / "FrostIDE"
        return cache / "crashes" / "freezes"

    def _stall_step(self) -> None:
        """FND-9b (ADR 0078 decision 7): no UI-thread stall over STALL_LIMIT_MS in the whole run. The
        oracle is the freeze reports frost's StallWatchdog wrote (one per stall of 50 ms or more,
        ongoing ones included); they are kept beside the evidence for tools/freeze_triage.py."""
        def no_stall() -> tuple[bool, Any, Any, str]:
            reports = []
            directory = self.freezes_directory()
            kept = self.evidence.path.with_suffix(".freezes")
            for path in sorted(directory.glob("freeze-*.json")) if directory.is_dir() else []:
                try:
                    report = json.loads(path.read_text(encoding="utf-8"))
                except (OSError, json.JSONDecodeError) as error:
                    report = {"duration_ms": None, "error": str(error)}
                reports.append({"file": path.name, "durationMs": report.get("duration_ms"),
                                "ongoing": bool(report.get("ongoing"))})
                kept.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, kept / path.name)
            over = [r for r in reports if r["durationMs"] is None or r["durationMs"] > STALL_LIMIT_MS]
            longest = max((r["durationMs"] or 0 for r in reports), default=0)
            return (not over, {"overLimitMs": STALL_LIMIT_MS, "reportsOver": []},
                    {"reports": len(reports), "longestMs": longest, "reportsOver": over,
                     "kept": str(kept) if reports else None},
                    "" if not over else f"{len(over)} stalls over {STALL_LIMIT_MS} ms (tools/freeze_triage.py "
                                        f"triage {kept})")

        self.run_step(f"no UI stall over {STALL_LIMIT_MS} ms", no_stall)

    def _cleanup(self) -> None:
        if self.oracle is not None:
            self.oracle.close()
            self.oracle = None
        if self.endpoint is not None:
            self.endpoint.close()
            self.endpoint = None
        if self.frost_process is not None:
            kill_tree(self.frost_process)
            if hasattr(self, "watchdog"):
                self.watchdog.cancel()
            if hasattr(self, "spawn_dir"):
                records = spawn_records(self.spawn_dir)
                # The spawn logs are kept beside the evidence; the temporary profile is removed.
                kept = self.evidence.path.with_suffix(".spawn")
                kept.mkdir(parents=True, exist_ok=True)
                for log in sorted(self.spawn_dir.glob("spawn-*.jsonl")):
                    shutil.copy2(log, kept / log.name)
                self.evidence.data["spawnLogs"] = [str(p) for p in sorted(kept.glob("spawn-*.jsonl"))]
                self.evidence.data["tools"] = tools_seen(records)
                self.evidence.data["launches"] = sum(1 for r in records if r.get("event") == "start")
            self.frost_process = None
            if hasattr(self, "tmp"):
                shutil.rmtree(self.tmp, ignore_errors=True)
        if self.seed_path is not None and self.seed_original is not None:
            if self.seed_path.read_bytes() != self.seed_original:
                self.seed_path.write_bytes(self.seed_original)
            self.seed_original = None


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
