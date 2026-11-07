# SPDX-License-Identifier: Apache-2.0
#
# The R0 exit criterion as an automated test, on both legs (Windows and WSL): the build this ctest
# tree holds is staged (tools/stage.ps1 -BundleTools, tools/stage.sh) into a temporary folder; the
# staged frost runs offscreen on a fresh profile with a trusted folder and opens a .lua file in it;
# frost starts the staged LuaLS through IProcessLauncher (the spawn log, frost/io/spawn_log.hpp,
# records the launch with the image's SHA-256); and frost shows LuaLS's undefined-global diagnostic
# (the editor's diagnostics, logged by Workbench::onLuaDiagnostics at debug level).
#
# Oracles. sdk/sdk.lock.json pins the LuaLS release archive by SHA-256: the archive in the SDK's
# _downloads must match it, the staged executable must be byte-identical to the archive's
# bin/lua-language-server member, and the spawn log's digest of what frost started must be the
# member's. The staged LuaLS's own --version output must name the pinned version. The diagnostic's
# code, line and global name are LuaLS's; the test only chooses the source file.
#
# Needs a Debug build tree (the spawn log is compiled into Debug frost only, FROST_ENABLE_SPAWN_LOG)
# named by FROST_TEST_TREE and FROST_TEST_CONFIG (tests/tools/CMakeLists.txt), and FROST_SDK with the
# pinned tools. FROST_SDK and FROST_LLVM_ROOT are removed from frost's environment, so only the stage
# can supply the tools. Every process the run started is killed, and the test checks that none
# whose image lies in the stage is left.

from __future__ import annotations

import hashlib
import json
import ntpath
import os
import signal
import subprocess
import sys
import tarfile
import time
import unittest
import zipfile
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env

WINDOWS = sys.platform == "win32"
LEG = "windows" if WINDOWS else "linux"
LOCK = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))[LEG]["luals"]
LUALS_EXE = "lua-language-server.exe" if WINDOWS else "lua-language-server"
# Generous: a cold LuaLS start on a loaded machine; the waits poll and end as soon as the result is there.
DEADLINE_SECONDS = 240.0
POLL_SECONDS = 0.25
GLOBAL = "frostStagedUndefinedGlobal"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def same_path(a: str | Path, b: str | Path) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def under(path: str, root: Path) -> bool:
    real, base = os.path.normcase(os.path.realpath(path)), os.path.normcase(os.path.realpath(root))
    return real.startswith(base + os.sep)


