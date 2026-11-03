#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Verify and install one manifest-bound managed-debug payload into an existing application directory.
from __future__ import annotations

import argparse
import ctypes
import errno
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile

from audit_inputs import digest


def verify(payload: Path, expected_manifest: str) -> dict:
    if payload.is_symlink() or not payload.is_dir() or not re.fullmatch(r"[0-9a-fA-F]{64}", expected_manifest):
        raise ValueError("payload or expected manifest digest is invalid")
    manifest = payload / "manifest.json"
    if manifest.is_symlink() or digest(manifest) != expected_manifest.lower():
        raise ValueError("payload manifest digest does not match the supplied build record")
    record = json.loads(manifest.read_text(encoding="utf-8"))
    if record.get("schema") != 1 or record.get("purpose") != "candidate-source-build":
        raise ValueError("unsupported managed-debug payload manifest")
    for key, expected in (("runtimeCommit", "6aa72daa24f66c01c3d7b6a32519e9d21396deec"),
                          ("netcoredbgCommit", "9744e1f051866215611b8440c638042aa2aa2f72"),
                          ("patchSha256", "6379dd5d3fd68f4bb2cbc5ba9252e43bc8650f84bb22c9790968a4396b87a50b"),
                          ("helperSourceSha256", "cc562d800e8cdbd095827b1b17c1e8c863632490733a2c03bad1158cb2af58dd"),
                          ("sdkVersion", "10.0.403")):
        if record.get(key) != expected:
            raise ValueError(f"payload input pin mismatch: {key}")
    if os.name == "nt":
        imports = {"netcoredbg.exe": ["kernel32.dll", "oleaut32.dll", "wsock32.dll"],
                   "dbgshim.dll": sorted(("kernel32.dll", "advapi32.dll", "ole32.dll", "oleaut32.dll", "version.dll",
                       "api-ms-win-crt-stdio-l1-1-0.dll", "api-ms-win-crt-string-l1-1-0.dll",
                       "api-ms-win-crt-runtime-l1-1-0.dll", "api-ms-win-crt-heap-l1-1-0.dll"))}
    else:
        imports = {"netcoredbg": sorted(("libc.so.6", "libgcc_s.so.1", "libm.so.6", "libstdc++.so.6")),
                   "libdbgshim.so": sorted(("ld-linux-x86-64.so.2", "libc.so.6", "libgcc_s.so.1", "libm.so.6",
                                            "libstdc++.so.6", "libpthread.so.0", "librt.so.1", "libdl.so.2"))}
    if record.get("systemImports") != imports:
        raise ValueError("payload system-library imports do not match the audited platform set")
    tool_hashes = record.get("toolSha256")
    recipes = record.get("recipeSha256")
    if not isinstance(tool_hashes, dict) or len(tool_hashes) != 5 or not isinstance(recipes, dict) or len(recipes) != 11:
        raise ValueError("payload lacks the recorded build-tool or recipe digests")
    if any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
           for value in (*tool_hashes.values(), *recipes.values())):
        raise ValueError("payload contains invalid build-tool or recipe digests")
    files = record.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("payload manifest has no files")
    actual = set()
    for entry in payload.rglob("*"):
        if entry.is_symlink():
            raise ValueError("payload cannot contain links")
        if entry.is_file():
            actual.add(entry.relative_to(payload).as_posix())
        elif not entry.is_dir():
            raise ValueError("payload contains a non-regular entry")
    if actual != set(files) | {"manifest.json"}:
        raise ValueError("payload has missing or unlisted files")
    required = {"PdbGuard/PdbGuard.dll", "PdbGuard/PdbGuard.deps.json", "PdbGuard/PdbGuard.runtimeconfig.json",
                "adapter/ManagedPart.dll", "adapter/notices/netcoredbg-MIT.txt", "adapter/notices/json-MIT.txt",
                "adapter/notices/dotnet-MIT.txt", "adapter/notices/runtime-third-party.txt",
                "adapter/notices/diagnostics-third-party.txt", "adapter/notices/roslyn-third-party.rtf",
                "adapter/notices/roslyn-analyzers-third-party.txt", "adapter/scratch-manifest.json"}
    required.add("adapter/netcoredbg.exe" if os.name == "nt" else "adapter/netcoredbg")
    required.add("adapter/dbgshim.dll" if os.name == "nt" else "adapter/libdbgshim.so")
    required.update("adapter/" + name for name in ("Microsoft.CodeAnalysis.dll", "Microsoft.CodeAnalysis.CSharp.dll",
                    "Microsoft.CodeAnalysis.Scripting.dll", "Microsoft.CodeAnalysis.CSharp.Scripting.dll"))
    required.update("adapter/notices/packages/" + name + ".nupkg.nuspec" for name in (
        "microsoft.codeanalysis.common.5.9.0", "microsoft.codeanalysis.csharp.5.9.0",
        "microsoft.codeanalysis.csharp.scripting.5.9.0", "microsoft.codeanalysis.scripting.common.5.9.0",
        "microsoft.codeanalysis.analyzers.5.9.0-1.26328.17", "microsoft.diagnostics.dbgshim.win-x64.10.0.750501",
        "microsoft.diagnostics.dbgshim.linux-x64.10.0.750501"))
    if set(files) != required:
        raise ValueError("payload does not match the exact approved file list")
    for name, expected in files.items():
        path = PurePosixPath(name)
        if path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in path.parts):
            raise ValueError("payload manifest contains an unsafe path")
        if not isinstance(expected, str) or len(expected) != 64 or digest(payload.joinpath(*path.parts)) != expected:
            raise ValueError(f"payload file digest mismatch: {name}")
    return record


def rename_new(source: Path, destination: Path) -> None:
    # Windows os.rename refuses an existing destination. Linux renameat2 with
    # RENAME_NOREPLACE prevents replacing even an empty, concurrently created directory.
    if os.name == "nt":
        os.rename(source, destination)
        return
    libc = ctypes.CDLL(None, use_errno=True)
    rename = getattr(libc, "renameat2", None)
    if rename is None:
        raise OSError(errno.ENOTSUP, "renameat2 is unavailable; refusing a non-atomic install")
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(source), -100, os.fsencode(destination), 1) != 0:
        code = ctypes.get_errno()
        raise OSError(code, os.strerror(code), str(destination))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--application-dir", type=Path, required=True)
    args = parser.parse_args()
    if not args.payload.is_absolute() or not args.application_dir.is_absolute():
        raise ValueError("payload and application directory must be absolute")
    if args.payload.is_symlink() or args.application_dir.is_symlink():
        raise ValueError("payload and application directory cannot be links")
    payload = args.payload.resolve(strict=True)
    app = args.application_dir.resolve(strict=True)
    if not app.is_dir():
        raise ValueError("application directory does not exist")
    record = verify(payload, args.manifest_sha256)
    expected_rid = "win-x64" if os.name == "nt" else "linux-x64"
    if record.get("rid") != expected_rid:
        raise ValueError("payload platform does not match this installer")
    destination = app / "FrostManagedDebug"
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"managed debugger already exists: {destination}")
    # The temporary directory is uniquely owned and remains for inspection on failure.
    staging = Path(tempfile.mkdtemp(prefix=".frost-managed-debug-", dir=app))
    shutil.copytree(payload, staging / "payload", symlinks=False)
    verify(staging / "payload", args.manifest_sha256)
    rename_new(staging / "payload", destination)
    staging.rmdir()
    print(f"Installed verified candidate payload to {destination}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"managed-debug install: {error}", file=sys.stderr)
        raise SystemExit(1)
