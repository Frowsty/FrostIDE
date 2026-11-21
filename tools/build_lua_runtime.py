#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Builds and stages the pinned Lua runtime from a local source archive using Clang.
"""Build/stage pinned standalone Lua with Clang. Authored recipe; never invoked implicitly.

Supply an already downloaded official source archive. No network, package installation, tests,
feature probes, guessed binary digest, or compiler fallback. Build commands have no wall-clock
cutoff. The fresh owned work directory is retained on errors; no recursive cleanup is performed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--coroutine-debugger", action="store_true",
                        help="build the opt-in coroutine tracker for the pinned standalone adapter profile")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    lock = json.loads((repo / "sdk/sdk.lock.json").read_text(encoding="utf-8"))
    pin = lock["sources"]["lua54"]
    for name in ("archive", "clang", "cmake", "ninja"):
        value = getattr(args, name)
        if not value.is_absolute() or not value.is_file():
            parser.error(f"--{name} must name an existing absolute file")
    resource_compiler = args.clang.parent / "llvm-rc.exe" if os.name == "nt" else None
    if resource_compiler is not None and not resource_compiler.is_file():
        parser.error("pinned Clang directory must contain llvm-rc.exe for the UTF-8 executable manifest")
    with args.archive.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != pin["sha256"]:
        parser.error("Lua source archive does not match sdk.lock source digest")
    if not args.work_dir.is_absolute() or not args.prefix.is_absolute():
        parser.error("work-dir and prefix must be absolute")
    if (args.work_dir.exists() or args.work_dir.is_symlink() or args.prefix.exists() or args.prefix.is_symlink()):
        parser.error("use fresh work-dir and prefix; existing artifacts are preserved")
    work, prefix = args.work_dir.resolve(), args.prefix.resolve()
    if work.is_relative_to(prefix) or prefix.is_relative_to(work):
        parser.error("work-dir and installed prefix must be separate trees")
    args.work_dir.mkdir(parents=True, exist_ok=False)
    root_name = "lua-" + pin["version"]
    with tarfile.open(args.archive, "r:gz") as archive:
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != root_name:
                raise ValueError("source archive member escapes the pinned source directory")
            if not (member.isfile() or member.isdir()):
                raise ValueError("source archive contains unsupported links/special objects")
        archive.extractall(args.work_dir / "source", filter="data")
    source = args.work_dir / "source" / root_name
    if args.coroutine_debugger:
        from lua_coroutine_runtime_patch import apply_coroutine_hooks
        apply_coroutine_hooks(source)
    configure = [str(args.cmake), "-S", str(repo / "tools/lua-runtime"), "-B", str(args.work_dir / "build"),
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=" + args.clang.as_posix(),
        "-DCMAKE_MAKE_PROGRAM=" + args.ninja.as_posix(), "-DLUA_SOURCE_DIR=" + source.as_posix(),
        "-DCMAKE_INSTALL_PREFIX=" + args.prefix.as_posix(),
        "-DFROST_PINNED_CLANG_VERSION=" + lock["windows" if os.name == "nt" else "linux"]["llvm"]["version"]]
    if args.coroutine_debugger:
        configure.append("-DFROST_LUA_COROUTINE_DEBUGGER=ON")
    if resource_compiler is not None:
        configure.append("-DCMAKE_RC_COMPILER=" + resource_compiler.as_posix())
    subprocess.run(configure, check=True)
    subprocess.run([str(args.cmake), "--build", str(args.work_dir / "build")], check=True)
    subprocess.run([str(args.cmake), "--install", str(args.work_dir / "build")], check=True)


if __name__ == "__main__":
    main()
