# SPDX-License-Identifier: Apache-2.0
#
# Tests for the Windows PowerShell scripts in tools/ that can run without installing
# anything: bootstrap.ps1's Python version check, taken from the script's syntax tree
# so the bootstrap itself never runs, env.ps1's cache redirection (D1: no tool
# caches on the system drive), dot-sourced in a child PowerShell, and stage.ps1's
# folder layout over a fake build tree and a fake windeployqt: the bundled tools it requires
# (FND-13), LuaLS among them, the symbol archive next to the folder, and that a failed stage leaves
# no partial folder (input checks first, cleanup after late and unexpected errors). Windows only: the
# scripts target Windows PowerShell 5.1. tests/tools/CMakeLists.txt registers this
# suite on Windows only.

from __future__ import annotations

import contextlib
import ctypes
import json
import ntpath
import os
import shutil
import subprocess
import sys
import unittest
import zipfile
from collections.abc import Iterator
from pathlib import Path

from tools_test_support import TOOLS, TempDirTestCase, clean_env

# The synthetic PE images and PDBs of the symbolize.py tests (a module import, so unittest does not
# collect that suite's tests here a second time).
import test_symbolize as symbol_fixtures

_FROST_GUID = bytes(range(16, 32))
_APP_GUID = bytes(range(32, 48))


@contextlib.contextmanager
def _exclusively_open(path: Path) -> Iterator[None]:
    """Holds `path` open with no sharing (CreateFileW, share mode 0), so every other open of it fails
    with a sharing violation until the block ends. Windows only."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    generic_read, open_existing, invalid_handle = 0x80000000, 3, ctypes.c_void_p(-1).value
    handle = kernel32.CreateFileW(str(path), generic_read, 0, None, open_existing, 0, None)
    if handle in (None, invalid_handle):
        raise OSError(ctypes.get_last_error(), f"cannot open {path} exclusively")
    try:
        yield
    finally:
        kernel32.CloseHandle(handle)


def _powershell() -> str:
    """The absolute path of Windows PowerShell 5.1."""
    root = os.environ.get("SystemRoot", r"C:\Windows")
    return ntpath.join(root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe")


def run_powershell(script: Path, *args: str) -> subprocess.CompletedProcess:
    """Runs `script` with -File (no profile, non-interactive); returns the completed process."""
    return subprocess.run(
        [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(script), *args],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=clean_env(), check=False,
    )


# Defines $MinPython and Assert-PythonVersion from bootstrap.ps1's syntax tree, then calls
# the function once per version in the JSON array $CasesFile and prints "ok <input>" or
# "fail <input>: <message>" per case (a file, because -File cannot pass an empty string
# or a line break reliably).
_ASSERT_PYTHON_VERSION = r"""
param([string]$Script, [string]$CasesFile)
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 emits a JSON array as one object, so no @() here: that would nest it.
$Versions = Get-Content -Raw -Path $CasesFile | ConvertFrom-Json
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Script, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { throw "bootstrap.ps1 does not parse: $($errors[0])" }
$minimum = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
    $n.Left.Extent.Text -eq '$MinPython' }, $false)
$function = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
    $n.Name -eq 'Assert-PythonVersion' }, $true)
if (-not $minimum -or -not $function) { throw 'bootstrap.ps1 lacks $MinPython or Assert-PythonVersion' }
. ([scriptblock]::Create($minimum.Extent.Text + "`n" + $function.Extent.Text))
foreach ($version in $Versions) {
    $shown = $version -replace "`r", '\r' -replace "`n", '\n'
    try { Assert-PythonVersion $version 'X:\venv\Scripts\python.exe' 'X:\venv'; "ok $shown" }
    catch { "fail ${shown}: $($_.Exception.Message)" }
}
"""

# Dot-sources env.ps1 with the given SDK root and prints the variables it sets as NAME=VALUE. It clears them
# first: the child inherits this process's environment, so a value left by an earlier dot-sourcing of env.ps1
# would otherwise pass for one that env.ps1 does not set.
_ENV_DUMP = r"""
param([string]$EnvScript, [string]$SdkRoot)
$ErrorActionPreference = 'Stop'
$names = @('FROST_SDK', 'DOTNET_CLI_HOME', 'NUGET_PACKAGES', 'NUGET_HTTP_CACHE_PATH', 'NUGET_PLUGINS_CACHE_PATH',
           'SCCACHE_DIR', 'VCPKG_DOWNLOADS', 'VCPKG_DEFAULT_BINARY_CACHE', 'X_VCPKG_REGISTRIES_CACHE',
           'DOTNET_ADD_GLOBAL_TOOLS_TO_PATH', 'DOTNET_GENERATE_ASPNET_CERTIFICATE')