def wait_for(check, what: str):
    """Polls `check` until it returns something truthy (returned) or the deadline passes (fails)."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while True:
        result = check()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {DEADLINE_SECONDS:.0f} s waiting for {what}")
        time.sleep(POLL_SECONDS)


def _system32(name: str) -> str:
    return ntpath.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", name)


def processes_in(root: Path) -> list[int]:
    """The pids of running processes whose executable lies under `root`."""
    if WINDOWS:
        script = ("Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath } | "
                  "ForEach-Object { \"$($_.ProcessId)`t$($_.ExecutablePath)\" }")
        out = subprocess.run([_system32(r"WindowsPowerShell\v1.0\powershell.exe"), "-NoProfile", "-NonInteractive",
                              "-Command", script], capture_output=True, text=True, errors="replace", check=False)
        pids = []
        for line in out.stdout.splitlines():
            pid, _, exe = line.partition("\t")
            if pid.strip().isdigit() and exe and under(exe.strip(), root):
                pids.append(int(pid))
        return pids
    pids = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                exe = os.readlink(entry / "exe")
            except OSError:
                continue
            if under(exe.removesuffix(" (deleted)"), root):
                pids.append(int(entry.name))
    return pids


def kill_tree(pid: int) -> None:
    if WINDOWS:
        subprocess.run([_system32("taskkill.exe"), "/T", "/F", "/PID", str(pid)], capture_output=True, check=False)
        return
    for kill in (lambda: os.killpg(pid, signal.SIGKILL), lambda: os.kill(pid, signal.SIGKILL)):
        try:
            kill()
        except OSError:
            pass


@unittest.skipUnless(os.environ.get("FROST_TEST_TREE"), "run by ctest (FROST_TEST_TREE names the build tree)")
class StagedLuaLanguageServerTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.tree = Path(os.environ["FROST_TEST_TREE"])
        self.config = os.environ.get("FROST_TEST_CONFIG", "")
        if self.config != "Debug":
            self.skipTest(f"the spawn log is compiled into Debug frost only; this tree runs {self.config or '?'}")
        sdk = os.environ.get("FROST_SDK", "")
        if not sdk or not Path(sdk).is_dir():
            self.fail("FROST_SDK must name the pinned SDK (tools/env.ps1, ~/sdk/env.sh)")
        self.sdk = Path(sdk)
        self.stage = self.tmp / "stage"
        self.started: list[int] = []
        self.children: list[subprocess.Popen] = []

    def tearDown(self) -> None:
        for pid in self.started:
            kill_tree(pid)
        for child in self.children:
            try:
                child.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.fail(f"frost (pid {child.pid}) did not end after it was killed")
        if hasattr(self, "stage") and self.stage.exists():
            leftovers = []
            deadline = time.monotonic() + 30
            while (leftovers := processes_in(self.stage)) and time.monotonic() < deadline:
                for pid in leftovers:
                    kill_tree(pid)
                time.sleep(POLL_SECONDS)
            if leftovers:
                self.fail(f"processes from the stage survived the test: {leftovers}")
        super().tearDown()

    # --- The oracle: the pinned archive and its executable ---------------------------------------

    def pinned_member(self) -> bytes:
        archive = self.sdk / "_downloads" / LOCK["file"]
        self.assertTrue(archive.is_file(), f"the pinned LuaLS archive {archive} (tools/bootstrap)")
        data = archive.read_bytes()
        self.assertEqual(sha256_bytes(data), LOCK["sha256"], f"{archive} is not the archive sdk.lock.json pins")
        if WINDOWS:
            with zipfile.ZipFile(archive) as packed:
                return packed.read(f"bin/{LUALS_EXE}")
        with tarfile.open(archive) as packed:
            member = packed.extractfile(f"bin/{LUALS_EXE}")
            self.assertIsNotNone(member)
            return member.read()

    # --- Staging ---------------------------------------------------------------------------------

    def stage_build(self) -> None:
        env = clean_env()
        if WINDOWS:
            powershell = _system32(r"WindowsPowerShell\v1.0\powershell.exe")
            command = [powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                       str(TOOLS / "stage.ps1"), "-Destination", str(self.stage), "-Config", "Debug", "-SkipBuild",
                       "-BundleTools", "-BuildDir", str(self.tree), "-SdkRoot", str(self.sdk), "-Python",
                       sys.executable]
        else:
            command = ["bash", str(TOOLS / "stage.sh"), "--dest", str(self.stage), "--config", "Debug",
                       "--skip-build", "--build-dir", str(self.tree), "--sdk-root", str(self.sdk)]
        result = subprocess.run(command, capture_output=True, text=True, errors="replace", env=env, check=False)
        self.assertEqual(result.returncode, 0, f"staging failed:\n{result.stdout}\n{result.stderr}")
        self.assertIn("stage: ok", result.stdout)

    def frost_environment(self, profile: Path, spawn_log: Path) -> dict[str, str]:
        env = clean_env(QT_QPA_PLATFORM="offscreen", FROST_LOG_LEVEL="debug", FROST_SPAWN_LOG_DIR=str(spawn_log))
        for name in ("FROST_SDK", "FROST_LLVM_ROOT", "FROST_TEST_TREE", "FROST_TEST_CONFIG"):
            env.pop(name, None)
        if WINDOWS:
            env["LOCALAPPDATA"] = str(profile / "local")
            env["APPDATA"] = str(profile / "roaming")
        else:
            env["HOME"] = str(profile / "home")
            for name, sub in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
                              ("XDG_STATE_HOME", "state")):
                env[name] = str(profile / sub)
            env.pop("XDG_RUNTIME_DIR", None)
        for value in (env.get("LOCALAPPDATA"), env.get("APPDATA"), env.get("HOME"), env.get("XDG_CONFIG_HOME"),
                      env.get("XDG_DATA_HOME"), env.get("XDG_CACHE_HOME"), env.get("XDG_STATE_HOME")):
            if value:
                Path(value).mkdir(parents=True, exist_ok=True)
        return env

    def trust(self, profile: Path, root: Path) -> None:
        """The fresh profile trusts `root` (frost/project/trust_store.hpp's file format)."""
        store = (profile / "local" / "FrostIDE" / "data" / "trusted-roots.txt" if WINDOWS
                 else profile / "data" / "FrostIDE" / "trusted-roots.txt")
        store.parent.mkdir(parents=True, exist_ok=True)
        store.write_text(f"# FrostIDE trusted roots v1\n{os.path.realpath(root)}\n", encoding="utf-8")

    # --- The journey -----------------------------------------------------------------------------

    def test_a_staged_build_starts_the_pinned_luals_and_shows_its_diagnostic(self) -> None:
        pinned = self.pinned_member()
        self.stage_build()
        staged_exe = self.stage / f"luals-{LOCK['version']}" / "bin" / LUALS_EXE
        self.assertTrue(staged_exe.is_file(), f"the stage has no {staged_exe}")
        self.assertEqual(sha256_bytes(staged_exe.read_bytes()), sha256_bytes(pinned),
                         "the staged LuaLS is not the pinned archive's executable")
        version = subprocess.run([str(staged_exe), "--version"], capture_output=True, text=True, errors="replace",
                                 timeout=120, check=False)
        self.assertIn(LOCK["version"], version.stdout + version.stderr, "the staged LuaLS's own --version")

        profile, spawn_log, project = self.tmp / "profile", self.tmp / "spawn-log", self.tmp / "project"
        project.mkdir()
        source = project / "main.lua"
        source.write_text(f"local answer = 42\nprint(answer, {GLOBAL})\n", encoding="utf-8")
        env = self.frost_environment(profile, spawn_log)
        self.trust(profile, project)
        frost = self.stage / ("frost.exe" if WINDOWS else "frost")
        process = subprocess.Popen([str(frost), os.path.realpath(project), os.path.realpath(source)], env=env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   cwd=str(self.tmp), start_new_session=not WINDOWS)
        self.started.append(process.pid)
        self.children.append(process)

        def luals_start() -> dict | None:
            if process.poll() is not None:
                raise AssertionError(f"frost exited early with {process.returncode}")
            for log in spawn_log.glob("spawn-*.jsonl"):
                for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                    record = json.loads(line)
                    if record.get("event") == "start" and Path(record["executable"]).name == LUALS_EXE:
                        return record
            return None

        start = wait_for(luals_start, "frost to start LuaLS (the spawn log)")
        self.started.append(int(start["pid"]))
        self.assertTrue(same_path(start["executable"], staged_exe),
                        f"frost started {start['executable']}, not the staged {staged_exe}")
        self.assertEqual(start["sha256"], sha256_bytes(pinned), "the spawned image is not the pinned LuaLS")
        self.assertTrue(Path(start["executable"]).is_absolute())

        def shown() -> str | None:
            if process.poll() is not None:
                raise AssertionError(f"frost exited early with {process.returncode}")
            for log in profile.rglob("frost-*.log"):
                for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                    if "Lua diagnostic shown:" in line and "[undefined-global]" in line and GLOBAL in line:
                        return line
            return None

        line = wait_for(shown, "frost to show LuaLS's undefined-global diagnostic")
        self.assertIn("main.lua:2 [undefined-global]", line, "LuaLS reports the use on line 2")


if __name__ == "__main__":
    # A skip exits with 77, which ctest reports as skipped (SKIP_RETURN_CODE), never as passed.
    outcome = unittest.main(exit=False).result
    if not outcome.wasSuccessful():
        sys.exit(1)
    sys.exit(77 if outcome.skipped else 0)
