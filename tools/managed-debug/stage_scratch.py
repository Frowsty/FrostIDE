#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Make a manifest-listed scratch stage for testing a reviewed CoreCLR debugger source build.
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from audit_inputs import digest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ROSLYN = {
    "Microsoft.CodeAnalysis.dll": "microsoft.codeanalysis.common.5.9.0.nupkg",
    "Microsoft.CodeAnalysis.CSharp.dll": "microsoft.codeanalysis.csharp.5.9.0.nupkg",
    "Microsoft.CodeAnalysis.Scripting.dll": "microsoft.codeanalysis.scripting.common.5.9.0.nupkg",
    "Microsoft.CodeAnalysis.CSharp.Scripting.dll": "microsoft.codeanalysis.csharp.scripting.5.9.0.nupkg",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new scratch stage, never an install directory")
    args = parser.parse_args()
    if not all(p.is_absolute() for p in (args.source, args.runtime, args.build_dir, args.sdk_root, args.cache, args.output)):
        raise ValueError("all paths must be absolute")
    subprocess.run([sys.executable, str(HERE / "audit_inputs.py"), "--cache", str(args.cache)], check=True)
    manifest = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    if os.name != "nt" and sys.platform != "linux":
        raise ValueError("only Windows and Linux x64 scratch stages are supported")
    git = shutil.which("git")
    if git is None:
        raise ValueError("git was not found")
    def git_output(*arguments: str, cwd: Path) -> str:
        return subprocess.run([git, *arguments], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()
    if git_output("rev-parse", "HEAD", cwd=args.runtime) != manifest["runtimeCommit"]:
        raise ValueError("runtime checkout is not at the pinned commit")
    if git_output("status", "--porcelain", "--ignored", "--untracked-files=all", cwd=args.runtime):
        raise ValueError("runtime checkout contains changes or untracked files")
    git_output("apply", "--reverse", "--ignore-space-change", "--check", str(HERE / "dap-only.patch"), cwd=args.source)
    cache_entries = {}
    for line in (args.build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if ":" in line and "=" in line and not line.startswith(("#", "//")):
            key, remainder = line.split(":", 1)
            cache_entries[key] = remainder.split("=", 1)[1]
    for key, expected in (("CMAKE_HOME_DIRECTORY", args.source), ("CORECLR_DIR", args.runtime / "src/coreclr"),
                          ("DOTNET_DIR", args.sdk_root)):
        if key not in cache_entries or Path(cache_entries[key]).resolve() != expected.resolve():
            raise ValueError(f"build cache {key} does not match the reviewed input")
    windows = os.name == "nt"
    exe = "netcoredbg.exe" if windows else "netcoredbg"
    shim = "dbgshim.dll" if windows else "libdbgshim.so"
    rid = "win-x64" if windows else "linux-x64"
    shim_archive = args.cache / f"microsoft.diagnostics.dbgshim.{rid}.10.0.750501.nupkg"
    build = args.build_dir / "src"
    source = args.source
    package_hashes = {item["file"]: item["sha256"] for item in manifest["packages"]}
    expected_assets: dict[str, str] = {}
    for dll, package in ROSLYN.items():
        archive_bytes = (args.cache / package).read_bytes()
        if hashlib.sha256(archive_bytes).hexdigest() != package_hashes[package]:
            raise ValueError(f"package changed after audit: {package}")
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as z:
            expected = hashlib.sha256(z.read(f"lib/net10.0/{dll}")).hexdigest()
        expected_assets[dll] = expected
        if digest(build / dll) != expected:
            raise ValueError(f"built Roslyn asset differs from audited package: {dll}")
    # This stage records source-built outputs; it is not yet a release or an installer.
    stage = args.output.resolve()
    stage.mkdir(parents=False, exist_ok=False)
    notices = stage / "notices"
    notices.mkdir()
    package_notices = notices / "packages"
    package_notices.mkdir()
    # Preserve each audited package's original licence and copyright metadata,
    # including the build-time analyzer even though its binary is not staged.
    for package in manifest["packages"]:
        archive_bytes = (args.cache / package["file"]).read_bytes()
        if hashlib.sha256(archive_bytes).hexdigest() != package["sha256"]:
            raise ValueError(f"package changed after audit: {package['file']}")
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as z:
            specs = [name for name in z.namelist() if name.endswith(".nuspec") and "/" not in name]
            if len(specs) != 1:
                raise ValueError(f"unexpected nuspec count: {package['file']}")
            (package_notices / (package["file"] + ".nuspec")).write_bytes(z.read(specs[0]))
    for name in (exe, "ManagedPart.dll", *ROSLYN):
        shutil.copyfile(build / name, stage / name)
    if not windows:
        (stage / exe).chmod(0o755)
    with zipfile.ZipFile(shim_archive) as z:
        (stage / shim).write_bytes(z.read(f"runtimes/{rid}/native/{shim}"))
    expected_shim = ("18730f1fe4b555ab8f062e4c14104ee64cac774c9277554e9cc560546abd22df" if windows else
                     "661698f7be43162f50b465576e059585713f6e715c6f2d5b9ba5e255e9a8deba")
    if digest(stage / shim) != expected_shim:
        raise ValueError("staged dbgshim does not match the reviewed RID asset")
    for dll, expected in expected_assets.items():
        if digest(stage / dll) != expected:
            raise ValueError(f"staged Roslyn asset changed: {dll}")
    notice_files = (
        (source / "LICENSE", "netcoredbg-MIT.txt", "6cd03b0de8299b0800f22b35ae842c931ded7684a2d1ba4f1d4188bab9b09a11"),
        (source / "third_party/json/LICENSE.MIT", "json-MIT.txt", "7b875bb7a4e28abb7956ea821b4b7d88ff6a2a4fabf261c3993730a365ec56dd"),
        (args.runtime / "LICENSE.TXT", "dotnet-MIT.txt", "cfc21f5e8bd655ae997eec916138b707b1d290b83272c02a95c9f821b8c87310"),
        (args.runtime / "THIRD-PARTY-NOTICES.TXT", "runtime-third-party.txt", "66f1d4e44973185519bb4aa8a9718eb22fc7af2cc532e3ae9cfc4c127ee7fc54"),
        (REPO / "third_party/managed-notices/diagnostics-third-party.txt", "diagnostics-third-party.txt", "8fd258379044804c3df612e2cd7ef045a0595329c59af259cc232cdd61d7c379"),
        (REPO / "third_party/managed-notices/roslyn-third-party.rtf", "roslyn-third-party.rtf", "a981c2217337e26a76acfae8405f0f09cddc4bd17ced6147862da4d81795da78"),
        (REPO / "third_party/managed-notices/roslyn-analyzers-third-party.txt", "roslyn-analyzers-third-party.txt", "d3541d7e650299bc9abfe9d0398d800a543666eb0058d1370fd22d05fee6d89c"),
    )
    for src, name, expected in notice_files:
        shutil.copyfile(src, notices / name)
        canonical = (notices / name).read_bytes().replace(b"\r\n", b"\n")
        if hashlib.sha256(canonical).hexdigest() != expected:
            raise ValueError(f"notice mismatch: {name}")
    files = {f.relative_to(stage).as_posix(): digest(f) for f in sorted(stage.rglob("*")) if f.is_file()}
    record = {"purpose": "scratch-only", "runtimeCommit": manifest["runtimeCommit"],
              "netcoredbgCommit": manifest["netcoredbg"]["commit"], "patchSha256": digest(HERE / "dap-only.patch"),
              "sdk": manifest["dotnetSdkVersion"], "rid": rid, "files": files}
    (stage / "scratch-manifest.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Created scratch stage {stage} with {len(files)} pinned or recorded files; not installed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"managed-debug scratch stage: {error}", file=sys.stderr)
        raise SystemExit(1)
