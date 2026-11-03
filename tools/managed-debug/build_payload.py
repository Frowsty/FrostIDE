#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Build a manifest-bound CoreCLR debugger and source-verifier payload from audited inputs.
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

from audit_inputs import digest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
PATCH_SHA256 = "6379dd5d3fd68f4bb2cbc5ba9252e43bc8650f84bb22c9790968a4396b87a50b"
HELPER_SHA256 = "cc562d800e8cdbd095827b1b17c1e8c863632490733a2c03bad1158cb2af58dd"
PROJECT_SHA256 = "e082701ffc9c2ab048498fa4acd319535114bb19ef6ccb73f5289c8a47654456"


def canonical_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cache", "runtime", "sdk-root", "llvm-root", "cmake", "ninja", "work", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=1, choices=(1, 2))
    args = parser.parse_args()
    paths = (args.cache, args.runtime, args.sdk_root, args.llvm_root, args.cmake, args.ninja, args.work, args.output)
    if not all(path.is_absolute() for path in paths):
        raise ValueError("all paths must be absolute")
    if args.work.exists() or args.output.exists() or args.work.is_symlink() or args.output.is_symlink():
        raise ValueError("work and output directories must be new")
    work_path, output_path = args.work.resolve(), args.output.resolve()
    if work_path == output_path or work_path in output_path.parents or output_path in work_path.parents:
        raise ValueError("work and output directories must not overlap")
    if platform.machine().lower() not in ("x86_64", "amd64") or (os.name != "nt" and sys.platform != "linux"):
        raise ValueError("only Windows and Linux x64 are supported")
    if digest(HERE / "dap-only.patch") != PATCH_SHA256:
        raise ValueError("debugger patch does not match the reviewed revision")
    helper = REPO / "integrations/managed-debug/PdbGuard"
    if canonical_hash(helper / "Program.cs") != HELPER_SHA256 or canonical_hash(helper / "PdbGuard.csproj") != PROJECT_SHA256:
        raise ValueError("source verifier does not match the reviewed revision")
    sdk, llvm = args.sdk_root.resolve(strict=True), args.llvm_root.resolve(strict=True)
    cmake, ninja = args.cmake.resolve(strict=True), args.ninja.resolve(strict=True)
    dotnet = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
    clang = llvm / "bin" / ("clang-cl.exe" if os.name == "nt" else "clang++")
    readobj = llvm / "bin" / ("llvm-readobj.exe" if os.name == "nt" else "llvm-readobj")
    for tool, expected in ((cmake, "4.4.5"), (ninja, "1.13.2"), (clang, "23.1.4")):
        version = subprocess.run([str(tool), "--version"], check=True, capture_output=True, text=True).stdout.splitlines()[0]
        if expected not in version:
            raise ValueError(f"unexpected tool version: {tool}: {version}")
    tools = (dotnet, clang, cmake, ninja, readobj)
    tool_hashes = {str(tool): digest(tool) for tool in tools}
    toolchain = REPO / "cmake/toolchains" / ("windows-clangcl.cmake" if os.name == "nt" else "linux-clang.cmake")
    recipes = (toolchain, REPO / "sdk/sdk.lock.json", HERE / "inputs.json", HERE / "packages.lock.json",
               HERE / "dap-only.patch", HERE / "audit_inputs.py", HERE / "prepare_source.py", HERE / "offline_restore.py",
               HERE / "stage_scratch.py", HERE / "build_pdb_guard.py", HERE / "build_payload.py")
    recipe_hashes = {str(path.relative_to(REPO)): canonical_hash(path) for path in recipes}
    work = args.work.resolve()
    work.mkdir(parents=False, exist_ok=False)
    source_parent, build = work / "source", work / "build"
    subprocess.run([sys.executable, str(HERE / "prepare_source.py"), "--cache", str(args.cache),
                    "--runtime-checkout", str(args.runtime), "--output", str(source_parent)], check=True)
    inputs = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    source = source_parent / ("netcoredbg-" + inputs["netcoredbg"]["commit"])
    subprocess.run([sys.executable, str(HERE / "offline_restore.py"), "--source", str(source), "--cache", str(args.cache),
                    "--sdk-root", str(sdk), "--build-dir", str(build)], check=True)
    private = build / "frost-restore"
    profile = private / "profile"
    env = os.environ.copy()
    env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                "LOCALAPPDATA": str(profile / "local"), "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"), "XDG_CACHE_HOME": str(profile / "cache"),
                "DOTNET_CLI_HOME": str(profile), "DOTNET_ROOT": str(sdk), "NUGET_PACKAGES": str(private / "packages"),
                "NUGET_HTTP_CACHE_PATH": str(private / "http"), "NUGET_PLUGINS_CACHE_PATH": str(private / "plugins"),
                "TMP": str(private / "tmp"), "TEMP": str(private / "tmp"), "TMPDIR": str(private / "tmp"),
                "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false", "FROST_LLVM_ROOT": str(llvm),
                "PATH": os.pathsep.join((str(llvm / "bin"), str(cmake.parent), str(ninja.parent), env.get("PATH", "")))})
    env.pop("NUGET_PLUGIN_PATHS", None)
    configure = [str(cmake), "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
                 f"-DCORECLR_DIR={args.runtime / 'src/coreclr'}", f"-DDOTNET_DIR={sdk}", f"-DDOTNETCLI={dotnet}",
                 "-DBUILD_TESTING=OFF", "-DINTEROP_DEBUGGING=OFF", "-DBUILD_MANAGED=ON", "-DDBGSHIM_DIR=",
                 "-DNCDB_DOTNET_STARTUP_HOOK="]
    subprocess.run(configure, env=env, cwd=work, check=True)
    subprocess.run([str(cmake), "--build", str(build), "--parallel", str(args.jobs)], env=env, cwd=work, check=True)
    stage = work / "stage"
    subprocess.run([sys.executable, str(HERE / "stage_scratch.py"), "--source", str(source), "--runtime", str(args.runtime),
                    "--build-dir", str(build), "--sdk-root", str(sdk), "--cache", str(args.cache), "--output", str(stage)], check=True)
    if os.name == "nt":
        expected_imports = {
            "netcoredbg.exe": {"wsock32.dll", "kernel32.dll", "oleaut32.dll"},
            "dbgshim.dll": {"kernel32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "version.dll",
                            "api-ms-win-crt-stdio-l1-1-0.dll", "api-ms-win-crt-string-l1-1-0.dll",
                            "api-ms-win-crt-runtime-l1-1-0.dll", "api-ms-win-crt-heap-l1-1-0.dll"},
        }
    else:
        expected_imports = {
            "netcoredbg": {"libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6"},
            "libdbgshim.so": {"ld-linux-x86-64.so.2", "libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6",
                               "libpthread.so.0", "librt.so.1", "libdl.so.2"},
        }
    system_imports = {}
    for name, expected in expected_imports.items():
        option = "--coff-imports" if os.name == "nt" else "--needed-libs"
        text = subprocess.run([str(readobj), option, str(stage / name)], env=env, check=True,
                              capture_output=True, text=True).stdout
        if os.name == "nt":
            imports = {item.lower() for item in re.findall(r"(?m)^\s*Name:\s*(\S+)\s*$", text)}
        else:
            block = re.search(r"NeededLibraries\s*\[([^]]*)\]", text)
            if not block:
                raise ValueError(f"cannot read system imports: {name}")
            imports = set(block.group(1).split())
        if imports != expected:
            raise ValueError(f"unexpected system imports for {name}: {sorted(imports)}")
        system_imports[name] = sorted(imports)
    guard = work / "guard"
    subprocess.run([sys.executable, str(HERE / "build_pdb_guard.py"), "--sdk-root", str(sdk), "--output", str(guard)], check=True)
    if canonical_hash(helper / "Program.cs") != HELPER_SHA256 or canonical_hash(helper / "PdbGuard.csproj") != PROJECT_SHA256:
        raise ValueError("source verifier changed during build")
    if any(digest(tool) != tool_hashes[str(tool)] for tool in tools) or any(
        canonical_hash(path) != recipe_hashes[str(path.relative_to(REPO))] for path in recipes
    ):
        raise ValueError("build tool or recipe changed during the build")
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    shutil.copytree(stage, output / "adapter")
    shutil.copytree(guard / "helper", output / "PdbGuard")
    files = {file.relative_to(output).as_posix(): digest(file) for file in sorted(output.rglob("*")) if file.is_file()}
    record = {"schema": 1, "purpose": "candidate-source-build", "rid": "win-x64" if os.name == "nt" else "linux-x64",
              "runtimeCommit": inputs["runtimeCommit"], "netcoredbgCommit": inputs["netcoredbg"]["commit"],
              "patchSha256": PATCH_SHA256, "helperSourceSha256": HELPER_SHA256,
              "sdkVersion": inputs["dotnetSdkVersion"], "systemImports": system_imports,
              "toolSha256": tool_hashes, "recipeSha256": recipe_hashes,
              "files": files}
    manifest = output / "manifest.json"
    manifest.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Candidate payload: {output}; manifest SHA-256: {digest(manifest)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"managed-debug payload: {error}", file=sys.stderr)
        raise SystemExit(1)