foreach ($name in $names) { Remove-Item -LiteralPath "Env:\$name" -ErrorAction SilentlyContinue }
. $EnvScript -SdkRoot $SdkRoot 3>$null
foreach ($name in $names) {
    "$name=$([Environment]::GetEnvironmentVariable($name))"
}
"""


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell scripts")
class BootstrapPythonCheckTest(TempDirTestCase):
    def test_versions_below_3_11_or_unreadable_stop_the_bootstrap(self) -> None:
        script = self.tmp / "assert_python_version.ps1"
        script.write_text(_ASSERT_PYTHON_VERSION, encoding="utf-8", newline="\n")
        accepted = ["3.11.0", "3.14.8", "3.12.1\r\n"]
        too_old = ["3.10.12", "2.7.18"]
        unreadable = ["", "Python 3.12.1", "3.12"]
        cases_file = self.tmp / "cases.json"
        cases_file.write_text(json.dumps(accepted + too_old + unreadable), encoding="utf-8", newline="\n")
        result = run_powershell(script, str(TOOLS / "bootstrap.ps1"), str(cases_file))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(len(lines), len(accepted + too_old + unreadable), result.stdout)
        for line in lines[:len(accepted)]:
            self.assertTrue(line.startswith("ok "), line)
        for line in lines[len(accepted):len(accepted + too_old)]:
            self.assertTrue(line.startswith("fail "), line)
            self.assertIn("need Python 3.11 or newer", line)
            self.assertIn(r"delete X:\venv", line)
        for line in lines[len(accepted + too_old):]:
            self.assertTrue(line.startswith("fail "), line)
            self.assertIn("could not read the Python version", line)
            self.assertIn(r"delete X:\venv", line)


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell scripts")
class EnvCacheRedirectionTest(TempDirTestCase):
    def test_every_tool_cache_is_under_the_sdk_root(self) -> None:
        sdk = os.environ.get("FROST_SDK")
        if not sdk:
            self.skipTest("FROST_SDK is not set (dot-source tools/env.ps1 first)")
        system_drive = os.environ.get("SystemDrive", "C:").upper()
        if ntpath.splitdrive(sdk)[0].upper() == system_drive:
            self.skipTest("the SDK itself is on the system drive")
        script = self.tmp / "env_dump.ps1"
        script.write_text(_ENV_DUMP, encoding="utf-8", newline="\n")
        result = run_powershell(script, str(TOOLS / "env.ps1"), sdk)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
        sdk_drive = ntpath.splitdrive(sdk)[0].upper()
        # X_VCPKG_REGISTRIES_CACHE: vcpkg's registry clones, %LOCALAPPDATA%\vcpkg\registries when it is unset.
        for name in ("DOTNET_CLI_HOME", "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH", "NUGET_PLUGINS_CACHE_PATH",
                     "SCCACHE_DIR", "VCPKG_DOWNLOADS", "X_VCPKG_REGISTRIES_CACHE"):
            self.assertTrue(ntpath.normcase(values[name]).startswith(ntpath.normcase(sdk) + "\\"),
                            f"{name}={values[name]} is not under {sdk}")
            self.assertTrue(Path(values[name]).is_dir(), f"{name}: {values[name]} was not created")
        # The vcpkg binary cache sits at the root of the SDK drive.
        self.assertEqual(ntpath.splitdrive(values["VCPKG_DEFAULT_BINARY_CACHE"])[0].upper(), sdk_drive)
        # A new .NET CLI home must not edit the user PATH or create a certificate on first use.
        self.assertEqual(values["DOTNET_ADD_GLOBAL_TOOLS_TO_PATH"], "0")
        self.assertEqual(values["DOTNET_GENERATE_ASPNET_CERTIFICATE"], "0")


# A stand-in for windeployqt.exe: it writes a Qt core DLL and the Windows platform plugin into the
# directory after --dir, as the real tool does for a debug frost_app.dll.
_FAKE_WINDEPLOYQT = r"""@echo off
set "NO_PLUGINS="
set "NO_SVG="
:loop
if "%~1"=="" goto done
if "%~1"=="--dir" set "DEST=%~2"
if "%~1"=="--no-plugins" set "NO_PLUGINS=1"
if "%~1"=="--no-svg" set "NO_SVG=1"
shift
goto loop
:done
if defined NO_PLUGINS goto runtime
if not exist "%DEST%\generic" mkdir "%DEST%\generic"
echo plugin>"%DEST%\generic\qtuiotouchplugind.dll"
:runtime
if not defined NO_SVG echo qt>"%DEST%\Qt6Svgd.dll"
if not exist "%DEST%\platforms" mkdir "%DEST%\platforms"
echo qt>"%DEST%\Qt6Cored.dll"
echo plugin>"%DEST%\platforms\qwindowsd.dll"
exit /b 0
"""


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell scripts")
class StageScriptTest(TempDirTestCase):
    def _fake_build(self) -> Path:
        """A build tree with the files the Debug frost target leaves in src/app/Debug."""
        build = self.tmp / "build"
        bin_dir = build / "src" / "app" / "Debug"
        (bin_dir / "platforms").mkdir(parents=True, exist_ok=True)
        installed = self.tmp / "installed"
        notice = installed / "x64-fixture/share/libsodium/copyright"
        notice.parent.mkdir(parents=True, exist_ok=True)
        notice.write_bytes(b"Upstream ISC copyright fixture\n")
        (build / "CMakeCache.txt").write_text(
            f"VCPKG_INSTALLED_DIR:PATH={installed.as_posix()}\nVCPKG_TARGET_TRIPLET:STRING=x64-fixture\n",
            encoding="utf-8")
        (bin_dir / "tls").mkdir(parents=True, exist_ok=True)
        (bin_dir / "tls" / "qschannelbackendd.dll").write_bytes(b"MZ-TLS-fixture")
        (bin_dir / "resources" / "themes").mkdir(parents=True, exist_ok=True)
        (bin_dir / "project-host").mkdir(parents=True, exist_ok=True)
        (bin_dir / "project-host" / "ProjectHostVsProbe.exe").write_bytes(b"MZ")
        (bin_dir / "project-host" / "ProjectHostVsProbe.exe.config").write_text("<configuration/>", encoding="utf-8")
        for name in ("crashpad_handler.exe", "conpty.dll", "OpenConsole.exe"):
            (bin_dir / name).write_bytes(b"MZ")
        # frost.exe and frost_app.dll name their PDBs, whose GUID and age match (the symbol archive).
        (bin_dir / "frost.exe").write_bytes(symbol_fixtures.pe_bytes(_FROST_GUID, 1, "frost.pdb"))
        (bin_dir / "frost.pdb").write_bytes(symbol_fixtures.msf_bytes(_FROST_GUID))
        (bin_dir / "frost_app.dll").write_bytes(symbol_fixtures.pe_bytes(_APP_GUID, 1, "frost_app.pdb"))
        (bin_dir / "frost_app.pdb").write_bytes(symbol_fixtures.msf_bytes(_APP_GUID))
        (bin_dir / "platforms" / "qoffscreend.dll").write_bytes(b"MZ")
        (bin_dir / "resources" / "themes" / "dark.json").write_text("{}", encoding="utf-8")
        return build

    def _stage(self, build: Path, destination: Path, *extra: str) -> subprocess.CompletedProcess:
        deploy = self.tmp / "windeployqt.cmd"
        deploy.write_text(_FAKE_WINDEPLOYQT, encoding="ascii", newline="\r\n")
        return run_powershell(TOOLS / "stage.ps1", "-Destination", str(destination), "-Config", "Debug",
                              "-BuildDir", str(build), "-WinDeployQt", str(deploy), "-Python", sys.executable,
                              "-SkipBuild", *extra)

    def _fake_tools(self) -> list[str]:
        """An SDK root and managed folders holding every bundled tool; the -BundleTools arguments."""
        lock = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))["windows"]
        sdk = self.tmp / "sdk"
        files = [f"llvm-{lock['llvm']['version']}/bin/clangd.exe",
                 f"llvm-{lock['llvm']['version']}/lib/clang/23/include/stddef.h",
                 f"lldb-{lock['lldb']['version']}/bin/lldb-dap.exe",
                 f"lldb-{lock['lldb']['version']}/bin/liblldb.dll",
                 f"lldb-{lock['lldb']['version']}/bin/lldb-argdumper.exe",
                 f"luals-{lock['luals']['version']}/bin/lua-language-server.exe",
                 f"luals-{lock['luals']['version']}/main.lua",
                 f"luals-{lock['luals']['version']}/LICENSE",
                 f"luals-{lock['luals']['version']}/meta/template/basic.lua",
                 f"luals-{lock['luals']['version']}/.frost-installed",
                 f"luals-{lock['luals']['version']}/log/service.log"]
        for name in files:
            (sdk / name).parent.mkdir(parents=True, exist_ok=True)
            (sdk / name).write_bytes(b"fixture " + name.encode())
        managed, debug = self.tmp / "managed-server", self.tmp / "managed-debug"
        managed.mkdir(exist_ok=True)
        (managed / "CSharpLanguageServer.dll").write_bytes(b"MZ")
        (debug / "adapter").mkdir(parents=True, exist_ok=True)
        (debug / "adapter" / "netcoredbg.exe").write_bytes(b"MZ")
        return ["-BundleTools", "-SdkRoot", str(sdk), "-ManagedServerDir", str(managed),
                "-ManagedDebugDir", str(debug)]

    def _fake_asan_runtime(self, build: Path) -> tuple[Path, list[str]]:
        """Exercise cache-driven runtime staging with the existing prebuilt Debug fixture."""
        lock = json.loads((TOOLS.parent / "sdk/sdk.lock.json").read_text(encoding="utf-8"))["windows"]["llvm"]
        sdk = self.tmp / "sdk"
        runtime = (sdk / f"llvm-{lock['version']}/lib/clang/{lock['version'].split('.')[0]}/lib/windows/"
                   "clang_rt.asan_dynamic-x86_64.dll")
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.write_bytes(b"MZ-pinned-ASan-runtime-fixture")
        shutil.copyfile(runtime, build / "src/app/Debug" / runtime.name)
        with (build / "CMakeCache.txt").open("a", encoding="utf-8") as cache:
            cache.write("FROST_SANITIZE:STRING=address\n")
        return runtime, ["-SdkRoot", str(sdk)]

    def test_asan_stage_carries_the_verified_pinned_runtime(self) -> None:
        build = self._fake_build()
        runtime, arguments = self._fake_asan_runtime(build)
        destination = self.tmp / "asan-stage"
        result = self._stage(build, destination, *arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((destination / runtime.name).read_bytes(), runtime.read_bytes())
        self.assertTrue((destination / "frost_app.dll").is_file())

    def test_asan_input_provenance_is_checked_before_force_replaces_destination(self) -> None:
        for problem in ("missing pinned ASan runtime", "missing build ASan runtime",
                        "build ASan runtime differs"):
            with self.subTest(problem=problem):
                build = self._fake_build()
                runtime, arguments = self._fake_asan_runtime(build)
                sidecar = build / "src/app/Debug" / runtime.name
                if problem == "missing pinned ASan runtime":
                    runtime.unlink()
                elif problem == "missing build ASan runtime":
                    sidecar.unlink()
                else:
                    sidecar.write_bytes(b"MZ-unrelated-runtime")
                destination = self.tmp / "asan-preflight"
                destination.mkdir(exist_ok=True)
                marker = destination / "existing.txt"
                marker.write_bytes(b"preserve on input failure")
                result = self._stage(build, destination, *arguments, "-Force")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(problem, result.stdout)
                self.assertEqual(marker.read_bytes(), b"preserve on input failure")
                self.assertFalse((self.tmp / "asan-preflight-symbols.zip").exists())

    def test_non_asan_stage_does_not_bundle_an_incidental_asan_sidecar(self) -> None:
        build = self._fake_build()
        name = "clang_rt.asan_dynamic-x86_64.dll"
        (build / "src/app/Debug" / name).write_bytes(b"unrelated sidecar")
        destination = self.tmp / "normal-stage"
        result = self._stage(build, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((destination / name).exists())

    def test_tampered_staged_asan_runtime_fails_and_removes_partial_stage(self) -> None:
        build = self._fake_build()
        runtime, arguments = self._fake_asan_runtime(build)
        deploy = self.tmp / "tamper-asan-deploy.cmd"
        deploy.write_text(_FAKE_WINDEPLOYQT.replace("exit /b 0", f'echo changed>"%DEST%\\{runtime.name}"\nexit /b 0'),
                          encoding="ascii", newline="\r\n")
        destination = self.tmp / "asan-corrupt"
        result = run_powershell(TOOLS / "stage.ps1", "-Destination", str(destination), "-Config", "Debug",
                                "-BuildDir", str(build), "-WinDeployQt", str(deploy), "-Python", sys.executable,
                                "-SkipBuild", *arguments)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("staged ASan runtime differs", result.stdout)
        self.assertFalse(destination.exists())
        self.assertFalse((self.tmp / "asan-corrupt-symbols.zip").exists())

    def test_bundled_tools_and_luals_are_staged(self) -> None:
        build = self._fake_build()
        destination = self.tmp / "bundled"
        lock = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))["windows"]
        luals = f"luals-{lock['luals']['version']}"
        result = self._stage(build, destination, *self._fake_tools())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in (f"llvm-{lock['llvm']['version']}/bin/clangd.exe",
                     f"llvm-{lock['llvm']['version']}/lib/clang/23/include/stddef.h",
                     f"lldb-{lock['lldb']['version']}/bin/liblldb.dll", f"{luals}/bin/lua-language-server.exe",
                     f"{luals}/main.lua", f"{luals}/meta/template/basic.lua", "licenses/luals-LICENSE.txt",
                     "managed/CSharpLanguageServer.dll", "FrostManagedDebug/adapter/netcoredbg.exe"):
            self.assertTrue((destination / name).is_file(), name)
        self.assertFalse((destination / luals / ".frost-installed").exists(), "the SDK's install marker")
        self.assertFalse((destination / luals / "log").exists(), "a log a run left in the SDK")

    def test_the_managed_folders_default_to_their_environment_variables(self) -> None:
        build = self._fake_build()
        arguments = self._fake_tools()
        # Without -ManagedServerDir and -ManagedDebugDir, the FROST_MANAGED_* variables name them.
        explicit = arguments.index("-ManagedServerDir")
        arguments = arguments[:explicit]
        deploy = self.tmp / "windeployqt.cmd"
        deploy.write_text(_FAKE_WINDEPLOYQT, encoding="ascii", newline="\r\n")
        destination = self.tmp / "from-env"
        result = subprocess.run(
            [_powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
             str(TOOLS / "stage.ps1"), "-Destination", str(destination), "-Config", "Debug", "-BuildDir", str(build),
             "-WinDeployQt", str(deploy), "-Python", sys.executable, "-SkipBuild", *arguments],
            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
            env=clean_env(FROST_MANAGED_SERVER_DIR=str(self.tmp / "managed-server"),
                          FROST_MANAGED_DEBUG_DIR=str(self.tmp / "managed-debug")))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((destination / "managed/CSharpLanguageServer.dll").read_bytes(),
                         (self.tmp / "managed-server/CSharpLanguageServer.dll").read_bytes())
        self.assertTrue((destination / "FrostManagedDebug/adapter/netcoredbg.exe").is_file())

    def test_a_missing_bundled_tool_fails_before_staging(self) -> None:
        lock = json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(encoding="utf-8"))["windows"]
        cases = {"clangd": f"sdk/llvm-{lock['llvm']['version']}/bin/clangd.exe",
                 "lldb-dap": f"sdk/lldb-{lock['lldb']['version']}/bin/liblldb.dll",
                 "LuaLS": f"sdk/luals-{lock['luals']['version']}/bin/lua-language-server.exe",
                 "managed language server": "managed-server/CSharpLanguageServer.dll",
                 "managed debugger": "managed-debug/adapter/netcoredbg.exe"}
        for tool, missing in cases.items():
            with self.subTest(tool=tool):
                build = self._fake_build()
                arguments = self._fake_tools()
                (self.tmp / missing).unlink()
                destination = self.tmp / "missing-tool"
                result = self._stage(build, destination, *arguments)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"stage: FAIL missing bundled {tool}", result.stdout)
                self.assertFalse(destination.exists(), "nothing is staged without the tool")

    def test_the_symbol_archive_holds_the_images_and_their_pdbs(self) -> None:
        build = self._fake_build()
        destination = self.tmp / "out" / "preview"
        result = self._stage(build, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive = self.tmp / "out" / "preview-symbols.zip"
        self.assertIn(f"symbols {archive}", result.stdout)
        with zipfile.ZipFile(archive) as packed:
            self.assertEqual(sorted(packed.namelist()),
                             ["frost.exe", "frost.pdb", "frost_app.dll", "frost_app.pdb", "symbols.json"])
            self.assertEqual(packed.read("frost_app.pdb"), (build / "src/app/Debug/frost_app.pdb").read_bytes())
        shutil.rmtree(destination)
        refused = self._stage(build, destination)
        self.assertEqual(refused.returncode, 1, "an existing archive needs -Force like the folder")
        self.assertIn("preview-symbols.zip exists", refused.stdout)
        self.assertEqual(self._stage(build, destination, "-Force").returncode, 0)
        self.assertTrue(archive.is_file())

    def test_a_stale_pdb_fails_the_stage(self) -> None:
        build = self._fake_build()
        (build / "src/app/Debug/frost_app.pdb").write_bytes(symbol_fixtures.msf_bytes(_APP_GUID, dbi_age=2))
        destination = self.tmp / "stale"
        result = self._stage(build, destination)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("FAIL   frost_app.dll:", result.stdout)
        # The identities are checked in the build tree before the destination is created (item 7).
        self.assertIn("have no matching PDBs", result.stdout)
        self.assertFalse(destination.exists(), "no partial stage is left behind")
        self.assertFalse((self.tmp / "stale-symbols.zip").exists())

    def _stage_with_failing_deploy(self, build: Path, destination: Path) -> subprocess.CompletedProcess:
        """Stages with a windeployqt that fails (step 4, after FrostIDE's files are copied)."""
        deploy = self.tmp / "failing-deploy.cmd"
        deploy.write_text("@echo off\r\necho deploy failed\r\nexit /b 3\r\n", encoding="ascii", newline="")
        return run_powershell(TOOLS / "stage.ps1", "-Destination", str(destination), "-Config", "Debug",
                              "-BuildDir", str(build), "-WinDeployQt", str(deploy), "-Python", sys.executable,
                              "-SkipBuild")

    def test_a_late_failure_removes_the_partial_stage(self) -> None:
        # windeployqt runs after FrostIDE's files are copied (step 4). The brackets check that the
        # cleanup takes the destination literally, not as a wildcard pattern.
        for name in ("late", "late[1]"):
            with self.subTest(destination=name):
                build = self._fake_build()
                destination = self.tmp / name
                result = self._stage_with_failing_deploy(build, destination)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("windeployqt failed (3)", result.stdout)
                self.assertIn(f"removed the partial {destination}", result.stdout)
                self.assertFalse(destination.exists(), "no partial stage is left behind")
                self.assertFalse((self.tmp / f"{name}-symbols.zip").exists())

    def test_a_late_failure_empties_an_existing_empty_destination(self) -> None:
        for name in ("given", "given[1]"):
            with self.subTest(destination=name):
                build = self._fake_build()
                destination = self.tmp / name
                destination.mkdir()
                result = self._stage_with_failing_deploy(build, destination)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("windeployqt failed (3)", result.stdout)
                self.assertIn(f"emptied the partial {destination}", result.stdout)
                self.assertTrue(destination.is_dir(), "a folder the caller made stays")
                self.assertEqual(list(destination.iterdir()), [], "but holds nothing of the failed stage")

    def test_an_unexpected_copy_error_removes_the_partial_stage(self) -> None:
        # An input held open without sharing makes Copy-Item fail with a terminating error after the
        # destination exists (steps 3 and 3b; the preflight only tests that these files exist, while
        # the PDB check reads the images, so none of them is locked here). The trap must clean up.
        luals = "sdk/luals-" + json.loads((TOOLS.parent / "sdk" / "sdk.lock.json").read_text(
            encoding="utf-8"))["windows"]["luals"]["version"] + "/main.lua"
        for locked, existed in (("build/src/app/Debug/project-host/ProjectHostVsProbe.exe.config", False),
                                ("build/src/app/Debug/resources/themes/dark.json", False),
                                (luals, False),
                                ("build/src/app/Debug/resources/themes/dark.json", True)):
            with self.subTest(locked=locked, existing_destination=existed):
                build = self._fake_build()
                arguments = self._fake_tools()
                destination = self.tmp / ("given-locked" if existed else "locked")
                if existed:
                    destination.mkdir()
                with _exclusively_open(self.tmp / locked):
                    result = self._stage(build, destination, *arguments)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("stage: FAIL unexpected error at line", result.stdout)
                self.assertIn(Path(locked).name, result.stdout)
                if existed:
                    self.assertTrue(destination.is_dir(), "a folder the caller made stays")
                    self.assertEqual(list(destination.iterdir()), [], "but holds nothing of the failed stage")
                else:
                    self.assertIn(f"removed the partial {destination}", result.stdout)
                    self.assertFalse(destination.exists(), "no partial stage is left behind")
                self.assertFalse((self.tmp / f"{destination.name}-symbols.zip").exists())
                if existed:
                    destination.rmdir()

    def test_input_checks_run_before_the_destination_is_touched(self) -> None:
        cases = {"no ProjectHost": "build/src/app/Debug/project-host/ProjectHostVsProbe.exe.config",
                 "no resources": "build/src/app/Debug/resources",
                 "no HTTPS backend": "build/src/app/Debug/tls/qschannelbackendd.dll",
                 "missing upstream libsodium copyright notice": "installed/x64-fixture/share/libsodium/copyright",
                 "no CMake cache": "build/CMakeCache.txt"}
        for message, missing in cases.items():
            with self.subTest(missing=missing):
                self._fake_build()
                path = self.tmp / missing
                shutil.rmtree(path) if path.is_dir() else path.unlink()
                destination = self.tmp / "preflight"
                result = self._stage(self.tmp / "build", destination)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(message, result.stdout)
                self.assertNotIn("partial", result.stdout, "the preflight fails before anything is created")
                self.assertFalse(destination.exists(), "nothing is staged")

    def test_the_staged_folder_holds_frost_qt_and_the_platform_plugin(self) -> None:
        build = self._fake_build()
        destination = self.tmp / "out" / "preview"
        result = self._stage(build, destination)
        self.assertTrue((destination / "unity" / "com.frostide.editor" / "package.json").is_file())
        self.assertTrue((destination / "unity" / "com.frostide.editor" / "Editor" / "FrostEditor.cs").is_file())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("stage: ok", result.stdout)
        for name in ("frost.exe", "frost_app.dll", "crashpad_handler.exe", "conpty.dll", "OpenConsole.exe", "frost.pdb", "Qt6Cored.dll",
                     "platforms/qwindowsd.dll", "platforms/qoffscreend.dll", "resources/themes/dark.json",
                     "tls/qschannelbackendd.dll",
                     "project-host/ProjectHostVsProbe.exe", "project-host/ProjectHostVsProbe.exe.config",
                     "LICENSE", "NOTICE", "licenses/tree-sitter-LICENSE"):
            self.assertTrue((destination / name).is_file(), name)
        self.assertEqual((destination / "tls/qschannelbackendd.dll").read_bytes(), b"MZ-TLS-fixture")
        self.assertFalse((destination / "generic").exists(), "optional Qt plugins must not be deployed")
        self.assertFalse((destination / "Qt6Svgd.dll").exists(), "SVG belongs to the icon baker only")
        self.assertEqual((destination / "licenses/libsodium-copyright.txt").read_bytes(),
                         b"Upstream ISC copyright fixture\n")

    def test_missing_upstream_sodium_notice_fails(self) -> None:
        build = self._fake_build()
        (self.tmp / "installed/x64-fixture/share/libsodium/copyright").unlink()
        result = self._stage(build, self.tmp / "missing-notice")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("missing upstream libsodium copyright notice", result.stdout)

    def test_release_requires_source_archive_before_staging(self) -> None:
        destination = self.tmp / "release"
        result = run_powershell(TOOLS / "stage.ps1", "-Destination", str(destination), "-Config", "Release",
                                "-SkipBuild")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("-QtSourceArchive is required", result.stdout)
        self.assertFalse(destination.exists())

    def test_missing_https_backend_fails(self) -> None:
        build = self._fake_build()
        (build / "src/app/Debug/tls/qschannelbackendd.dll").unlink()
        result = self._stage(build, self.tmp / "missing-tls")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no HTTPS backend", result.stdout)

    def test_build_helper_wrappers_are_not_in_the_payload(self) -> None:
        build = self._fake_build()
        wrapper = build / "vcpkg_installed/x64-windows-clangcl/share/vcpkg-make/wrappers/compile"
        wrapper.parent.mkdir(parents=True)
        wrapper.write_bytes(b"BUILD-ONLY-HELPER")
        destination = self.tmp / "no-build-tools"
        result = self._stage(build, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any(p.name in ("vcpkg-make", "wrappers", "compile") for p in destination.rglob("*")))

    def test_a_non_empty_destination_needs_force(self) -> None:
        build = self._fake_build()
        destination = self.tmp / "out"
        destination.mkdir()
        (destination / "old.txt").write_text("old", encoding="utf-8")
        refused = self._stage(build, destination)
        self.assertEqual(refused.returncode, 1, refused.stdout + refused.stderr)
        self.assertIn("is not empty", refused.stdout)
        self.assertTrue((destination / "old.txt").exists())
        replaced = self._stage(build, destination, "-Force")
        self.assertEqual(replaced.returncode, 0, replaced.stdout + replaced.stderr)
        self.assertFalse((destination / "old.txt").exists())
        self.assertTrue((destination / "frost.exe").is_file())

    def test_corrupted_staged_conpty_fails(self) -> None:
        build = self._fake_build()
        deploy = self.tmp / "tamper-deploy.cmd"
        deploy.write_text(_FAKE_WINDEPLOYQT.replace("exit /b 0", 'echo changed>"%DEST%\\conpty.dll"\nexit /b 0'),
                          encoding="ascii", newline="\r\n")
        result = run_powershell(TOOLS / "stage.ps1", "-Destination", str(self.tmp / "corrupt-out"),
                                "-Config", "Debug", "-BuildDir", str(build), "-WinDeployQt", str(deploy), "-SkipBuild")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("staged conpty.dll differs", result.stdout)

    def test_missing_conpty_pair_fails(self) -> None:
        for missing in ("conpty.dll", "OpenConsole.exe"):
            with self.subTest(missing=missing):
                build = self._fake_build()
                (build / "src" / "app" / "Debug" / missing).unlink()
                result = self._stage(build, self.tmp / "missing-out")
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("no " + missing, result.stdout)

    def test_missing_project_host_fails(self) -> None:
        build = self._fake_build()
        (build / "src" / "app" / "Debug" / "project-host" / "ProjectHostVsProbe.exe").unlink()
        result = self._stage(build, self.tmp / "missing-host")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no ProjectHost", result.stdout)

    def test_stale_project_host_dll_is_not_staged(self) -> None:
        build = self._fake_build()
        (build / "src" / "app" / "Debug" / "project-host" / "Microsoft.Build.dll").write_bytes(b"stale")
        destination = self.tmp / "host-out"
        result = self._stage(build, destination)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((destination / "project-host" / "Microsoft.Build.dll").exists())

    def test_a_missing_build_output_fails(self) -> None:
        build = self._fake_build()
        (build / "src" / "app" / "Debug" / "frost_app.dll").unlink()
        result = self._stage(build, self.tmp / "out")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("no frost_app.dll", result.stdout)

if __name__ == "__main__":
    unittest.main()
