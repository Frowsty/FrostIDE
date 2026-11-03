#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Restore the reviewed ManagedPart graph from a private offline feed into the native build's obj directory.
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from audit_inputs import digest

HERE = Path(__file__).resolve().parent
PROJECT_HASH = "4a9b2e66ffdf38ae38806d0cc99494b9f00f8faa450bfc912177e67f58b7e33a"


def require_marker() -> None:
    # NuGet uses the real Windows LocalAppData Known Folder even with an isolated profile.
    if os.name != "nt":
        return
    class Guid(ctypes.Structure):
        _fields_ = [("data1", wintypes.DWORD), ("data2", wintypes.WORD), ("data3", wintypes.WORD),
                    ("data4", ctypes.c_ubyte * 8)]
    identifier = Guid(0xF1B32785, 0x6FBA, 0x4FCF, (ctypes.c_ubyte * 8)(0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91))
    path = ctypes.c_wchar_p()
    if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(identifier), 0x4000, None, ctypes.byref(path)) != 0:
        raise ValueError("cannot inspect Windows LocalAppData")
    local = Path(path.value)
    ctypes.windll.ole32.CoTaskMemFree(path)
    if not (local / "NuGet" / "Migrations" / "1").is_file():
        raise ValueError("NuGet migration marker is absent; refusing to modify the user's profile")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="prepared netcoredbg source root")
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--sdk-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True, help="new native CMake build directory")
    args = parser.parse_args()
    if not all(p.is_absolute() for p in (args.source, args.cache, args.sdk_root, args.build_dir)):
        raise ValueError("all paths must be absolute")
    source, sdk = args.source.resolve(strict=True), args.sdk_root.resolve(strict=True)
    project = source / "src" / "managed" / "ManagedPart.csproj"
    # git apply writes native line endings; only the newline representation may differ.
    project_hash = hashlib.sha256(project.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    if project_hash != PROJECT_HASH or digest(project.parent / "packages.lock.json") != digest(HERE / "packages.lock.json"):
        raise ValueError("prepared project or lock has changed")
    subprocess.run([sys.executable, str(HERE / "audit_inputs.py"), "--cache", str(args.cache)], check=True)
    dotnet = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
    if not dotnet.is_file():
        raise ValueError("SDK dotnet executable is missing")
    require_marker()
    build = args.build_dir.resolve()
    build.mkdir(parents=False, exist_ok=False)
    scratch = build / "frost-restore"
    directories = {n: scratch / n for n in ("feed", "profile", "packages", "http", "plugins", "tmp")}
    for directory in directories.values():
        directory.mkdir(parents=True)
    manifest = json.loads((HERE / "inputs.json").read_text(encoding="utf-8-sig"))
    for package in manifest["packages"]:
        if package["file"].startswith("microsoft.codeanalysis."):
            copied = directories["feed"] / package["file"]
            shutil.copyfile(args.cache / package["file"], copied)
            if digest(copied) != package["sha256"]:
                raise ValueError(f"copied package changed: {package['file']}")
    config = ET.Element("configuration")
    sources = ET.SubElement(config, "packageSources")
    ET.SubElement(sources, "clear")
    ET.SubElement(sources, "add", key="offline", value=str(directories["feed"]))
    config_file = scratch / "NuGet.Config"
    ET.ElementTree(config).write(config_file, encoding="utf-8", xml_declaration=True)
    profile = directories["profile"]
    env = os.environ.copy()
    env.update({"HOME": str(profile), "USERPROFILE": str(profile), "APPDATA": str(profile / "appdata"),
                "LOCALAPPDATA": str(profile / "local"), "XDG_CONFIG_HOME": str(profile / "config"),
                "XDG_DATA_HOME": str(profile / "data"), "XDG_CACHE_HOME": str(profile / "cache"),
                "DOTNET_CLI_HOME": str(profile), "DOTNET_ROOT": str(sdk),
                "DOTNET_GENERATE_ASPNET_CERTIFICATE": "false", "DOTNET_CLI_TELEMETRY_OPTOUT": "1",
                "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH": "false", "NUGET_PACKAGES": str(directories["packages"]),
                "NUGET_HTTP_CACHE_PATH": str(directories["http"]), "NUGET_PLUGINS_CACHE_PATH": str(directories["plugins"]),
                "TMP": str(directories["tmp"]), "TEMP": str(directories["tmp"]), "TMPDIR": str(directories["tmp"])})
    env.pop("NUGET_PLUGIN_PATHS", None)
    version = subprocess.run([str(dotnet), "--version"], env=env, cwd=scratch, check=True, capture_output=True, text=True).stdout.strip()
    if version != manifest["dotnetSdkVersion"]:
        raise ValueError(f"expected SDK {manifest['dotnetSdkVersion']}, got {version}")
    obj = build / "src" / "obj"
    obj.mkdir(parents=True)
    subprocess.run([str(dotnet), "restore", str(project), "--locked-mode", "--configfile", str(config_file),
                    f"-p:BaseIntermediateOutputPath={obj}{os.sep}", "--nologo"], env=env, cwd=scratch, check=True)
    print(f"Locked offline restore complete: {obj}. No native build or install was performed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"managed-debug restore: {error}", file=sys.stderr)
        raise SystemExit(1)
